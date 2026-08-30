// rokktui -- interactive front-end (client) for the RokkDoxx search service.
//
//   * enter a world seed (numeric, or a text seed hashed the way Minecraft does)
//   * choose the pattern size (width x height), the Y layer, and orientations
//   * paint the bedrock pattern on a grid (bedrock / not-bedrock / unknown)
//   * submit it to the service and watch progress; list every matching origin
//
// The search itself lives in librokksvc (SearchService + a compute Worker). This
// process only renders and talks to a SearchClient -- in-process by default, or
// a rokkd daemon via `--service unix:/path`.
#include <termios.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "gen/bedrock.hpp"
#include "svc/client.hpp"
#include "svc/pattern_io.hpp"
#include "svc/service_types.hpp"

namespace {

namespace svc = rokkdoxx::svc;

// ---------------------------------------------------------------------------
// terminal raw mode
// ---------------------------------------------------------------------------

termios g_orig{};
bool g_raw = false;

void restore_terminal() {
    if (!g_raw) return;
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &g_orig);
    std::fputs("\x1b[?25h\x1b[?1049l", stdout);
    std::fflush(stdout);
    g_raw = false;
}

void on_signal(int sig) {
    restore_terminal();
    std::signal(sig, SIG_DFL);
    std::raise(sig);
}

void enter_raw() {
    tcgetattr(STDIN_FILENO, &g_orig);
    termios r = g_orig;
    r.c_lflag &= ~(ECHO | ICANON | IEXTEN);
    r.c_iflag &= ~(IXON | ICRNL | BRKINT | INPCK | ISTRIP);
    r.c_oflag &= ~OPOST;
    r.c_cflag |= CS8;
    r.c_cc[VMIN] = 0;
    r.c_cc[VTIME] = 1;
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &r);
    g_raw = true;
    std::atexit(restore_terminal);
    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);
    std::fputs("\x1b[?1049h\x1b[?25l", stdout);
    std::fflush(stdout);
}

enum Key : int {
    K_NONE = -1, K_UP = -2, K_DOWN = -3, K_LEFT = -4, K_RIGHT = -5,
    K_ENTER = -6, K_BACKSPACE = -7, K_ESC = -8, K_TAB = -9
};

int read_key() {
    unsigned char c;
    if (read(STDIN_FILENO, &c, 1) <= 0) return K_NONE;
    if (c == '\r' || c == '\n') return K_ENTER;
    if (c == '\t') return K_TAB;
    if (c == 127 || c == 8) return K_BACKSPACE;
    if (c == 27) {
        unsigned char s0, s1;
        if (read(STDIN_FILENO, &s0, 1) <= 0) return K_ESC;
        if (read(STDIN_FILENO, &s1, 1) <= 0) return K_ESC;
        if (s0 == '[') {
            if (s1 == 'A') return K_UP;
            if (s1 == 'B') return K_DOWN;
            if (s1 == 'C') return K_RIGHT;
            if (s1 == 'D') return K_LEFT;
            if (s1 == '3') { unsigned char t; (void)read(STDIN_FILENO, &t, 1); return 127; }
        }
        return K_ESC;
    }
    return static_cast<int>(c);
}

// ---------------------------------------------------------------------------
// editable model
// ---------------------------------------------------------------------------

constexpr int kMaxDim = 32;
enum Cell : std::int8_t { UNKNOWN = 0, BEDROCK = 1, NOT_BEDROCK = 2 };

struct Model {
    std::string seed = "0";
    int w = 8, h = 8;
    int y = -60;
    std::string cx = "0", cz = "0", radius = "5000";
    bool all_orient = true;
    std::int8_t grid[kMaxDim][kMaxDim] = {};
};

bool parse_i64(const std::string& s, long long& out) {
    try {
        std::size_t pos = 0;
        out = std::stoll(s, &pos);
        return pos == s.size();
    } catch (...) {
        return false;
    }
}

