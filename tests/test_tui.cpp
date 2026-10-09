// rokktui logic tests -- no terminal involved: the Model and its conversions,
// a fill-from-world round trip through a real (CPU) search, the matches file
// format, and the Frame diff/clipping.
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <map>
#include <string>
#include <thread>
#include <vector>

#include "check.hpp"
#include "svc/client.hpp"
#include "tui/frame.hpp"
#include "tui/model.hpp"
#include "tui/screens.hpp"

using namespace rokkdoxx;
using namespace rokkdoxx::tui;

namespace {

void test_file_round_trip() {
    Model a;
    a.seed = "hello world";
    a.w = 5;
    a.h = 3;
    a.y = -61;
    a.cx = "-100";
    a.cz = "250";
    a.radius = "77";
    a.all_orient = false;
    a.edition = svc::Edition::bedrock;
    a.at(0, 0) = svc::Cell::bedrock;
    a.at(4, 2) = svc::Cell::not_bedrock;
    a.at(10, 10) = svc::Cell::bedrock;  // outside w x h: not part of the pattern

    Model b;
    file_to_model(model_to_file(a), b);
    check(b.seed == a.seed && b.w == 5 && b.h == 3 && b.y == -61 && b.edition == svc::Edition::bedrock,
          "round trip: header fields");
    check(b.cx == "-100" && b.cz == "250" && b.radius == "77" && !b.all_orient,
          "round trip: region + orientation");
    check(b.at(0, 0) == svc::Cell::bedrock && b.at(4, 2) == svc::Cell::not_bedrock &&
              b.at(1, 1) == svc::Cell::unknown,
          "round trip: cells");
    check(b.at(10, 10) == svc::Cell::unknown, "round trip: cells outside the pattern dropped");
}

void test_build_request_errors() {
    Model m;
    svc::SearchRequest req;
    std::string err;
    check(!build_request(m, req, err) && !err.empty(), "empty pattern is rejected");

    m.at(0, 0) = svc::Cell::bedrock;
    m.radius = "-5";
    check(!build_request(m, req, err), "negative radius is rejected");
    m.radius = "-1";
    check(build_request(m, req, err) && req.region.x0 == -svc::Region::kWorldBorder &&
              req.region.z1 == svc::Region::kWorldBorder - 1,
          "radius -1 searches the whole world");
    check(search_candidates(m) == 4.0 * svc::Region::kWorldBorder * svc::Region::kWorldBorder,
          "radius -1 area is the whole world");
    m.radius = "12x";
    check(!build_request(m, req, err), "junk radius is rejected");
    m.radius = "10";
    m.cx = "3";
    m.cz = "-4";
    m.checkpoint = "ck.txt";
    check(build_request(m, req, err), "valid model builds a request");
    check(req.region.x0 == -7 && req.region.x1 == 13 && req.region.z0 == -14 && req.region.z1 == 6,
          "request region is centred on (cx, cz)");
    check(req.checkpoint_path == "ck.txt", "checkpoint path is passed through");
    check(req.pattern.w == m.w && req.pattern.knowns().size() == 1, "request pattern matches");
}

// Paint from the world, then search for it: the spot we painted from must come
// back, reported (post Step 9) at the anchor cell -- somewhere inside the
// pattern's footprint.
void test_fill_and_find(bool all_orient, svc::Edition edition = svc::Edition::java) {
    Model m;
    m.edition = edition;
    m.seed = "0";
    m.w = 6;
    m.h = 6;
    m.y = -60;
    m.cx = "137";
    m.cz = "-251";
    m.radius = "48";
    m.all_orient = all_orient;
    std::string err;
    check(fill_from_world(m, err), "fill_from_world succeeds");

    svc::SearchRequest req;
    check(build_request(m, req, err), "filled model builds a request");
    // Centre the search elsewhere so the target isn't trivially at the centre.
    req.region = svc::Region::centered(150, -240, 48);

    auto client = svc::make_client("cpu");
    const svc::JobId id = client->submit(req);
    svc::JobStatus st;
    for (;;) {
        st = client->poll(id);
        if (st.state != svc::JobState::pending && st.state != svc::JobState::running) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    check(st.state == svc::JobState::done, "search finishes");
    bool found = false;
    for (const svc::Match& mm : client->results(id))
        if (mm.x >= 137 && mm.x < 137 + 6 && mm.z >= -251 && mm.z < -251 + 6 && (mm.orient_mask & 1))
            found = true;
    check(found, std::string(edition == svc::Edition::bedrock ? "bedrock " : "") +
                     (all_orient ? "all-8: painted spot is found (identity bit, anchor in footprint)"
                                 : "exact: painted spot is found (anchor in footprint)"));
}

void test_format_matches() {
    const std::vector<svc::Match> ms = {{10, -20, 1}, {-5, 300, 0x83}};
    check(format_matches(ms) == "10 -20 1\n-5 300 131\n", "matches format is rokksearch's 'x z mask'");
    check(format_matches({}).empty(), "no matches -> empty");
    check(orient_names(0x83) == "id r90 m+r270", "orientation names");
}

void test_clip_visible() {
    check(clip_visible("hello", 3) == "hel\x1b[0m", "plain text clipped");
    check(clip_visible("hi", 5) == "hi", "short text untouched");
    check(clip_visible("\x1b[1;32mabcdef\x1b[0m", 2) == "\x1b[1;32mab\x1b[0m",
          "escape codes don't count toward width");
    check(clip_visible("\xc2\xb7\xc2\xb7\xc2\xb7", 2) == "\xc2\xb7\xc2\xb7\x1b[0m",
          "a UTF-8 character counts once");
}

void test_frame_diff() {
    Frame fr;
    const TermSize sz{40, 10};
    fr.begin(sz);
    fr.line("alpha");
    fr.line("beta");
    const std::string first = fr.flush();
    check(first.find("\x1b[2J") != std::string::npos, "first flush repaints everything");

    fr.begin(sz);
    fr.line("alpha");
    fr.line("beta");
    check(fr.flush().empty(), "unchanged frame emits nothing");

    fr.begin(sz);
    fr.line("alpha");
    fr.line("gamma");
    const std::string d = fr.flush();
    check(d.find("gamma") != std::string::npos && d.find("alpha") == std::string::npos &&
              d.find("\x1b[2;1H") != std::string::npos,
          "only the changed row is rewritten");

    fr.begin(sz);
    fr.line("alpha");
    const std::string shrink = fr.flush();
    check(shrink == "\x1b[2;1H\x1b[0m\x1b[K", "a row that disappears is cleared");

    fr.begin(TermSize{50, 10});
    fr.line("alpha");
    check(fr.flush().find("\x1b[2J") != std::string::npos, "a resize repaints everything");

    fr.begin(TermSize{50, 2});
    fr.line("1");
    fr.line("2");
    fr.line("3");
    check(fr.flush().find('3') == std::string::npos, "rows past the bottom are dropped");
}

void test_format_blocks() {
    check(format_blocks(64) == "64" && format_blocks(4096) == "4096", "small counts are plain");
    check(format_blocks(820000) == "820k" && format_blocks(-12000) == "-12k", "thousands use k");
    check(format_blocks(-29999984) == "-30.0M" && format_blocks(1.5e6) == "1.5M", "millions use M");
}

// --- the search map ---------------------------------------------------------

// Split a Frame's bytes into screen rows (1-based) -> the text written there.
std::map<int, std::string> screen_rows(const std::string& bytes) {
    std::map<int, std::string> rows;
    std::size_t i = 0;
    while (i < bytes.size()) {
        if (bytes.compare(i, 2, "\x1b[") != 0) { ++i; continue; }
        std::size_t j = i + 2, k = j;
        while (k < bytes.size() && bytes[k] >= '0' && bytes[k] <= '9') ++k;
        if (bytes.compare(k, 3, ";1H") != 0) { i = j; continue; }
        const int row = std::atoi(bytes.substr(j, k - j).c_str());
        const std::size_t end = bytes.find("\x1b[0m\x1b[K", k + 3);
        rows[row] = bytes.substr(k + 3, end - (k + 3));
        i = end == std::string::npos ? bytes.size() : end;
    }
    return rows;
}

int count_of(const std::string& s, const std::string& needle) {
    int n = 0;
    for (std::size_t p = s.find(needle); p != std::string::npos; p = s.find(needle, p + 1)) ++n;
    return n;
}

// The map rows of a drawn screen (the ones framed by │).
std::vector<std::string> map_rows(const std::string& bytes) {
    std::vector<std::string> out;
    for (const auto& [row, text] : screen_rows(bytes))
        if (text.find("\xe2\x94\x82") != std::string::npos) out.push_back(text);
    return out;
}

// Just the map's rows, joined -- colour checks must not see the legend's samples.
std::string map_text(const std::string& bytes) {
    std::string out;
    for (const auto& r : map_rows(bytes)) out += r + "\n";
    return out;
}

svc::MapCell cell(svc::MapCell::Phase p, bool hit = false) { return {p, hit}; }

// A running job whose map is 3x3 (so, at side 6, each cell is drawn 2x2).
App running_app() {
    App app;
    app.screen = Screen::result;
    app.backend_label = "test";
    app.job_running = true;
    app.map_side = 6;
    app.job_region = svc::Region::centered(0, 0, 2999);  // 5999 blocks across
    app.jst.state = svc::JobState::running;
    app.jst.map_w = app.jst.map_h = 3;
    app.jst.map.assign(9, cell(svc::MapCell::pending));
    app.map_prev = app.jst.map;
    app.phase_since.assign(9, -1'000'000'000);
    app.hit_since.assign(9, -1'000'000'000);
    return app;
}

std::string render(App& app, int cols = 100, int rows = 40) {
    Frame fr;
    fr.begin(TermSize{cols, rows});
    draw(app, fr);
    return fr.flush();
}

const std::string kSearchA = "\x1b[1;93m#", kSearchB = "\x1b[33m#";
const std::string kPending = "\x1b[2m#", kDone = "\x1b[36m#", kHit = "\x1b[1;30;42m#";
const std::string kPop = "\x1b[1;96m#", kParty = "\x1b[1;95m#", kPartial = "\x1b[94m#";
const std::string kHitFlashA = "\x1b[1;30;102m#", kHitFlashB = "\x1b[1;97;42m#";

void test_map_layout() {
    App app = running_app();
    const auto rows = map_rows(render(app));
    check(rows.size() == 6, "a 3x3 map at side 6 is drawn 6 rows tall");
    bool shape = true;
    for (const auto& r : rows) shape = shape && count_of(r, "#") == 6;
    check(shape, "...and 6 hashes across");
    const std::string all = render(app);
    check(all.find("\xe2\x94\x8c") != std::string::npos && all.find("\xe2\x94\x98") != std::string::npos,
          "the map is framed");
    check(all.find("[#") == std::string::npos && all.find("----") == std::string::npos,
          "no classic progress bar while the map is up");
    check(all.find("x -2999 .. 2999") != std::string::npos, "the region is captioned");

    // Too small a window for the map: the old bar comes back instead.
    App small = running_app();
    const std::string s = render(small, 100, 12);
    check(map_rows(s).empty() && s.find("[---") != std::string::npos,
          "a window too short for the map falls back to the progress bar");
    App narrow = running_app();
    check(map_rows(render(narrow, 12, 40)).empty(), "a window too narrow for the map falls back too");

    // A classic 80x24 terminal: the map fits and nothing on the screen is clipped.
    App classic = running_app();
    classic.map_side = 10;
    const std::string c = render(classic, 80, 24);
    check(map_rows(c).size() == 9, "80x24: a 3x3 map at side 10 is 9 rows");
    check(c.find(" partial") != std::string::npos && c.find(" match") != std::string::npos,
          "80x24: the legend (down to its last word) is not clipped");
    check(c.find("blocks") != std::string::npos && c.find("c\x1b[0m cancel") != std::string::npos,
          "80x24: caption and the cancel hint are both on screen");
}

void test_map_colours() {
    App app = running_app();
    auto& m = app.jst.map;
    m[0] = cell(svc::MapCell::searching);
    m[1] = cell(svc::MapCell::done);
    m[2] = cell(svc::MapCell::done, true);
    m[3] = cell(svc::MapCell::partial);
    app.map_prev = m;  // nothing changed "just now": everything is steady state

    app.now_ms = 0;  // first half of the flash period
    std::string out = map_text(render(app));
    check(out.find(kSearchA) != std::string::npos && out.find(kSearchB) == std::string::npos,
          "searching: bright yellow in the first half of the flash");
    check(out.find(kDone) != std::string::npos, "done: steady cyan");
    check(out.find(kHit) != std::string::npos, "an old hit: steady green");
    check(out.find(kPending) != std::string::npos, "untouched cells are dim");
    check(out.find(kPartial) != std::string::npos, "a partly searched cell is blue (steady)");

    app.now_ms = 250;  // second half
    out = map_text(render(app));
    check(out.find(kSearchB) != std::string::npos && out.find(kSearchA) == std::string::npos,
          "searching: plain yellow in the second half (it pulses)");

    // A cell that just became a hit flashes green, then settles.
    app.hit_since[2] = 1000;
    app.now_ms = 1000;
    out = map_text(render(app));
    check(out.find(kHitFlashA) != std::string::npos && out.find(kHit) == std::string::npos,
          "a fresh hit flashes bright green");
    app.now_ms = 1250;
    out = map_text(render(app));
    check(out.find(kHitFlashB) != std::string::npos, "...and alternates to the other green");
    app.now_ms = 1000 + 3000;
    out = map_text(render(app));
    check(out.find(kHit) != std::string::npos && out.find(kHitFlashA) == std::string::npos &&
              out.find(kHitFlashB) == std::string::npos,
          "...then settles to steady green");

    // A cell that just finished blinks cyan, then settles.
    app.phase_since[1] = 5000;
    app.now_ms = 5000;
    out = map_text(render(app));
    check(out.find(kPop) != std::string::npos, "a cell that just finished pops bright cyan");
    app.now_ms = 5000 + 1000;
    out = map_text(render(app));
    check(out.find(kPop) == std::string::npos && out.find(kDone) != std::string::npos,
          "...then settles to cyan");
}

// A match found in an area that is still being searched must not hide the fact
// that the search is there: flash green on the find, then pulse yellow <-> green.
void test_map_searching_hit() {
    App app = running_app();
    app.jst.map[4] = cell(svc::MapCell::searching, true);
    app.map_prev = app.jst.map;
    app.hit_since[4] = 2000;

    app.now_ms = 2000;
    std::string out = map_text(render(app));
    check(out.find(kHitFlashA) != std::string::npos, "a hit in a cell being searched flashes green at once");
    check(out.find(kSearchA) == std::string::npos, "...while the find is fresh");

    app.now_ms = 2000 + 3000 + 0;  // flash over; this is an 'on' beat (5000/250 is even)
    out = map_text(render(app));
    check(out.find(kSearchA) != std::string::npos && out.find(kHit) == std::string::npos,
          "then it pulses: bright yellow on the on beat...");
    app.now_ms += 250;
    out = map_text(render(app));
    check(out.find(kHit) != std::string::npos && out.find(kSearchA) == std::string::npos,
          "...and steady green on the off beat (still being searched, already holds a match)");
}

void test_map_celebration() {
    App app = running_app();
    app.job_running = false;
    app.jst.state = svc::JobState::done;
    app.jst.progress = 1.0;
    app.jst.map.assign(9, cell(svc::MapCell::done));
    app.jst.map[4] = cell(svc::MapCell::done, true);
    app.map_prev = app.jst.map;
    app.finished_at_ms = 10'000;

    app.now_ms = 10'000;  // flash "on"
    std::string out = map_text(render(app));
    check(count_of(out, kParty) > 0, "completion: the map flashes magenta");
    check(out.find(kHitFlashA) != std::string::npos, "...while the hits flash green");
    app.now_ms = 10'250;  // flash "off"
    out = map_text(render(app));
    check(count_of(out, kParty) == 0 && out.find(kDone) != std::string::npos,
          "...and is back to steady cyan on the off beat");

    app.now_ms = 10'000 + 3500;  // the party is over; the matches take the map's place
    app.matches = {{1, 2, 1}};
    std::string raw = render(app);
    check(map_rows(raw).empty() && raw.find("x = 1") != std::string::npos,
          "after the flash the matches replace the map");
    handle_key(app, 'm');
    raw = render(app);
    check(map_rows(raw).size() == 6 && map_text(raw).find(kHit) != std::string::npos,
          "m brings the map back (hit still green)");
    handle_key(app, 'm');
    check(map_rows(render(app)).empty(), "m hides it again");

    // No matches: nothing to list, so the map stays.
    App none = running_app();
    none.job_running = false;
    none.jst.state = svc::JobState::done;
    none.jst.map.assign(9, cell(svc::MapCell::done));
    none.finished_at_ms = 0;
    none.now_ms = 60'000;
    check(map_rows(render(none)).size() == 6, "a search with no matches keeps its map");
}

// The whole path: Enter on the grid starts a real CPU search with a map, the
// polled map fills in, and the TUI's bookkeeping sees it finish.
void test_map_end_to_end() {
    App app;
    std::string err;
    check(select_backend(app, "cpu", err), "cpu backend opens");
    app.screen = Screen::grid;
    app.term_cols = 100;
    app.term_rows = 40;  // room for a 26-cell map
    app.m.w = app.m.h = 4;
    app.m.radius = "9000";  // 18001 blocks -> 5x5 tiles of 4096
    app.m.all_orient = false;
    for (int i = 0; i < 4; ++i) app.m.at(i, 0) = svc::Cell::bedrock;
    handle_key(app, K_ENTER);
    check(app.screen == Screen::result && app.job_running, "Enter starts the search");
    check(app.map_side == 26, "the map is sized from the terminal (40 rows - 14 chrome)");

    bool saw_map = false, changed = false;
    for (int i = 0; i < 2000 && app.job_running; ++i) {
        app.now_ms += 10;
        refresh_job(app);
        if (!app.jst.map.empty()) saw_map = true;
        for (long long s : app.phase_since) changed = changed || s >= 0;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    check(!app.job_running && app.jst.state == svc::JobState::done, "the search finishes");
    check(saw_map && app.jst.map_w == 5 && app.jst.map_h == 5, "the map comes back clamped to the 5x5 tile grid");
    check(changed, "cell changes are timestamped as they are seen");
    bool all_done = true;
    for (svc::MapCell c : app.jst.map) all_done = all_done && c.phase == svc::MapCell::done;
    check(all_done, "every cell ends done");
    check(app.finished_at_ms > 0, "the finish time is recorded for the completion flash");
    // 5x5 cells in a 26-wide space: each cell is drawn 5x5 hashes (25 across).
    check(map_rows(render(app)).size() == 25, "a coarse map is scaled up to fill its space");

    // A terminal too small for a map asks the service for none.
    App tiny;
    tiny.screen = Screen::grid;
    tiny.term_rows = 16;
    tiny.m.at(0, 0) = svc::Cell::bedrock;
    tiny.m.radius = "100";
    std::string e2;
    check(select_backend(tiny, "cpu", e2), "cpu backend opens (tiny)");
    handle_key(tiny, K_ENTER);
    check(tiny.map_side == 0, "a short terminal gets no map");
    for (int i = 0; i < 2000 && tiny.job_running; ++i) {
        refresh_job(tiny);
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    check(tiny.jst.map.empty(), "...and the service sends none");
}

// "stop at first match": a parameter on the params screen, carried into the request, and a
// result label when the job ended early.
void test_stop_first() {
    App app;
    app.screen = Screen::params;
    check(!app.m.stop_first, "off by default");
    auto selected_row = [&] {
        for (const auto& [row, text] : screen_rows(render(app)))
            if (text.find("\xe2\x96\xb6") != std::string::npos) return text;  // the selection arrow
        return std::string();
    };
    for (int i = 0; i < 20 && selected_row().find("stop at first") == std::string::npos; ++i)
        handle_key(app, K_DOWN);
    check(selected_row().find("stop at first") != std::string::npos, "the parameter is on the params screen");
    handle_key(app, K_RIGHT);
    check(app.m.stop_first && selected_row().find("yes") != std::string::npos, "Right turns it on");
    handle_key(app, ' ');
    check(!app.m.stop_first && selected_row().find("no") != std::string::npos, "space toggles it back");
    handle_key(app, ' ');

    app.m.at(0, 0) = svc::Cell::bedrock;
    svc::SearchRequest req;
    std::string err;
    check(build_request(app.m, req, err) && req.stop_at_first_match, "the request carries the flag");
    app.m.stop_first = false;
    check(build_request(app.m, req, err) && !req.stop_at_first_match, "...and the default does not stop early");

    App done = running_app();
    done.job_running = false;
    done.jst.state = svc::JobState::done;
    done.jst.stopped_early = true;
    done.jst.progress = 0.04;
    done.finished_at_ms = 0;
    done.now_ms = 100;
    check(render(done).find("found, stopped") != std::string::npos,
          "a job that stopped early says so instead of 'done'");
}

// The edition toggle: on the params screen and carried into the request. On
// Bedrock the seed row says it is unused, and -63 is solid there.
void test_edition_toggle() {
    App app;
    app.screen = Screen::params;
    auto selected_row = [&] {
        for (const auto& [row, text] : screen_rows(render(app)))
            if (text.find("\xe2\x96\xb6") != std::string::npos) return text;  // the selection arrow
        return std::string();
    };
    auto shown = [&](const std::string& s) { return render(app).find(s) != std::string::npos; };
    for (int i = 0; i < 20 && selected_row().find("edition") == std::string::npos; ++i) handle_key(app, K_DOWN);
    check(selected_row().find("Java") != std::string::npos, "the edition is on the params screen, Java by default");
    handle_key(app, K_RIGHT);
    check(app.m.edition == svc::Edition::bedrock && shown("not used on Bedrock"),
          "Right switches to Bedrock and marks the seed unused");
    app.m.y = -63;
    check(shown("solid everywhere"), "on Bedrock y -63 is all bedrock");
    app.m.at(0, 0) = svc::Cell::bedrock;
    svc::SearchRequest req;
    std::string err;
    check(build_request(app.m, req, err) && req.edition == svc::Edition::bedrock, "the request carries the edition");
    handle_key(app, ' ');
    check(app.m.edition == svc::Edition::java && !shown("not used on Bedrock"), "space switches back to Java");
}

// --- pause, save a session, continue later ---------------------------------

// A CPU search of ~1.5 s with one planted match, so there is time to pause in the middle.
void slow_search(App& app) {
    std::string err;
    select_backend(app, "cpu", err);
    app.screen = Screen::grid;
    app.term_cols = 100;
    app.term_rows = 40;
    app.m.seed = "99";
    app.m.w = app.m.h = 9;
    app.m.y = -60;
    app.m.cx = "5004";
    app.m.cz = "-6996";
    app.m.radius = "15000";
    app.m.all_orient = false;
    fill_from_world(app.m, err);  // fills at the model's centre: move it to the planted spot
    app.m.cx = "5000";
    app.m.cz = "-7000";
    fill_from_world(app.m, err);
    app.m.cx = "5004";
    app.m.cz = "-6996";
}

bool pump_until(App& app, const std::function<bool()>& done, double timeout_s = 30) {
    const auto t0 = std::chrono::steady_clock::now();
    while (!done()) {
        if (std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() > timeout_s) return false;
        app.now_ms += 10;
        refresh_job(app);
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return true;
}

void test_pause_save_resume() {
    std::printf("test_pause_save_resume\n");
    App app;
    slow_search(app);

    // The reference: the same search, uninterrupted.
    handle_key(app, K_ENTER);
    check(pump_until(app, [&] { return !app.job_running; }), "reference search finishes");
    const std::string want = format_matches(app.matches);
    check(!want.empty(), "...and finds the planted spot");

    // Live pause.
    handle_key(app, K_ENTER);  // back to the grid
    handle_key(app, K_ENTER);  // start again
    check(pump_until(app, [&] { return app.jst.progress > 0.15; }), "search is under way");
    handle_key(app, 'p');
    check(app.want_pause, "p starts pausing");
    check(pump_until(app, [&] { return app.jst.state == svc::JobState::paused; }), "the job reaches 'paused'");
    const std::string paused_screen = render(app);
    check(paused_screen.find("paused") != std::string::npos && paused_screen.find("resume") != std::string::npos,
          "the paused screen says so and offers resume");
    const long long frozen_at = app.jst.candidates_done;
    for (int i = 0; i < 40; ++i) {  // 0.2 s of polling
        app.now_ms += 10;
        refresh_job(app);
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    check(app.job_running && app.jst.candidates_done == frozen_at, "no progress while paused");

    // Save the session while paused (exact), then keep the files for the checks below.
    const std::string session = "test_session.tmp";
    std::remove(session.c_str());
    std::remove((session + ".ckpt").c_str());
    handle_key(app, 's');
    check(app.screen == Screen::prompt && app.prompt_buf == "session.txt", "s asks where to save (with a default)");
    app.prompt_buf = session;
    handle_key(app, K_ENTER);
    check(app.status.find("saved") != std::string::npos && app.status.find("--resume") != std::string::npos,
          "...and says how to continue");
    svc::PatternFile pf;
    std::string err;
    check(svc::load_pattern_file(session, pf, err) && pf.checkpoint == session + ".ckpt" &&
              pf.pattern.w == 9 && pf.seed == "99",
          "the session file is the pattern plus the progress file it names");
    check(std::ifstream(session + ".ckpt").good(), "the progress file exists");

    // Resume live, to the end.
    handle_key(app, 'p');
    check(pump_until(app, [&] { return !app.job_running; }), "resuming finishes the search");
    check(format_matches(app.matches) == want, "...with the results of the uninterrupted run");

    // Come back later: a brand new App picks the session up and finishes the rest.
    App later;
    later.term_cols = 100;
    later.term_rows = 40;
    std::string e2;
    select_backend(later, "cpu", e2);
    check(resume_session(later, session, e2), "resume_session accepts the saved session: " + e2);
    check(later.m.seed == "99" && later.m.w == 9 && later.screen == Screen::result && later.job_running,
          "it restores the search and starts it");
    check(pump_until(later, [&] { return !later.job_running; }), "the resumed session finishes");
    check(format_matches(later.matches) == want, "...with the same results");
    // It started from the saved progress, not from zero: rate x elapsed = what this run searched.
    check(later.jst.rate * later.jst.elapsed_s < 0.97 * static_cast<double>(later.jst.candidates_total),
          "...having searched only the part that was left");

    // Not a session / missing progress file are refused with a reason.
    std::string e3;
    App x;
    std::ofstream("test_plain.tmp") << "# rokkdoxx pattern\nseed 1\nsize 1 1\n#\n";
    check(!resume_session(x, "test_plain.tmp", e3) && e3.find("plain pattern") != std::string::npos,
          "a plain pattern file is not a session");
    std::remove((session + ".ckpt").c_str());
    check(!resume_session(x, session, e3) && e3.find("missing") != std::string::npos, "a missing progress file is reported");
    std::remove(session.c_str());
    std::remove("test_plain.tmp");
}

void test_continue_after_stop() {
    std::printf("test_continue_after_stop\n");
    App app;
    slow_search(app);
    handle_key(app, K_ENTER);
    check(pump_until(app, [&] { return !app.job_running; }), "reference search finishes");
    const std::string want = format_matches(app.matches);
    check(render(app).find(" continue") == std::string::npos, "a finished search offers no 'continue'");

    // Cancel part way: `r` continues from where it stopped.
    handle_key(app, K_ENTER);
    handle_key(app, K_ENTER);
    check(pump_until(app, [&] { return app.jst.progress > 0.2; }), "search is under way");
    handle_key(app, 'c');
    check(pump_until(app, [&] { return !app.job_running; }), "cancel stops it");
    check(app.jst.state == svc::JobState::cancelled && render(app).find("r\x1b[0m continue") != std::string::npos,
          "a cancelled search offers 'continue'");
    handle_key(app, 'r');
    check(app.job_running && app.screen == Screen::result, "r starts it again");
    check(pump_until(app, [&] { return !app.job_running; }), "the continued search finishes");
    check(app.jst.state == svc::JobState::done && format_matches(app.matches) == want,
          "...with the results of the uninterrupted run");
    check(app.jst.rate * app.jst.elapsed_s < 0.9 * static_cast<double>(app.jst.candidates_total),
          "...having searched only what was left");

    // Stop at the first match, then carry on to the next.
    handle_key(app, K_ENTER);
    app.m.stop_first = true;
    for (int j = 3; j < 9; ++j)  // keep 3 rows: loose enough to match in many places
        for (int i = 0; i < 9; ++i) app.m.at(i, j) = svc::Cell::unknown;
    handle_key(app, K_ENTER);
    check(pump_until(app, [&] { return !app.job_running; }), "a stop-at-first search ends");
    check(app.jst.stopped_early, "...early");
    const std::size_t first_count = app.matches.size();
    handle_key(app, 'r');
    check(pump_until(app, [&] { return !app.job_running; }), "continuing it ends again");
    check(app.matches.size() > first_count, "...having found more matches than the first stop");
}

// A running search ignores everything but p / s / c: a fat-fingered Enter must not end it.
void test_running_search_ignores_stray_keys() {
    std::printf("test_running_search_ignores_stray_keys\n");
    App app;
    slow_search(app);
    handle_key(app, K_ENTER);
    check(app.job_running && app.screen == Screen::result, "the search is running");
    for (int k : std::initializer_list<int>{K_ENTER, K_ESC, 'q', 'Q', 'x', ' ', 'r', 'm', 'S', K_UP, K_DOWN, K_TAB, K_DELETE, K_BACKSPACE})
        handle_key(app, k);
    check(app.job_running && app.screen == Screen::result && app.jst.state != svc::JobState::cancelled,
          "Enter / Esc / q and other stray keys change nothing");
    handle_key(app, 'c');
    check(pump_until(app, [&] { return !app.job_running; }), "c still cancels it");
    check(app.jst.state == svc::JobState::cancelled, "...as a cancel");
    handle_key(app, K_ENTER);
    check(app.screen == Screen::grid, "and once it has stopped, Enter goes back");
}

}  // namespace

void test_grid_typing() {
    std::printf("test_grid_typing\n");
    App app;
    app.screen = Screen::grid;
    app.m.w = 3;
    app.m.h = 2;
    for (int k : {'b', 'e', '.', 'b'}) handle_key(app, k);
    check(app.m.at(0, 0) == svc::Cell::bedrock && app.m.at(1, 0) == svc::Cell::not_bedrock &&
              app.m.at(2, 0) == svc::Cell::unknown && app.m.at(0, 1) == svc::Cell::bedrock,
          "b / e / . paint cells like typing");
    check(app.gx == 1 && app.gy == 1, "cursor advances and wraps to the next row");
    handle_key(app, K_BACKSPACE);
    handle_key(app, K_BACKSPACE);
    check(app.gx == 2 && app.gy == 0, "backspace steps back across the row boundary");
    for (int k : {'e', 'e', 'e', 'e'}) handle_key(app, k);
    check(app.gx == 2 && app.gy == 1 && app.m.at(2, 1) == svc::Cell::not_bedrock,
          "the last cell holds the cursor");
}

int main() {
    test_file_round_trip();
    test_build_request_errors();
    test_fill_and_find(false);
    test_fill_and_find(true);
    test_fill_and_find(true, svc::Edition::bedrock);
    test_format_matches();
    test_clip_visible();
    test_frame_diff();
    test_format_blocks();
    test_map_layout();
    test_map_colours();
    test_map_searching_hit();
    test_map_celebration();
    test_map_end_to_end();
    test_stop_first();
    test_edition_toggle();
    test_pause_save_resume();
    test_continue_after_stop();
    test_running_search_ignores_stray_keys();
    test_grid_typing();
    return report();
}
