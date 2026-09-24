// rokktui logic tests -- no terminal involved: the Model and its conversions,
// a fill-from-world round trip through a real (CPU) search, the matches file
// format, and the Frame diff/clipping.
#include <chrono>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

#include "svc/client.hpp"
#include "tui/frame.hpp"
#include "tui/model.hpp"

using namespace rokkdoxx;
using namespace rokkdoxx::tui;

namespace {

int g_fail = 0;
void check(bool ok, const std::string& what) {
    if (!ok) {
        std::printf("  FAIL: %s\n", what.c_str());
        ++g_fail;
    }
}

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
    a.at(0, 0) = svc::Cell::bedrock;
    a.at(4, 2) = svc::Cell::not_bedrock;
    a.at(10, 10) = svc::Cell::bedrock;  // outside w x h: not part of the pattern

    Model b;
    file_to_model(model_to_file(a), b);
    check(b.seed == a.seed && b.w == 5 && b.h == 3 && b.y == -61, "round trip: header fields");
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
void test_fill_and_find(bool all_orient) {
    Model m;
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
    check(found, all_orient ? "all-8: painted spot is found (identity bit, anchor in footprint)"
                            : "exact: painted spot is found (anchor in footprint)");
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

}  // namespace

int main() {
    test_file_round_trip();
    test_build_request_errors();
    test_fill_and_find(false);
    test_fill_and_find(true);
    test_format_matches();
    test_clip_visible();
    test_frame_diff();
    if (g_fail) {
        std::printf("test_tui: %d failure(s)\n", g_fail);
        return 1;
    }
    std::printf("test_tui: all passed\n");
    return 0;
}