svc::PatternFile model_to_file(const Model& m) {
    svc::PatternFile pf;
    pf.seed = m.seed;
    pf.y = m.y;
    pf.center_x = m.cx;
    pf.center_z = m.cz;
    pf.radius = m.radius;
    pf.all_orientations = m.all_orient;
    pf.pattern.w = m.w;
    pf.pattern.h = m.h;
    pf.pattern.cells.assign(static_cast<std::size_t>(m.w) * m.h, svc::Cell::unknown);
    for (int j = 0; j < m.h; ++j)
        for (int i = 0; i < m.w; ++i)
            pf.pattern.cells[static_cast<std::size_t>(j) * m.w + i] =
                static_cast<svc::Cell>(m.grid[j][i]);
    return pf;
}

void file_to_model(const svc::PatternFile& pf, Model& m) {
    m.seed = pf.seed;
    m.y = pf.y;
    m.cx = pf.center_x;
    m.cz = pf.center_z;
    m.radius = pf.radius;
    m.all_orient = pf.all_orientations;
    m.w = std::clamp(pf.pattern.w, 1, kMaxDim);
    m.h = std::clamp(pf.pattern.h, 1, kMaxDim);
    std::memset(m.grid, 0, sizeof(m.grid));
    for (int j = 0; j < m.h; ++j)
        for (int i = 0; i < m.w; ++i)
            m.grid[j][i] = static_cast<std::int8_t>(pf.pattern.at(i, j));
}

// ---------------------------------------------------------------------------
// rendering
// ---------------------------------------------------------------------------

std::string g_frame;
void put(const std::string& s) { g_frame += s; }
void line(const std::string& s = "") {
    g_frame += s;
    g_frame += "\r\n";
}
void flush_frame() {
    std::string out = "\x1b[H\x1b[2J";
    out += g_frame;
    (void)!write(STDOUT_FILENO, out.data(), out.size());
    g_frame.clear();
}

const char* kDim = "\x1b[2m";
const char* kRst = "\x1b[0m";
const char* kInv = "\x1b[7m";
const char* kBed = "\x1b[1;33m";
const char* kAir = "\x1b[36m";
const char* kHi = "\x1b[1;32m";

enum Screen { S_PARAMS, S_GRID, S_RESULT, S_TEXT };

struct Ui {
    Model m;
    Screen screen = S_PARAMS;
    int field = 0;
    int gx = 0, gy = 0;
    std::string status;

    // service
    std::unique_ptr<svc::SearchClient> client;
    std::string backend_label;
    svc::JobId job = 0;
    bool job_running = false;
    svc::JobStatus jst;
    std::vector<svc::Match> matches;
    int result_scroll = 0;

    std::string prompt_label, prompt_buf;
    Screen prompt_return = S_GRID;
    void (*prompt_done)(Ui&) = nullptr;
};

constexpr int kFields = 8;
const char* field_name(int f) {
    switch (f) {
        case 0: return "seed";
        case 1: return "width";
        case 2: return "height";
        case 3: return "Y layer";
        case 4: return "center X";
        case 5: return "center Z";
        case 6: return "radius (blocks)";
        case 7: return "orientations";
    }
    return "";
}
std::string field_value(const Model& m, int f) {
    switch (f) {
        case 0: return m.seed;
        case 1: return std::to_string(m.w);
        case 2: return std::to_string(m.h);
        case 3: return std::to_string(m.y);
        case 4: return m.cx;
        case 5: return m.cz;
        case 6: return m.radius;
        case 7: return m.all_orient ? "all 8" : "exact";
    }
    return "";
}

