// Service-layer tests (CPU worker): the orchestrator + scheduler + sink produce
// exactly what a direct BedrockGenerator brute force does, including all 8
// orientations and regardless of tiling.
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <map>
#include <string>
#include <thread>
#include <vector>

#include "gen/bedrock.hpp"
#include "svc/pattern_io.hpp"
#include "svc/search_service.hpp"
#include "svc/workers.hpp"

using namespace rokkdoxx::svc;

namespace {

int g_fail = 0;
void check(bool ok, const std::string& what) {
    if (!ok) {
        std::printf("  FAIL: %s\n", what.c_str());
        ++g_fail;
    }
}

SearchService make_service() { return SearchService(make_worker_factory("cpu")); }

std::vector<Match> run(SearchService& svc, const SearchRequest& req) {
    JobId id = svc.submit(req);
    for (;;) {
        JobStatus st = svc.poll(id);
        if (st.state == JobState::done || st.state == JobState::cancelled) break;
        if (st.state == JobState::error) {
            std::printf("  FAIL: job error: %s\n", st.error.c_str());
            ++g_fail;
            return {};
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return svc.results(id);
}

// Brute-force reference: every origin in the region, every requested orientation.
std::map<std::pair<int, int>, std::uint8_t> brute(const SearchRequest& req) {
    rokkdoxx::BedrockGenerator gen(req.seed);
    const auto knowns = req.pattern.knowns();
    const int gN = req.all_orientations ? 8 : 1;
    std::map<std::pair<int, int>, std::uint8_t> out;
    for (std::int64_t z = req.region.z0; z <= req.region.z1; ++z)
        for (std::int64_t x = req.region.x0; x <= req.region.x1; ++x) {
            std::uint8_t mask = 0;
            for (int g = 0; g < gN; ++g) {
                Transform tf = kOrientations[static_cast<std::size_t>(g)];
                bool ok = true;
                for (const auto& kc : knowns) {
                    int du = tf.a * kc.i + tf.b * kc.j;
                    int dv = tf.c * kc.i + tf.d * kc.j;
                    bool bedrock = gen.is_bedrock_floor(static_cast<int>(x + du), req.plane_y,
                                                        static_cast<int>(z + dv));
                    if (bedrock != static_cast<bool>(kc.want)) {
                        ok = false;
                        break;
                    }
                }
                if (ok) mask |= static_cast<std::uint8_t>(1u << g);
            }
            if (mask) out[{static_cast<int>(x), static_cast<int>(z)}] = mask;
        }
    return out;
}

Pattern fill_pattern(std::int64_t seed, int y, int cx, int cz, int w, int h) {
    rokkdoxx::BedrockGenerator gen(seed);
    Pattern p;
    p.w = w;
    p.h = h;
    p.cells.assign(static_cast<std::size_t>(w) * h, Cell::unknown);
    for (int j = 0; j < h; ++j)
        for (int i = 0; i < w; ++i)
            p.cells[static_cast<std::size_t>(j) * w + i] =
                gen.is_bedrock_floor(cx + i, y, cz + j) ? Cell::bedrock : Cell::not_bedrock;
    return p;
}

void test_roundtrip_and_equiv() {
    std::printf("test_roundtrip_and_equiv\n");
    auto svc = make_service();
    const std::int64_t seed = 3257840388504953787LL;
    const int y = -60, cx = 1200, cz = -800;

    SearchRequest req;
    req.seed = seed;
    req.plane_y = y;
    req.pattern = fill_pattern(seed, y, cx, cz, 6, 6);
    req.region = Region::centered(cx + 2, cz + 2, 400);  // center not aligned with the pattern origin
    req.all_orientations = true;
    req.tile_side = 4096;

    auto got = run(svc, req);
    auto want = brute(req);

    check(got.size() == want.size(),
          "match count " + std::to_string(got.size()) + " vs brute " + std::to_string(want.size()));
    std::map<std::pair<int, int>, std::uint8_t> gm;
    for (auto& m : got) gm[{m.x, m.z}] = m.orient_mask;
    check(gm == want, "match sets identical");

    bool found_origin = false;
    for (auto& m : got)
        if (m.x == cx && m.z == cz && (m.orient_mask & 1)) found_origin = true;
    check(found_origin, "identity orientation found at the fill origin");
}

void test_tiling_invariant() {
    std::printf("test_tiling_invariant\n");
    auto svc = make_service();
    SearchRequest req;
    req.seed = 42;
    req.plane_y = -61;
    req.pattern = fill_pattern(42, -61, -50, 77, 5, 8);
    req.region = Region::centered(-48, 80, 300);
    req.all_orientations = true;

    req.tile_side = 4096;
    auto big = run(svc, req);
    req.tile_side = 7;  // pathological: pattern crosses many tile edges
    auto small = run(svc, req);

    std::map<std::pair<int, int>, std::uint8_t> a, b;
    for (auto& m : big) a[{m.x, m.z}] = m.orient_mask;
    for (auto& m : small) b[{m.x, m.z}] = m.orient_mask;
    check(a == b, "tile size does not change results (" + std::to_string(a.size()) + " vs " +
                      std::to_string(b.size()) + ")");
}

void test_cancel() {
    std::printf("test_cancel\n");
    auto svc = make_service();
    SearchRequest req;
    req.seed = 1;
    req.plane_y = -60;
    req.pattern = fill_pattern(1, -60, 0, 0, 4, 4);
    req.region = Region::centered(0, 0, 4'000'000);  // ~6e13 candidates
    JobId id = svc.submit(req);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    svc.cancel(id);
    for (int i = 0; i < 500; ++i) {
        JobStatus st = svc.poll(id);
        if (st.state == JobState::cancelled) {
            check(st.progress < 1.0, "cancelled before completion");
            return;
        }
        if (st.state == JobState::done) {
            check(false, "job finished a 6e13 search instead of cancelling");
            return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    check(false, "job did not cancel within 5s");
}

void test_scheduler_checkpoint() {
    std::printf("test_scheduler_checkpoint\n");
    SearchRequest req;
    req.seed = 5;
    req.region = Region::centered(0, 0, 5000);
    req.tile_side = 1024;
    const std::uint64_t fp = request_fingerprint(req);
    const std::string path = "test_ckpt.tmp";

    TileScheduler s1(req.region, req.tile_side);
    Tile t;
    int marked = 0;
    while (s1.next(t) && marked < s1.tile_count() / 2) {
        s1.mark_done(t);
        ++marked;
    }
    s1.save_checkpoint(path, fp);

    TileScheduler s2(req.region, req.tile_side);
    check(s2.load_checkpoint(path, fp), "checkpoint loads with matching fingerprint");
    check(s2.done_count() == marked, "resumed with the right tile count");

    TileScheduler s3(req.region, req.tile_side);
    check(!s3.load_checkpoint(path, fp ^ 1), "checkpoint rejected on fingerprint mismatch");
    std::remove(path.c_str());
}

void test_pattern_io_roundtrip() {
    std::printf("test_pattern_io_roundtrip\n");
    // Rows starting with '#' (a bedrock cell) must NOT be eaten as comments.
    PatternFile pf;
    pf.seed = "2024";
    pf.y = -60;
    pf.center_x = "100";
    pf.center_z = "100";
    pf.radius = "400";
    pf.all_orientations = false;
    pf.pattern = fill_pattern(2024, -60, 100, 100, 5, 5);  // ~any layout incl. leading '#'
    const std::string path = "test_pat.tmp";
    std::string err;
    check(save_pattern_file(path, pf, err), "save ok");

    PatternFile got;
    check(load_pattern_file(path, got, err), "load ok");
    check(got.pattern.w == 5 && got.pattern.h == 5, "dims preserved");
    check(got.pattern.cells == pf.pattern.cells, "cells preserved (no '#'-row loss)");
    check(got.pattern.knowns().size() == 25, "all 25 cells known after round-trip");
    std::remove(path.c_str());
}

}  // namespace

int main() {
    test_roundtrip_and_equiv();
    test_tiling_invariant();
    test_cancel();
    test_scheduler_checkpoint();
    test_pattern_io_roundtrip();
    if (g_fail == 0) {
        std::printf("\nALL PASS\n");
        return 0;
    }
    std::printf("\n%d FAILURE(S)\n", g_fail);
    return 1;
}
