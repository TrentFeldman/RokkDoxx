#include "screens.hpp"

#include <algorithm>
#include <cstdio>
#include <exception>
#include <fstream>

namespace rokkdoxx::tui {

namespace {

const char* kDim = "\x1b[2m";
const char* kRst = "\x1b[0m";
const char* kInv = "\x1b[7m";
const char* kBed = "\x1b[1;33m";
const char* kAir = "\x1b[36m";
const char* kHi = "\x1b[1;32m";

std::string hi(const std::string& s) { return std::string(kHi) + s + kRst; }
std::string dim(const std::string& s) { return std::string(kDim) + s + kRst; }

void status_line(const App& app, Frame& fr) {
    if (app.status.empty()) return;
    fr.line();
    fr.line("  " + app.status);
}

// ---------------------------------------------------------------------------
// parameters
// ---------------------------------------------------------------------------

enum Field {
    F_SEED, F_W, F_H, F_Y, F_CX, F_CZ, F_RADIUS, F_ORIENT, F_BACKEND, F_CKPT, F_COUNT
};

const char* field_name(int f) {
    switch (f) {
        case F_SEED: return "seed";
        case F_W: return "width";
        case F_H: return "height";
        case F_Y: return "Y layer";
        case F_CX: return "center X";
        case F_CZ: return "center Z";
        case F_RADIUS: return "radius (blocks)";
        case F_ORIENT: return "orientations";
        case F_BACKEND: return "backend";
        case F_CKPT: return "checkpoint file";
    }
    return "";
}

std::string backend_text(const App& app) {
    if (app.backend_idx < 0) return "auto  (" + app.backend_label + ")";
    const svc::BackendInfo& b = app.backends[static_cast<std::size_t>(app.backend_idx)];
    std::string s = b.id + "  " + b.label;
    if (b.units > 0) s += "  (" + std::to_string(b.units) + (b.is_gpu ? " CUs)" : " threads)");
    return s;
}

std::string field_value(const App& app, int f) {
    const Model& m = app.m;
    switch (f) {
        case F_SEED: return m.seed;
        case F_W: return std::to_string(m.w);
        case F_H: return std::to_string(m.h);
        case F_Y: return std::to_string(m.y);
        case F_CX: return m.cx;
        case F_CZ: return m.cz;
        case F_RADIUS: return m.radius;
        case F_ORIENT: return m.all_orient ? "all 8" : "exact";
        case F_BACKEND: return backend_text(app);
        case F_CKPT: return m.checkpoint.empty() ? "(off)" : m.checkpoint;
    }
    return "";
}

void draw_params(const App& app, Frame& fr) {
    fr.line(hi("RokkDoxx  \xc2\xb7  bedrock pattern search"));
    fr.line();
    fr.line("Parameters   (Up/Down/Tab move, type to edit, Del clears, Left/Right to change)");
    fr.line();
    for (int f = 0; f < F_COUNT; ++f) {
        const bool sel = (f == app.field);
        char buf[64];
        std::snprintf(buf, sizeof(buf), "  %s %-16s ", sel ? "\xe2\x96\xb6" : " ", field_name(f));
        fr.put(buf);
        if (sel) fr.put(kInv);
        fr.put(" " + field_value(app, f) + " ");
        if (sel) fr.put(kRst);
        fr.line();
    }
    fr.line();
    {
        const int y = app.m.y;
        char b[160];
        std::snprintf(b, sizeof(b), "  Y=%d  ->  P(bedrock) = %.2f   %s", y, bedrock_probability(y),
                      (y <= -64)   ? "(solid everywhere -- nothing to match)"
                      : (y >= -59) ? "(air everywhere -- nothing to match)"
                      : (y == -60) ? "(recommended: most detail per cell)"
                                   : "");
        fr.line(dim(b));
    }
    {
        const double n = search_candidates(app.m);
        char b[160];
        if (n < 0)
            std::snprintf(b, sizeof(b), "  search area: radius must be a non-negative integer");
        else
            std::snprintf(b, sizeof(b), "  search area: %.4g candidate origins", n);
        fr.line(dim(b));
    }
    if (!app.m.checkpoint.empty())
        fr.line(dim("  checkpoint: resumes automatically if the file matches this exact search"));
    fr.line();
    fr.line("  " + hi("Enter") + " edit pattern      " + hi("L") + " load file      " + hi("q") +
            " quit");
    status_line(app, fr);
}

void edit_text(std::string& s, int key, bool digits_only, bool allow_sign) {
    if (key == K_BACKSPACE) {
        if (!s.empty()) s.pop_back();
    } else if (key == K_DELETE) {
        s.clear();
    } else if (key >= 32 && key < 127) {
        const char c = static_cast<char>(key);
        if (digits_only) {
            if ((c >= '0' && c <= '9') || (allow_sign && c == '-' && s.empty())) s.push_back(c);
        } else if (s.size() < 240) {
            s.push_back(c);
        }
    }
}

void begin_prompt(App& app, const std::string& label, Screen ret, void (*done)(App&)) {
    app.prompt_label = label;
    app.prompt_buf.clear();
    app.prompt_return = ret;
    app.prompt_done = done;
    app.screen = Screen::prompt;
}

void do_load_pattern(App& app) {
    if (app.prompt_buf.empty()) return;
    svc::PatternFile pf;
    std::string err;
    if (svc::load_pattern_file(app.prompt_buf, pf, err)) {
        file_to_model(pf, app.m);
        app.gx = std::min(app.gx, app.m.w - 1);
        app.gy = std::min(app.gy, app.m.h - 1);
        app.status = "loaded " + app.prompt_buf;
    } else {
        app.status = "load failed: " + err;
    }
}

void cycle_backend(App& app, int dir) {
    // Positions: -1 (auto), 0 .. n-1.
    const int n = static_cast<int>(app.backends.size());
    int idx = app.backend_idx + dir;
    if (idx < -1) idx = n - 1;
    if (idx >= n) idx = -1;
    const std::string id =
        idx < 0 ? "auto" : app.backends[static_cast<std::size_t>(idx)].id;
    std::string err;
    if (select_backend(app, id, err)) {
        app.backend_idx = idx;
        app.status = "backend: " + app.backend_label;
    } else {
        app.status = "backend " + id + " unavailable: " + err;
    }
}

bool handle_params(App& app, int k) {
    Model& m = app.m;
    if (k == 'q' && app.field != F_SEED && app.field != F_CKPT) return false;
    if (k == K_ESC) return false;
    if (k == K_TAB || k == K_DOWN) {
        app.field = (app.field + 1) % F_COUNT;
        return true;
    }
    if (k == K_UP) {
        app.field = (app.field + F_COUNT - 1) % F_COUNT;
        return true;
    }
    if (k == K_HOME || k == K_PGUP) {
        app.field = 0;
        return true;
    }
    if (k == K_END || k == K_PGDN) {
        app.field = F_COUNT - 1;
        return true;
    }
    if (k == K_ENTER) {
        app.screen = Screen::grid;
        app.status.clear();
        return true;
    }
    if (k == 'L' && app.field != F_SEED && app.field != F_CKPT) {
        begin_prompt(app, "Load pattern file:", Screen::params, do_load_pattern);
        return true;
    }
    const int step = (k == K_LEFT) ? -1 : (k == K_RIGHT) ? 1 : 0;
    switch (app.field) {
        case F_SEED: edit_text(m.seed, k, false, false); break;
        case F_W: m.w = std::clamp(m.w + step, 1, kMaxDim); break;
        case F_H: m.h = std::clamp(m.h + step, 1, kMaxDim); break;
        case F_Y: m.y = std::clamp(m.y + step, -64, -59); break;
        case F_CX: edit_text(m.cx, k, true, true); break;
        case F_CZ: edit_text(m.cz, k, true, true); break;
        case F_RADIUS: edit_text(m.radius, k, true, false); break;
        case F_ORIENT:
            if (step != 0 || k == ' ') m.all_orient = !m.all_orient;
            break;
        case F_BACKEND:
            if (step != 0) cycle_backend(app, step);
            break;
        case F_CKPT: edit_text(m.checkpoint, k, false, false); break;
    }
    app.gx = std::min(app.gx, m.w - 1);
    app.gy = std::min(app.gy, m.h - 1);
    return true;
}

// ---------------------------------------------------------------------------
// pattern grid
// ---------------------------------------------------------------------------

constexpr int kGridChromeRows = 11;  // header, col index, summary, help, status
constexpr int kRowLabelCols = 6;

// Keep the cursor inside a viewport of vis_rows x vis_cols.
void scroll_to_cursor(App& app, int vis_rows, int vis_cols) {
    auto fit = [](int& start, int cur, int vis, int total) {
        if (cur < start) start = cur;
        if (cur >= start + vis) start = cur - vis + 1;
        start = std::clamp(start, 0, std::max(0, total - vis));
    };
    fit(app.grid_top, app.gy, vis_rows, app.m.h);
    fit(app.grid_left, app.gx, vis_cols, app.m.w);
}

void draw_grid(App& app, Frame& fr) {
    const Model& m = app.m;
    long long r = 0, cx = 0, cz = 0;
    parse_i64(m.radius, r);
    parse_i64(m.cx, cx);
    parse_i64(m.cz, cz);

    const int vis_rows = std::max(1, std::min(m.h, fr.rows_left() - kGridChromeRows));
    const int vis_cols = std::max(1, std::min(m.w, fr.cols() - 1 - kRowLabelCols));
    scroll_to_cursor(app, vis_rows, vis_cols);

    int known = 0, needbed = 0;
    for (int j = 0; j < m.h; ++j)
        for (int i = 0; i < m.w; ++i) {
            if (m.at(i, j) != svc::Cell::unknown) ++known;
            if (m.at(i, j) == svc::Cell::bedrock) ++needbed;
        }

    char hdr[256];
    std::snprintf(hdr, sizeof(hdr), "Pattern  %dx%d  Y=%d  %s   seed %s   center (%lld,%lld) r=%lld",
                  m.w, m.h, m.y, m.all_orient ? "all-orient" : "exact", m.seed.c_str(), cx, cz, r);
    fr.line(hi(hdr));
    fr.line();

    fr.put(std::string(kRowLabelCols, ' '));
    for (int i = app.grid_left; i < app.grid_left + vis_cols; ++i) fr.put(dim(std::to_string(i % 10)));
    fr.line();
    for (int j = app.grid_top; j < app.grid_top + vis_rows; ++j) {
        char rb[16];
        std::snprintf(rb, sizeof(rb), "  %3d ", j);
        fr.put(dim(rb));
        for (int i = app.grid_left; i < app.grid_left + vis_cols; ++i) {
            const svc::Cell c = m.at(i, j);
            const char* col = c == svc::Cell::bedrock ? kBed : c == svc::Cell::not_bedrock ? kAir : kDim;
            const char* gl = c == svc::Cell::bedrock ? "#" : c == svc::Cell::not_bedrock ? "o" : ".";
            fr.put(col);
            if (i == app.gx && j == app.gy) fr.put(kInv);
            fr.put(gl);
            fr.put(kRst);
        }
        fr.line();
    }
    char sb[160];
    if (vis_rows < m.h || vis_cols < m.w) {
        // Uses the spacer row under the grid, so the chrome height is unchanged.
        std::snprintf(sb, sizeof(sb), "  view: rows %d-%d, cols %d-%d (window too small; it scrolls)",
                      app.grid_top, app.grid_top + vis_rows - 1, app.grid_left,
                      app.grid_left + vis_cols - 1);
        fr.line(dim(sb));
    } else {
        fr.line();
    }
    std::snprintf(sb, sizeof(sb), "  known cells: %d / %d      must be bedrock: %d", known, m.w * m.h,
                  needbed);
    fr.line(dim(sb));
    fr.line();
    fr.line("  " + hi("space") + " cycle  " + kBed + "#" + kRst + " bedrock  " + kAir + "o" + kRst +
            " not-bedrock  " + kDim + "." + kRst + " unknown   " + hi("Home/End/PgUp/PgDn") + " jump");
    fr.line("  " + hi("P") + " fill from world at center   " + hi("C") + " clear   " + hi("S") +
            " save");
    fr.line("  " + hi("Enter") + " run search   " + hi("Tab") + " parameters   " + hi("q") + " quit");
    status_line(app, fr);
}

void do_save_pattern(App& app) {
    if (app.prompt_buf.empty()) return;
    std::string err;
    app.status = svc::save_pattern_file(app.prompt_buf, model_to_file(app.m), err)
                     ? ("saved pattern -> " + app.prompt_buf)
                     : ("save failed: " + err);
}

void start_search(App& app) {
    svc::SearchRequest req;
    std::string err;
    app.matches.clear();
    app.result_scroll = 0;
    app.status.clear();
    app.screen = Screen::result;
    app.jst = {};
    if (!build_request(app.m, req, err)) {
        app.jst.state = svc::JobState::error;
        app.jst.error = err;
        return;
    }
    try {
        app.job = app.client->submit(req);
        app.job_running = true;
        app.jst = app.client->poll(app.job);
    } catch (const std::exception& e) {
        app.job_running = false;
        app.jst.state = svc::JobState::error;
        app.jst.error = e.what();
    }
}

bool handle_grid(App& app, int k) {
    Model& m = app.m;
    switch (k) {
        case 'q': return false;
        case K_TAB:
            app.screen = Screen::params;
            app.status.clear();
            return true;
        case K_UP: case 'k': app.gy = (app.gy + m.h - 1) % m.h; return true;
        case K_DOWN: case 'j': app.gy = (app.gy + 1) % m.h; return true;
        case K_LEFT: case 'h': app.gx = (app.gx + m.w - 1) % m.w; return true;
        case K_RIGHT: case 'l': app.gx = (app.gx + 1) % m.w; return true;
        case K_HOME: app.gx = 0; return true;
        case K_END: app.gx = m.w - 1; return true;
        case K_PGUP: app.gy = 0; return true;
        case K_PGDN: app.gy = m.h - 1; return true;
        case ' ': {
            svc::Cell& c = m.at(app.gx, app.gy);
            c = static_cast<svc::Cell>((static_cast<int>(c) + 1) % 3);
            return true;
        }
        case '#': case '1': m.at(app.gx, app.gy) = svc::Cell::bedrock; return true;
        case 'o': case '0': m.at(app.gx, app.gy) = svc::Cell::not_bedrock; return true;
        case '.': case 'x': case K_DELETE: m.at(app.gx, app.gy) = svc::Cell::unknown; return true;
        case 'C':
            m.clear();
            app.status = "cleared";
            return true;
        case 'P': {
            std::string err;
            app.status = fill_from_world(m, err)
                             ? "filled pattern from world at center (round-trip test)"
                             : err;
            return true;
        }
        case 'S':
            begin_prompt(app, "Save pattern to file:", Screen::grid, do_save_pattern);
            return true;
        case K_ENTER: start_search(app); return true;
    }
    return true;
}

// ---------------------------------------------------------------------------
// search / results
// ---------------------------------------------------------------------------

std::string progress_bar(double frac, int width) {
    frac = std::clamp(frac, 0.0, 1.0);
    const int fill = static_cast<int>(frac * width + 0.5);
    std::string s = "[";
    for (int i = 0; i < width; ++i) s += (i < fill) ? '#' : '-';
    s += "]";
    return s;
}

constexpr int kResultFooterRows = 5;

void draw_result(App& app, Frame& fr) {
    const svc::JobStatus& st = app.jst;
    fr.line(hi("Search") + dim("   backend: " + app.backend_label));
    fr.line();

    if (st.state == svc::JobState::error) {
        fr.line(std::string(kAir) + "  error: " + st.error + kRst);
        fr.line();
        fr.line("  " + hi("Enter/Esc") + " back");
        return;
    }

    char b[256];
    std::snprintf(b, sizeof(b), "  %s  %5.1f%%   %s   %.0f M/s   %.1fs", to_string(st.state),
                  st.progress * 100.0, progress_bar(st.progress, 32).c_str(), st.rate / 1e6,
                  st.elapsed_s);
    fr.line(b);
    std::snprintf(b, sizeof(b), "  scanned %.4g / %.4g origins    matches: %llu%s",
                  static_cast<double>(st.candidates_done), static_cast<double>(st.candidates_total),
                  static_cast<unsigned long long>(st.matches), st.truncated ? "  (capped)" : "");
    fr.line(b);
    if (!app.m.checkpoint.empty()) fr.line(dim("  checkpoint: " + app.m.checkpoint));
    fr.line();

    if (app.job_running) {
        fr.line("  " + hi("c") + " cancel" +
                (app.m.checkpoint.empty() ? "" : dim("  (progress is kept in the checkpoint)")));
        status_line(app, fr);
        return;
    }

    const int total = static_cast<int>(app.matches.size());
    if (total > 0) fr.line(dim("  anchor cell (~ pattern centre) of each match:"));
    app.result_rows = std::max(1, fr.rows_left() - kResultFooterRows);
    const int rows = app.result_rows;
    app.result_scroll = std::clamp(app.result_scroll, 0, std::max(0, total - rows));
    const int start = app.result_scroll;
    for (int k = start; k < std::min(total, start + rows); ++k) {
        const svc::Match& mm = app.matches[static_cast<std::size_t>(k)];
        char lb[160];
        if (app.m.all_orient)
            std::snprintf(lb, sizeof(lb), "    x = %-11d  z = %-11d  %s", mm.x, mm.z,
                          orient_names(mm.orient_mask).c_str());
        else
            std::snprintf(lb, sizeof(lb), "    x = %-11d  z = %-11d", mm.x, mm.z);
        fr.line(lb);
    }
    if (total > rows) {
        char lb[96];
        std::snprintf(lb, sizeof(lb), "  [%d-%d of %d]  Up/Down/PgUp/PgDn/Home/End scroll", start + 1,
                      std::min(total, start + rows), total);
        fr.line(dim(lb));
    }
    fr.line();
    fr.line("  " + hi("S") + " save matches   " + hi("Enter/Esc") + " back to pattern");
    status_line(app, fr);
}

void do_save_matches(App& app) {
    if (app.prompt_buf.empty()) return;
    std::ofstream f(app.prompt_buf, std::ios::binary | std::ios::trunc);
    if (!f) {
        app.status = "save failed: cannot open " + app.prompt_buf;
        return;
    }
    f << matches_header(app.m) << format_matches(app.matches);
    app.status = f ? "saved " + std::to_string(app.matches.size()) + " matches -> " + app.prompt_buf
                   : "save failed: write error";
}

bool handle_result(App& app, int k) {
    if (k == 'c' && app.job_running) {
        cancel_job(app);
        app.status = "cancelling...";
        return true;
    }
    if (k == K_ENTER || k == K_ESC || k == 'q') {
        cancel_job(app);
        app.job_running = false;
        app.screen = Screen::grid;
        app.status.clear();
        return true;
    }
    if (app.job_running) return true;
    const int page = app.result_rows;
    switch (k) {
        case K_UP: --app.result_scroll; break;
        case K_DOWN: ++app.result_scroll; break;
        case K_PGUP: app.result_scroll -= page; break;
        case K_PGDN: app.result_scroll += page; break;
        case K_HOME: app.result_scroll = 0; break;
        case K_END: app.result_scroll = static_cast<int>(app.matches.size()); break;
        case 'S':
            if (app.jst.state != svc::JobState::error)
                begin_prompt(app, "Save matches to file:", Screen::result, do_save_matches);
            break;
    }
    // draw_result clamps result_scroll to the list.
    app.result_scroll = std::max(0, app.result_scroll);
    return true;
}

// ---------------------------------------------------------------------------
// text prompt
// ---------------------------------------------------------------------------

void draw_prompt(const App& app, Frame& fr) {
    fr.line(hi(app.prompt_label));
    fr.line();
    fr.line("  " + std::string(kInv) + " " + app.prompt_buf + " " + kRst);
    fr.line();
    fr.line(dim("  Enter to confirm, Esc to cancel, Del to clear"));
}

bool handle_prompt(App& app, int k) {
    if (k == K_ESC) {
        app.screen = app.prompt_return;
        return true;
    }
    if (k == K_ENTER) {
        app.screen = app.prompt_return;
        if (app.prompt_done) app.prompt_done(app);
        return true;
    }
    edit_text(app.prompt_buf, k, false, false);
    return true;
}

}  // namespace

bool select_backend(App& app, const std::string& id, std::string& err) {
    try {
        auto c = svc::make_client(id);
        app.backend_label = c->backend_name();
        app.client = std::move(c);
        return true;
    } catch (const std::exception& e) {
        err = e.what();
        return false;
    }
}

void draw(App& app, Frame& fr) {
    switch (app.screen) {
        case Screen::params: draw_params(app, fr); break;
        case Screen::grid: draw_grid(app, fr); break;
        case Screen::result: draw_result(app, fr); break;
        case Screen::prompt: draw_prompt(app, fr); break;
    }
}

bool handle_key(App& app, int key) {
    switch (app.screen) {
        case Screen::params: return handle_params(app, key);
        case Screen::grid: return handle_grid(app, key);
        case Screen::result: return handle_result(app, key);
        case Screen::prompt: return handle_prompt(app, key);
    }
    return true;
}

void refresh_job(App& app) {
    if (!app.job_running) return;
    try {
        app.jst = app.client->poll(app.job);
        if (app.jst.state == svc::JobState::done || app.jst.state == svc::JobState::cancelled ||
            app.jst.state == svc::JobState::error) {
            app.matches = app.client->results(app.job);
            app.job_running = false;
            if (app.jst.state == svc::JobState::cancelled) app.status = "cancelled";
        }
    } catch (const std::exception& e) {
        app.job_running = false;
        app.jst.state = svc::JobState::error;
        app.jst.error = e.what();
    }
}

void cancel_job(App& app) {
    if (!app.job_running || !app.client) return;
    try {
        app.client->cancel(app.job);
    } catch (...) {
    }
}

}  // namespace rokkdoxx::tui