void draw_params(const Ui& u) {
    line(std::string(kHi) + "RokkDoxx  \xc2\xb7  bedrock pattern search" + kRst);
    line(std::string(kDim) + "  backend: " + u.backend_label + kRst);
    line();
    line("Parameters   (Up/Down or Tab to move, type to edit, Left/Right for width/height/Y/orient)");
    line();
    for (int f = 0; f < kFields; ++f) {
        const bool sel = (f == u.field);
        char buf[128];
        std::snprintf(buf, sizeof(buf), "  %s %-16s ", sel ? "\xe2\x96\xb6" : " ", field_name(f));
        put(buf);
        if (sel) put(kInv);
        put(" " + field_value(u.m, f) + " ");
        if (sel) put(kRst);
        line();
    }
    line();
    {
        const int span = u.m.y + 64;
        const double p = (u.m.y <= -64) ? 1.0 : (u.m.y >= -59) ? 0.0 : 1.0 - span / 5.0;
        char b[160];
        std::snprintf(b, sizeof(b), "  Y=%d  ->  P(bedrock) = %.2f   %s", u.m.y, p,
                      (u.m.y <= -64)   ? "(solid everywhere -- nothing to match)"
                      : (u.m.y >= -59) ? "(air everywhere -- nothing to match)"
                      : (u.m.y == -60) ? "(recommended: most detail per cell)"
                                       : "");
        line(std::string(kDim) + b + kRst);
    }
    {
        long long r = 0;
        parse_i64(u.m.radius, r);
        const long long side = 2 * (r < 0 ? 0 : r) + 1;
        char b[160];
        std::snprintf(b, sizeof(b), "  search area: %lld x %lld = %.3g candidate origins", side, side,
                      static_cast<double>(side) * static_cast<double>(side));
        line(std::string(kDim) + b + kRst);
    }
    line();
    line(std::string(kHi) + "  Enter" + kRst + " edit pattern      " + kHi + "L" + kRst +
         " load file      " + kHi + "q" + kRst + " quit");
    if (!u.status.empty()) {
        line();
        line("  " + u.status);
    }
}

void draw_grid(const Ui& u) {
    long long r = 0, cx = 0, cz = 0;
    parse_i64(u.m.radius, r);
    parse_i64(u.m.cx, cx);
    parse_i64(u.m.cz, cz);

    int known = 0, needbed = 0;
    for (int j = 0; j < u.m.h; ++j)
        for (int i = 0; i < u.m.w; ++i) {
            if (u.m.grid[j][i] != UNKNOWN) ++known;
            if (u.m.grid[j][i] == BEDROCK) ++needbed;
        }

    char hdr[220];
    std::snprintf(hdr, sizeof(hdr),
                  "Pattern  %dx%d  Y=%d  %s   seed %s   center (%lld,%lld) r=%lld", u.m.w, u.m.h,
                  u.m.y, u.m.all_orient ? "all-orient" : "exact", u.m.seed.c_str(), cx, cz, r);
    line(std::string(kHi) + hdr + kRst);
    line();

    put("      ");
    for (int i = 0; i < u.m.w; ++i) put(std::string(kDim) + std::to_string(i % 10) + kRst);
    line();
    for (int j = 0; j < u.m.h; ++j) {
        char rb[16];
        std::snprintf(rb, sizeof(rb), "  %3d ", j);
        put(std::string(kDim) + rb + kRst);
        for (int i = 0; i < u.m.w; ++i) {
            const bool cur = (i == u.gx && j == u.gy);
            const std::int8_t c = u.m.grid[j][i];
            const char* col = c == BEDROCK ? kBed : c == NOT_BEDROCK ? kAir : kDim;
            const char* gl = c == BEDROCK ? "#" : c == NOT_BEDROCK ? "o" : ".";
            put(col);
            if (cur) put(kInv);
            put(gl);
            put(kRst);
        }
        line();
    }
    line();
    char sb[160];
    std::snprintf(sb, sizeof(sb), "  known cells: %d / %d      must be bedrock: %d", known,
                  u.m.w * u.m.h, needbed);
    line(std::string(kDim) + sb + kRst);
    line();
    line(std::string(kHi) + "  space" + kRst + " cycle  " + kBed + "#" + kRst + " bedrock  " + kAir +
         "o" + kRst + " not-bedrock  " + kDim + "." + kRst + " unknown");
    line("  " + std::string(kHi) + "P" + kRst + " fill from world at center   " + kHi + "C" + kRst +
         " clear   " + kHi + "S" + kRst + " save");
    line("  " + std::string(kHi) + "Enter" + kRst + " run search   " + kHi + "Tab" + kRst +
         " parameters   " + kHi + "q" + kRst + " quit");
    if (!u.status.empty()) {
        line();
        line("  " + u.status);
    }
}

std::string progress_bar(double frac, int width) {
    frac = std::clamp(frac, 0.0, 1.0);
    const int fill = static_cast<int>(frac * width + 0.5);
    std::string s = "[";
    for (int i = 0; i < width; ++i) s += (i < fill) ? '#' : '-';
    s += "]";
    return s;
}

void draw_result(const Ui& u) {
    const svc::JobStatus& st = u.jst;
    line(std::string(kHi) + "Search" + kRst);
    line();

    if (st.state == svc::JobState::error) {
        line(std::string(kAir) + "  error: " + st.error + kRst);
        line();
        line(std::string(kHi) + "  Enter/Esc" + kRst + " back");
        return;
    }

    char b[256];
    std::snprintf(b, sizeof(b), "  %s  %5.1f%%   %s   %.0f M/s   %.1fs", to_string(st.state),
                  st.progress * 100.0, progress_bar(st.progress, 32).c_str(), st.rate / 1e6,
                  st.elapsed_s);
    line(b);
    std::snprintf(b, sizeof(b), "  scanned %.4g / %.4g origins    matches: %llu%s",
                  static_cast<double>(st.candidates_done),
                  static_cast<double>(st.candidates_total),
                  static_cast<unsigned long long>(st.matches), st.truncated ? "  (capped)" : "");
    line(b);
    line();

    const bool finished =
        st.state == svc::JobState::done || st.state == svc::JobState::cancelled;
    if (finished) {
        const int rows = 14;
        const int total = static_cast<int>(u.matches.size());
        int start = std::clamp(u.result_scroll, 0, std::max(0, total - rows));
        for (int k = start; k < std::min(total, start + rows); ++k) {
            const svc::Match& m = u.matches[static_cast<std::size_t>(k)];
            char lb[96];
            if (u.m.all_orient)
                std::snprintf(lb, sizeof(lb), "    x = %-11d  z = %-11d  orient 0x%02x", m.x, m.z,
                              m.orient_mask);
            else
                std::snprintf(lb, sizeof(lb), "    x = %-11d  z = %-11d", m.x, m.z);
            line(lb);
        }
        if (total > rows) {
            char lb[80];
            std::snprintf(lb, sizeof(lb), "  [%d-%d of %d]  Up/Down scroll", start + 1,
                          std::min(total, start + rows), total);
            line(std::string(kDim) + lb + kRst);
        }
        line();
        line(std::string(kHi) + "  S" + kRst + " save matches   " + kHi + "Enter/Esc" + kRst +
             " back to pattern");
    } else {
        line(std::string(kHi) + "  c" + kRst + " cancel");
    }
    if (!u.status.empty()) {
        line();
        line("  " + u.status);
    }
}

void draw_text_prompt(const Ui& u) {
    line(std::string(kHi) + u.prompt_label + kRst);
    line();
    put("  ");
    put(kInv);
    put(" " + u.prompt_buf + " ");
    put(kRst);
    line();
    line();
    line(std::string(kDim) + "  Enter to confirm, Esc to cancel" + kRst);
}

void draw(const Ui& u) {
    switch (u.screen) {
        case S_PARAMS: draw_params(u); break;
        case S_GRID: draw_grid(u); break;
        case S_RESULT: draw_result(u); break;
        case S_TEXT: draw_text_prompt(u); break;
    }
    flush_frame();
}

// ---------------------------------------------------------------------------
// input
// ---------------------------------------------------------------------------

void edit_text_field(std::string& s, int key, bool digits_only, bool allow_sign) {
    if (key == K_BACKSPACE) {
        if (!s.empty()) s.pop_back();
    } else if (key >= 32 && key < 127) {
        const char c = static_cast<char>(key);
        if (digits_only) {
            if ((c >= '0' && c <= '9') || (allow_sign && c == '-' && s.empty())) s.push_back(c);
        } else if (s.size() < 240) {
            s.push_back(c);
        }
    }
}

void begin_prompt(Ui& u, const std::string& label, Screen ret, void (*done)(Ui&)) {
    u.prompt_label = label;
    u.prompt_buf.clear();
    u.prompt_return = ret;
    u.prompt_done = done;
    u.screen = S_TEXT;
}

void do_save_pattern(Ui& u) {
    if (u.prompt_buf.empty()) return;
    std::string err;
    u.status = svc::save_pattern_file(u.prompt_buf, model_to_file(u.m), err)
                   ? ("saved pattern -> " + u.prompt_buf)
                   : ("save failed: " + err);
}

void do_save_matches(Ui& u) {
    if (u.prompt_buf.empty()) return;
    FILE* f = std::fopen(u.prompt_buf.c_str(), "w");
    if (!f) {
        u.status = "save failed";
        return;
    }
    std::fprintf(f, "# rokkdoxx matches  seed=%s y=%d size=%dx%d\n", u.m.seed.c_str(), u.m.y, u.m.w,
                 u.m.h);
    for (const auto& mm : u.matches) std::fprintf(f, "%d %d %u\n", mm.x, mm.z, mm.orient_mask);
    std::fclose(f);
    u.status = "saved " + std::to_string(u.matches.size()) + " matches -> " + u.prompt_buf;
}

void do_load_pattern(Ui& u) {
    if (u.prompt_buf.empty()) return;
    svc::PatternFile pf;
    std::string err;
    if (svc::load_pattern_file(u.prompt_buf, pf, err)) {
        file_to_model(pf, u.m);
        u.gx = std::min(u.gx, u.m.w - 1);
        u.gy = std::min(u.gy, u.m.h - 1);
        u.status = "loaded " + u.prompt_buf;
    } else {
        u.status = "load failed: " + err;
    }
}

void fill_from_world(Ui& u) {
    long long cx = 0, cz = 0;
    if (!parse_i64(u.m.cx, cx) || !parse_i64(u.m.cz, cz)) {
        u.status = "set integer center X/Z first";
        return;
    }
    rokkdoxx::BedrockGenerator gen(svc::seed_from_string(u.m.seed));
    for (int j = 0; j < u.m.h; ++j)
        for (int i = 0; i < u.m.w; ++i)
            u.m.grid[j][i] = gen.is_bedrock_floor(static_cast<int>(cx + i), u.m.y,
                                                  static_cast<int>(cz + j))
                                 ? BEDROCK
                                 : NOT_BEDROCK;
    u.status = "filled pattern from world at center (round-trip test)";
}

bool build_request(const Model& m, svc::SearchRequest& req, std::string& err) {
    long long cx, cz, rad;
    if (!parse_i64(m.cx, cx) || !parse_i64(m.cz, cz) || !parse_i64(m.radius, rad) || rad < 0) {
        err = "center X/Z and radius must be integers (radius >= 0)";
        return false;
    }
    req.seed = svc::seed_from_string(m.seed);
    req.plane_y = m.y;
    req.pattern.w = m.w;
    req.pattern.h = m.h;
    req.pattern.cells.assign(static_cast<std::size_t>(m.w) * m.h, svc::Cell::unknown);
    bool any = false;
    for (int j = 0; j < m.h; ++j)
        for (int i = 0; i < m.w; ++i) {
            auto c = static_cast<svc::Cell>(m.grid[j][i]);
            req.pattern.cells[static_cast<std::size_t>(j) * m.w + i] = c;
            if (c != svc::Cell::unknown) any = true;
        }
    if (!any) {
        err = "pattern has no known cells -- paint some bedrock first";
        return false;
    }
    req.region = svc::Region::centered(cx, cz, rad);
    req.all_orientations = m.all_orient;
    req.match_cap = 1u << 20;
    return true;
}

void start_search(Ui& u) {
    svc::SearchRequest req;
    std::string err;
    if (!build_request(u.m, req, err)) {
        u.jst = {};
        u.jst.state = svc::JobState::error;
        u.jst.error = err;
        u.screen = S_RESULT;
        return;
    }
    u.matches.clear();
    u.result_scroll = 0;
    u.status.clear();
    u.screen = S_RESULT;
    try {
        u.job = u.client->submit(req);
        u.job_running = true;
        u.jst = u.client->poll(u.job);
    } catch (const std::exception& e) {
        u.job_running = false;
        u.jst = {};
        u.jst.state = svc::JobState::error;
        u.jst.error = e.what();
    }
}

void refresh_job(Ui& u) {
    if (!u.job_running) return;
    try {
        u.jst = u.client->poll(u.job);
        if (u.jst.state == svc::JobState::done || u.jst.state == svc::JobState::cancelled ||
            u.jst.state == svc::JobState::error) {
            u.matches = u.client->results(u.job);
            u.job_running = false;
        }
    } catch (const std::exception& e) {
        u.job_running = false;
        u.jst.state = svc::JobState::error;
        u.jst.error = e.what();
    }
}

bool handle_params(Ui& u, int k) {
    if (k == 'q' || k == K_ESC) return false;
    if (k == K_TAB || k == K_DOWN) {
        u.field = (u.field + 1) % kFields;
        return true;
    }
    if (k == K_UP) {
        u.field = (u.field + kFields - 1) % kFields;
        return true;
    }
    if (k == K_ENTER) {
        u.screen = S_GRID;
        u.status.clear();
        return true;
    }
    if (k == 'L') {
        begin_prompt(u, "Load pattern file:", S_PARAMS, do_load_pattern);
        return true;
    }
    switch (u.field) {
        case 0: edit_text_field(u.m.seed, k, false, false); break;
        case 1:
            if (k == K_LEFT) u.m.w = std::max(1, u.m.w - 1);
            else if (k == K_RIGHT) u.m.w = std::min(kMaxDim, u.m.w + 1);
            break;
        case 2:
            if (k == K_LEFT) u.m.h = std::max(1, u.m.h - 1);
            else if (k == K_RIGHT) u.m.h = std::min(kMaxDim, u.m.h + 1);
            break;
        case 3:
            if (k == K_LEFT) u.m.y = std::max(-64, u.m.y - 1);
            else if (k == K_RIGHT) u.m.y = std::min(-59, u.m.y + 1);
            break;
        case 4: edit_text_field(u.m.cx, k, true, true); break;
        case 5: edit_text_field(u.m.cz, k, true, true); break;
        case 6: edit_text_field(u.m.radius, k, true, false); break;
        case 7:
            if (k == K_LEFT || k == K_RIGHT || k == ' ') u.m.all_orient = !u.m.all_orient;
            break;
    }
    return true;
}

bool handle_grid(Ui& u, int k) {
    switch (k) {
        case 'q': return false;
        case K_TAB:
            u.screen = S_PARAMS;
            u.status.clear();
            return true;
        case K_UP: case 'k': u.gy = (u.gy + u.m.h - 1) % u.m.h; return true;
        case K_DOWN: case 'j': u.gy = (u.gy + 1) % u.m.h; return true;
        case K_LEFT: case 'h': u.gx = (u.gx + u.m.w - 1) % u.m.w; return true;
        case K_RIGHT: case 'l': u.gx = (u.gx + 1) % u.m.w; return true;
        case ' ': {
            std::int8_t& c = u.m.grid[u.gy][u.gx];
            c = static_cast<std::int8_t>((c + 1) % 3);
            return true;
        }
        case '#': case '1': u.m.grid[u.gy][u.gx] = BEDROCK; return true;
        case 'o': case '0': u.m.grid[u.gy][u.gx] = NOT_BEDROCK; return true;
        case '.': case 'x': u.m.grid[u.gy][u.gx] = UNKNOWN; return true;
        case 'C':
            std::memset(u.m.grid, 0, sizeof(u.m.grid));
            u.status = "cleared";
            return true;
        case 'P': fill_from_world(u); return true;
        case 'S': begin_prompt(u, "Save pattern to file:", S_GRID, do_save_pattern); return true;
        case K_ENTER: start_search(u); return true;
    }
    return true;
}

bool handle_result(Ui& u, int k) {
    const bool finished = !u.job_running;
    auto try_cancel = [&] {
        try {
            u.client->cancel(u.job);
        } catch (...) {
        }
    };
    if (k == 'c' && u.job_running) {
        try_cancel();
        u.status = "cancelling...";
        return true;
    }
    if (k == K_ENTER || k == K_ESC || k == 'q') {
        if (u.job_running) try_cancel();
        u.job_running = false;
        u.screen = S_GRID;
        u.status.clear();
        return true;
    }
    if (finished) {
        const int total = static_cast<int>(u.matches.size());
        if (k == K_UP) u.result_scroll = std::max(0, u.result_scroll - 1);
        if (k == K_DOWN) u.result_scroll = std::min(std::max(0, total - 14), u.result_scroll + 1);
        if (k == 'S' && u.jst.state != svc::JobState::error)
            begin_prompt(u, "Save matches to file:", S_RESULT, do_save_matches);
    }
    return true;
}

bool handle_text(Ui& u, int k) {
    if (k == K_ESC) {
        u.screen = u.prompt_return;
        return true;
    }
    if (k == K_ENTER) {
        u.screen = u.prompt_return;
        if (u.prompt_done) u.prompt_done(u);
        return true;
    }
    edit_text_field(u.prompt_buf, k, false, false);
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    Ui u;
    std::string service = "local", backend, load_path;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "-h" || a == "--help") {
            std::puts(
                "rokktui -- interactive bedrock pattern search (client)\n"
                "  --load FILE        preload a pattern file\n"
                "  --service TARGET   local (default) | unix:/path/to.sock\n"
                "  --backend ID       cpu | opencl:N | auto  (in-process only)\n"
                "Requires an interactive terminal. Controls are shown on screen.");
            return 0;
        }
        if (a == "--load" && i + 1 < argc) load_path = argv[++i];
        else if (a == "--service" && i + 1 < argc) service = argv[++i];
        else if (a == "--backend" && i + 1 < argc) backend = argv[++i];
    }

    if (!load_path.empty()) {
        svc::PatternFile pf;
        std::string err;
        if (!svc::load_pattern_file(load_path, pf, err)) {
            std::fprintf(stderr, "load failed: %s\n", err.c_str());
            return 1;
        }
        file_to_model(pf, u.m);
    }

    std::signal(SIGPIPE, SIG_IGN);
    try {
        u.client = svc::make_client(svc::parse_target(service, backend));
        u.backend_label = u.client->backend_name();
    } catch (const std::exception& e) {
        std::fprintf(stderr, "service error: %s\n", e.what());
        return 1;
    }

    if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO)) {
        std::fprintf(stderr, "rokktui needs an interactive terminal.\n");
        return 1;
    }

    enter_raw();
    draw(u);
    for (;;) {
        const int k = read_key();
        if (u.screen == S_RESULT && u.job_running) refresh_job(u);

        if (k == K_NONE) {
            if (u.screen == S_RESULT) draw(u);  // keep progress live
            continue;
        }
        bool keep = true;
        switch (u.screen) {
            case S_PARAMS: keep = handle_params(u, k); break;
            case S_GRID: keep = handle_grid(u, k); break;
            case S_RESULT: keep = handle_result(u, k); break;
            case S_TEXT: keep = handle_text(u, k); break;
        }
        if (!keep) break;
        draw(u);
    }
    restore_terminal();
    return 0;
}
