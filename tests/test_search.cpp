// Service-layer tests (CPU worker): the orchestrator + scheduler + sink produce
// exactly what a direct BedrockGenerator brute force does, including all 8
// orientations and regardless of tiling.
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <fstream>
#include <iterator>
#include <map>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "check.hpp"
#include "gen/bedrock.hpp"
#include "svc/pattern_io.hpp"
#include "svc/search_service.hpp"
#include "svc/workers.hpp"

using namespace rokkdoxx::svc;

namespace {

SearchService make_service() { return SearchService(make_worker_factory("cpu")); }

JobStatus wait_for(SearchService& svc, JobId id, JobState want, double timeout_s = 20) {
    const auto t0 = std::chrono::steady_clock::now();
    for (;;) {
        JobStatus st = svc.poll(id);
        if (st.state == want || st.state == JobState::error) return st;
        if (std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() > timeout_s) return st;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
}

std::map<std::pair<int, int>, std::uint8_t> as_map(const std::vector<Match>& ms) {
    std::map<std::pair<int, int>, std::uint8_t> out;
    for (const Match& m : ms) out[{m.x, m.z}] = m.orient_mask;
    return out;
}

std::vector<Match> run(SearchService& svc, const SearchRequest& req) {
    const JobId id = svc.submit(req);
    const JobStatus st = wait_for(svc, id, JobState::done, 600);
    check(st.state == JobState::done, "job finishes " + st.error);
    check(st.eta_s == 0.0, "a finished job has no ETA left");
    return svc.results(id);
}

// Brute-force reference. Independent of build_search_plan's variant/offset
// tables: it takes only the anchor cell offset from the plan (trivially
// checkable) and does a plain 8-orientation scan, keyed to the anchor's world
// position -- the worker's output contract.
std::map<std::pair<int, int>, std::uint8_t> brute(const SearchRequest& req) {
    rokkdoxx::BedrockGenerator gen(req.seed, req.edition);
    const auto knowns = req.pattern.knowns();
    const std::uint32_t thr = gen.threshold(req.plane_y);
    const SearchPlan plan = build_search_plan(knowns, thr, req.all_orientations);
    const int ai = plan.anchor_i, aj = plan.anchor_j;
    const int gN = req.all_orientations ? 8 : 1;
    std::map<std::pair<int, int>, std::uint8_t> out;
    for (std::int64_t z = req.region.z0; z <= req.region.z1; ++z)
        for (std::int64_t x = req.region.x0; x <= req.region.x1; ++x) {
            std::uint8_t mask = 0;
            for (int g = 0; g < gN; ++g) {
                Transform tf = kOrientations[static_cast<std::size_t>(g)];
                bool ok = true;
                for (const auto& kc : knowns) {
                    const int di = kc.i - ai, dj = kc.j - aj;
                    const int du = tf.a * di + tf.b * dj;
                    const int dv = tf.c * di + tf.d * dj;
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

void test_roundtrip_and_equiv() {
    std::printf("test_roundtrip_and_equiv\n");
    auto svc = make_service();
    const std::int64_t seed = 3257840388504953787LL;
    const int y = -60, cx = 1200, cz = -800;

    SearchRequest req;
    req.seed = seed;
    req.plane_y = y;
    req.pattern = world_patch(seed, y, cx, cz, 6, 6);
    req.region = Region::centered(cx + 2, cz + 2, 400);  // center not aligned with the pattern origin
    req.all_orientations = true;
    req.tile_side = 4096;

    auto got = run(svc, req);
    auto want = brute(req);

    check(got.size() == want.size(),
          "match count " + std::to_string(got.size()) + " vs brute " + std::to_string(want.size()));
    check(as_map(got) == want, "match sets identical");

    // The fill origin under identity: the anchor cell sits at (cx, cz) + anchor
    // offset (matches report the anchor's world position).
    rokkdoxx::BedrockGenerator g2(seed);
    const SearchPlan plan = build_search_plan(req.pattern.knowns(), g2.threshold(y), true);
    bool found_origin = false;
    for (auto& m : got)
        if (m.x == cx + plan.anchor_i && m.z == cz + plan.anchor_j && (m.orient_mask & 1))
            found_origin = true;
    check(found_origin, "identity orientation found at the fill origin");
}

// Bedrock Edition: the CPU worker's chunk-at-a-time path equals the brute force
// (one generator call per cell), near 0 and across the 2^24 edge where columns
// start to repeat; 9-block tiles put the pattern across many tile edges.
void test_bedrock_equiv() {
    std::printf("test_bedrock_equiv\n");
    auto svc = make_service();
    for (const int c : {-40, 16777216})
        for (const int tile : {4096, 9}) {
            SearchRequest req;
            req.edition = Edition::bedrock;
            req.seed = 12345;  // not used
            req.plane_y = -61;
            req.pattern = world_patch(0, -61, c, -c, 4, 5, Edition::bedrock);
            req.pattern.cells[3] = Cell::unknown;
            req.region = Region::centered(c + 1, -c + 2, 60);
            req.tile_side = tile;
            const auto want = brute(req);
            check(!want.empty() && as_map(run(svc, req)) == want,
                  "bedrock at " + std::to_string(c) + ", tile " + std::to_string(tile) + ": " +
                      std::to_string(want.size()) + " brute-force matches");
        }

    SearchRequest req;
    req.pattern = world_patch(0, -60, 0, 0, 3, 3);
    const std::uint64_t java = request_fingerprint(req);
    req.edition = Edition::bedrock;
    check(request_fingerprint(req) != java, "a Java checkpoint does not resume a Bedrock search");
    req.region = Region::centered(33554400, 0, 1);
    const JobStatus st = wait_for(svc, svc.submit(req), JobState::done);
    check(st.state == JobState::error, "Bedrock searches past +/-2^25 are refused");
}

void test_tiling_invariant() {
    std::printf("test_tiling_invariant\n");
    auto svc = make_service();
    SearchRequest req;
    req.seed = 42;
    req.plane_y = -61;
    req.pattern = world_patch(42, -61, -50, 77, 5, 8);
    req.region = Region::centered(-48, 80, 300);
    req.all_orientations = true;

    req.tile_side = 4096;
    auto big = run(svc, req);
    req.tile_side = 7;  // pathological: pattern crosses many tile edges
    auto small = run(svc, req);

    check(as_map(big) == as_map(small) && big.size() == small.size(),
          "tile size does not change results (" + std::to_string(big.size()) + " vs " +
              std::to_string(small.size()) + ")");
}

void test_cancel() {
    std::printf("test_cancel\n");
    auto svc = make_service();
    SearchRequest req;
    req.seed = 1;
    req.plane_y = -60;
    req.pattern = world_patch(1, -60, 0, 0, 4, 4);
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
    const std::vector<Match> saved_matches = {{10, 20, 1}, {-5, 300, 0x83}};

    TileScheduler s1(req.region, req.tile_side);
    Tile t;
    int marked = 0;
    while (s1.next(t) && marked < s1.tile_count() / 2) {
        s1.mark_done(t);
        ++marked;
    }
    std::ofstream out(path);
    s1.write_checkpoint(out, fp, saved_matches);
    out.close();

    // The whole point of persisting matches: a resumed run must not lose (or
    // need to re-find) matches from tiles it's about to skip as already-done.
    TileScheduler s2(req.region, req.tile_side);
    std::vector<Match> restored;
    check(s2.load_checkpoint(path, fp, restored), "checkpoint loads with matching fingerprint");
    check(s2.done_count() == marked, "resumed with the right tile count");
    check(restored.size() == saved_matches.size(), "resumed with the right match count");
    bool matches_equal = restored.size() == saved_matches.size();
    for (std::size_t i = 0; matches_equal && i < restored.size(); ++i)
        matches_equal = restored[i].x == saved_matches[i].x && restored[i].z == saved_matches[i].z &&
                         restored[i].orient_mask == saved_matches[i].orient_mask;
    check(matches_equal, "resumed matches are byte-identical to what was saved");

    TileScheduler s3(req.region, req.tile_side);
    std::vector<Match> ignored;
    check(!s3.load_checkpoint(path, fp ^ 1, ignored), "checkpoint rejected on fingerprint mismatch");

    std::remove(path.c_str());
}

// Which of `cells` map cells holds tile `t` of `n`? Written the slow obvious
// way (cell c owns tiles [c*n/cells, (c+1)*n/cells)), independent of the
// scheduler's closed-form inverse.
int slow_cell(int t, int n, int cells) {
    int c = 0;
    while (c + 1 < cells && t >= (c + 1) * n / cells) ++c;
    return c;
}

MapCell cell(MapCell::Phase p, bool hit = false) { return {p, hit}; }

// Which tile of an nx x nz grid a region's midpoint falls in (the spiral's start).
int centre_tile(const Region& r, int tile, int nx, int nz) {
    const int cx = std::min(nx - 1, static_cast<int>(((r.x0 + r.x1) / 2 - r.x0) / tile));
    const int cz = std::min(nz - 1, static_cast<int>(((r.z0 + r.z1) / 2 - r.z0) / tile));
    return cz * nx + cx;
}

// The scan goes outward from the middle of the region: every tile once, the
// centre tile first, never a farther ring before a nearer one, and each full
// ring walked round clockwise from its top-left corner.
void test_scheduler_spiral() {
    std::printf("test_scheduler_spiral\n");
    struct Grid { const char* name; Region region; int nx, nz; };
    const Grid grids[] = {
        {"5x5 (odd)", Region{0, 499, 0, 499}, 5, 5},
        {"4x4 (even)", Region{0, 399, 0, 399}, 4, 4},
        {"7x5", Region{0, 699, 0, 499}, 7, 5},
        {"1x9 strip", Region{0, 99, 0, 899}, 1, 9},
        {"9x1 strip", Region{0, 899, 0, 99}, 9, 1},
        {"1x1", Region{0, 99, 0, 99}, 1, 1},
        {"off-origin", Region{-300, 499, 1000, 1399}, 8, 4},
    };
    for (const Grid& g : grids) {
        const std::string name = g.name;
        TileScheduler s(g.region, 100);
        check(s.tile_count() == g.nx * g.nz, name + ": tile count");
        const int centre = centre_tile(g.region, 100, g.nx, g.nz);
        const int cx = centre % g.nx, cz = centre / g.nx;

        std::vector<Tile> order;
        Tile t;
        while (s.next(t)) order.push_back(t);
        std::vector<int> seen(static_cast<std::size_t>(g.nx * g.nz), 0);
        for (const Tile& tl : order) ++seen[static_cast<std::size_t>(tl.index)];
        bool once = order.size() == seen.size();
        for (int n : seen) once = once && n == 1;
        check(once, name + ": every tile exactly once");
        check(!order.empty() && order[0].index == centre, name + ": starts at the centre tile");

        bool rings_ok = true;
        int last_ring = 0;
        for (const Tile& tl : order) {
            const int ring = std::max(std::abs(tl.index % g.nx - cx), std::abs(tl.index / g.nx - cz));
            if (ring < last_ring) rings_ok = false;
            last_ring = ring;
        }
        check(rings_ok, name + ": a farther ring never comes before a nearer one");
    }

    // A ring that fits inside the grid is one clockwise lap from its top-left corner.
    TileScheduler s(Region{0, 499, 0, 499}, 100);  // 5x5, centre (2,2)
    std::vector<int> idx;
    Tile t;
    while (s.next(t)) idx.push_back(t.index);
    bool laps = true;
    std::size_t at = 1;  // ring 0 is the centre
    for (int r = 1; r <= 2; ++r) {
        int x = 2 - r, z = 2 - r;  // top-left corner
        for (int step = 0; step < 8 * r; ++step, ++at) {
            laps = laps && idx[at] == z * 5 + x;
            if (z == 2 - r && x < 2 + r) ++x;         // top row, rightwards
            else if (x == 2 + r && z < 2 + r) ++z;    // right column, downwards
            else if (z == 2 + r && x > 2 - r) --x;    // bottom row, leftwards
            else --z;                                 // left column, upwards
        }
    }
    check(laps && at == idx.size(), "rings are walked clockwise from the top-left corner");
}

// Step through a whole scan like the service does (two tiles in flight) and compare the
// map after every step with per-cell counters kept here, independently of the scheduler.
void test_scheduler_map() {
    std::printf("test_scheduler_map\n");
    // 7 x 5 tiles of 100 blocks, cut into 3 x 2 map cells: widths 2,2,3 tiles, heights 2,3.
    const Region region{0, 699, 0, 499};
    TileScheduler s(region, 100, 3, 2);
    check(s.tile_count() == 35 && s.map_w() == 3 && s.map_h() == 2, "7x5 tiles in a 3x2 map");
    check(s.map().size() == 6, "a 3x2 map has 6 cells");

    auto cell_of = [](const Tile& t) {
        return slow_cell(t.index / 7, 5, 2) * 3 + slow_cell(t.index % 7, 7, 3);
    };
    int total[6] = {}, done[6] = {}, busy[6] = {};
    for (int i = 0; i < 35; ++i) ++total[slow_cell(i / 7, 5, 2) * 3 + slow_cell(i % 7, 7, 3)];
    auto expected = [&] {
        std::vector<MapCell> e;
        for (int c = 0; c < 6; ++c)
            e.push_back(cell(busy[c] ? MapCell::searching
                             : done[c] == total[c] ? MapCell::done
                             : done[c] > 0         ? MapCell::partial
                                                   : MapCell::pending));
        return e;
    };

    bool states_ok = s.map() == expected();
    bool saw_partial = false, saw_searching = false;
    std::deque<Tile> inflight;
    Tile t;
    bool more = true;
    while (true) {
        while (more && inflight.size() < 2) {
            if (!s.next(t)) { more = false; break; }
            ++busy[cell_of(t)];
            inflight.push_back(t);
            states_ok = states_ok && s.map() == expected();
        }
        if (inflight.empty()) break;
        const Tile d = inflight.front();
        inflight.pop_front();
        --busy[cell_of(d)];
        ++done[cell_of(d)];
        s.mark_done(d);
        const std::vector<MapCell> m = s.map();
        states_ok = states_ok && m == expected();
        for (const MapCell& c : m) {
            saw_partial = saw_partial || c.phase == MapCell::partial;
            saw_searching = saw_searching || c.phase == MapCell::searching;
        }
    }
    check(states_ok, "map matches independent per-cell counters after every step");
    check(saw_partial && saw_searching, "the scan passes through 'partial' and 'searching'");
    bool all_done = true;
    for (const MapCell& c : s.map()) all_done = all_done && c == cell(MapCell::done);
    check(all_done, "a finished scan is all done");

    // Hits: the origin's own cell, from the coordinates alone -- and independent of the
    // phase (a cell can hold a match while still being searched, or before it is scanned).
    TileScheduler h(region, 100, 3, 2);
    Tile first;
    check(h.next(first), "first tile");  // in flight: its cell is searching
    const int searching_cell = cell_of(first);
    const int other = searching_cell == 5 ? 4 : 5;
    h.mark_hit(first.x0, first.z0);
    h.mark_hit(other % 3 * 233 + 50, other / 3 * 300 + 50);  // inside `other`'s area (not started)
    h.mark_hit(-5, 0);                                      // outside the region: ignored
    h.mark_hit(0, 500);
    const std::vector<MapCell> hm = h.map();
    check(hm[static_cast<std::size_t>(searching_cell)] == cell(MapCell::searching, true),
          "a cell can be searching and already hold a hit");
    int hits = 0;
    for (const MapCell& c : hm) hits += c.hit;
    check(hits == 2 && hm[static_cast<std::size_t>(other)].hit && hm[static_cast<std::size_t>(other)].phase == MapCell::pending,
          "a match marks its cell even before it is scanned; outside the region changes nothing");

    // Every tile's origin lights up exactly the cell the slow formula says.
    bool right_cell = true;
    for (int tz = 0; tz < 5; ++tz)
        for (int tx = 0; tx < 7; ++tx) {
            TileScheduler one_hit(region, 100, 3, 2);
            one_hit.mark_hit(tx * 100 + 99, tz * 100);  // the far edge of the tile, too
            const int want = slow_cell(tz, 5, 2) * 3 + slow_cell(tx, 7, 3);
            const std::vector<MapCell> m = one_hit.map();
            for (int c = 0; c < 6; ++c)
                if (m[static_cast<std::size_t>(c)].hit != (c == want)) right_cell = false;
        }
    check(right_cell, "a match lights up the map cell its tile belongs to, for every tile");

    // A map bigger than the tile grid is clamped to it.
    TileScheduler one(Region{0, 99, 0, 99}, 100, 10, 10);
    check(one.map_w() == 1 && one.map_h() == 1 && one.map().size() == 1, "map clamped to the tile grid");

    // A resumed checkpoint shows the progress it covers (partly done cells included).
    const std::string path = "test_map_ckpt.tmp";
    SearchRequest req;
    req.region = region;
    req.tile_side = 100;
    const std::uint64_t fp = request_fingerprint(req);
    TileScheduler part(region, 100, 3, 2);
    for (int i = 0; i < 12; ++i) {
        part.next(t);
        part.mark_done(t);
    }
    std::ofstream out(path);
    part.write_checkpoint(out, fp, {});
    out.close();
    TileScheduler r(region, 100, 3, 2);
    std::vector<Match> restored;
    check(r.load_checkpoint(path, fp, restored), "checkpoint loads");
    check(r.map() == part.map(), "resumed map equals the map that was saved");
    bool any_partial = false;
    for (const MapCell& c : r.map()) any_partial = any_partial || c.phase == MapCell::partial;
    check(any_partial, "...including cells that were only partly done");
    std::remove(path.c_str());
}

// A real job with a map: the map has the requested shape, only ever moves forward, and ends
// with exactly the cells that contain a match marked as hits.
void test_service_map() {
    std::printf("test_service_map\n");
    auto svc = make_service();
    const std::int64_t seed = 3257840388504953787LL;
    const int y = -60, cx = 1200, cz = -800;
    SearchRequest req;
    req.seed = seed;
    req.plane_y = y;
    req.pattern = world_patch(seed, y, cx, cz, 5, 5);
    req.region = Region::centered(cx + 2, cz + 2, 450);  // 901 blocks -> 10 x 10 tiles of 100
    req.tile_side = 100;
    req.map_w = req.map_h = 4;

    const JobId id = svc.submit(req);
    JobStatus st;
    std::vector<MapCell> prev;
    bool monotonic = true;
    for (;;) {
        st = svc.poll(id);
        if (!st.map.empty()) {
            for (std::size_t i = 0; i < prev.size(); ++i) {
                if (prev[i].phase == MapCell::done && st.map[i].phase != MapCell::done) monotonic = false;
                if (prev[i].phase != MapCell::pending && st.map[i].phase == MapCell::pending) monotonic = false;
                if (prev[i].hit && !st.map[i].hit) monotonic = false;
            }
            prev = st.map;
        }
        if (st.state == JobState::done || st.state == JobState::error) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    check(st.state == JobState::done, "search finishes");
    check(st.map_w == 4 && st.map_h == 4 && st.map.size() == 16, "the map has the requested 4x4 shape");
    check(monotonic, "done is final, a started cell never goes back to pending, a hit never disappears");

    std::vector<MapCell> want(16, cell(MapCell::done));
    const auto results = svc.results(id);
    check(!results.empty(), "the search found something to mark");
    // Scanning in a different order must not change what is found.
    check(as_map(results) == brute(req), "spiral scan finds exactly what brute force finds");
    for (const Match& m : results) {
        const int tx = static_cast<int>((m.x - req.region.x0) / 100);
        const int tz = static_cast<int>((m.z - req.region.z0) / 100);
        want[static_cast<std::size_t>(slow_cell(tz, 10, 4) * 4 + slow_cell(tx, 10, 4))].hit = true;
    }
    check(st.map == want, "final map: done everywhere, hit exactly where matches are");

    // Resuming from that run's checkpoint recomputes nothing, yet the map must
    // come back whole -- including the hits, which only survive as restored matches.
    req.checkpoint_path = "test_map_svc.ckpt";
    std::remove(req.checkpoint_path.c_str());
    run(svc, req);  // writes the checkpoint
    st = wait_for(svc, svc.submit(req), JobState::done);
    check(st.map == want, "a fully resumed job shows the same map, hits included");
    std::remove(req.checkpoint_path.c_str());
    req.checkpoint_path.clear();

    // Not asking for a map costs nothing and reports nothing.
    req.map_w = req.map_h = 0;
    st = wait_for(svc, svc.submit(req), JobState::done);
    check(st.map.empty() && st.map_w == 0, "no map unless asked for");
}

// stop_at_first_match: a pattern planted at the middle of the region is found within the first
// few rings, so the job ends with most of the region unsearched; resuming finishes the rest.
void test_stop_at_first() {
    std::printf("test_stop_at_first\n");
    auto svc = make_service();
    const std::int64_t seed = 777;
    const int y = -60, cx = -40000, cz = 25000;
    SearchRequest req;
    req.seed = seed;
    req.plane_y = y;
    req.pattern = world_patch(seed, y, cx - 6, cz - 6, 12, 12);  // 12x12: unique in this region
    req.region = Region::centered(cx, cz, 450);                   // 10 x 10 tiles of 100
    req.tile_side = 100;
    req.checkpoint_path = "test_stop.ckpt";
    std::remove(req.checkpoint_path.c_str());

    const SearchPlan plan =
        build_search_plan(req.pattern.knowns(), rokkdoxx::BedrockGenerator(seed).threshold(y), true);
    const int want_x = cx - 6 + plan.anchor_i, want_z = cz - 6 + plan.anchor_j;
    auto has_planted = [&](const std::vector<Match>& ms) {
        for (const Match& m : ms)
            if (m.x == want_x && m.z == want_z && (m.orient_mask & 1)) return true;
        return false;
    };

    req.stop_at_first_match = true;
    JobId id = svc.submit(req);
    JobStatus st = wait_for(svc, id, JobState::done);
    check(st.state == JobState::done && st.stopped_early, "stops early once a tile has a match");
    check(has_planted(svc.results(id)), "...with the planted match in the results");
    check(st.candidates_done * 5 < st.candidates_total,
          "...having searched under a fifth of the region (it started at the middle)");
    check(st.progress < 1.0, "...and reports that the region was not finished");

    // The same job resumed from its checkpoint carries on (restored matches must not stop it
    // again at once) and completes the region; with one match in the region it ends normally.
    id = svc.submit(req);
    st = wait_for(svc, id, JobState::done);
    check(st.state == JobState::done && !st.stopped_early && st.progress == 1.0,
          "a resumed job finishes the rest of the region");
    check(has_planted(svc.results(id)), "...and still reports the match found before the stop");
    std::remove(req.checkpoint_path.c_str());

    // Off (the default): the whole region is searched.
    req.stop_at_first_match = false;
    req.checkpoint_path.clear();
    id = svc.submit(req);
    st = wait_for(svc, id, JobState::done);
    check(!st.stopped_early && st.progress == 1.0 && has_planted(svc.results(id)),
          "without the flag the whole region is searched");
}

// --- pause / resume / in-memory checkpoints ---------------------------------

// A job big enough (~1-2 s on the CPU) to pause in the middle of.
SearchRequest pausable_request() {
    SearchRequest req;
    req.seed = 99;
    req.plane_y = -60;
    req.pattern = world_patch(99, -60, 5000, -7000, 9, 9);
    req.region = Region::centered(5004, -6996, 15000);
    req.tile_side = 1024;
    req.all_orientations = false;
    return req;
}

void test_pause_resume() {
    std::printf("test_pause_resume\n");
    auto svc = make_service();
    const SearchRequest req = pausable_request();

    const JobId reference = svc.submit(req);
    check(wait_for(svc, reference, JobState::done).state == JobState::done, "reference run finishes");
    const auto want = as_map(svc.results(reference));
    check(!want.empty(), "the planted match is in the reference results");

    const auto wall0 = std::chrono::steady_clock::now();
    const JobId id = svc.submit(req);
    while (svc.poll(id).progress < 0.1) std::this_thread::sleep_for(std::chrono::milliseconds(2));
    svc.pause(id, true);
    JobStatus frozen = wait_for(svc, id, JobState::paused);
    check(frozen.state == JobState::paused, "pause takes effect (state becomes paused)");
    check(frozen.progress < 1.0, "...in the middle of the job");

    std::this_thread::sleep_for(std::chrono::milliseconds(600));
    const JobStatus later = svc.poll(id);
    check(later.state == JobState::paused && later.candidates_done == frozen.candidates_done,
          "nothing is searched while paused");
    check(later.elapsed_s == frozen.elapsed_s, "elapsed time stops while paused");

    svc.pause(id, false);
    const JobStatus fin = wait_for(svc, id, JobState::done);
    const double wall = std::chrono::duration<double>(std::chrono::steady_clock::now() - wall0).count();
    check(fin.state == JobState::done && fin.progress == 1.0, "a resumed job finishes");
    check(as_map(svc.results(id)) == want, "...with exactly the results of an uninterrupted run");
    check(fin.elapsed_s < wall - 0.5, "the 0.6 s pause is not counted in elapsed");
    // rate * elapsed = candidates searched this run (all of them: nothing was resumed). If the
    // pause leaked into the rate it would come up short. Compared within one run, so CPU noise
    // between runs cannot trip it.
    const double searched = fin.rate * fin.elapsed_s / static_cast<double>(fin.candidates_total);
    check(searched > 0.9 && searched < 1.1, "...nor in the rate (so the ETA stays honest after a pause)");

    // Cancel works on a paused job.
    const JobId id2 = svc.submit(req);
    while (svc.poll(id2).progress < 0.05) std::this_thread::sleep_for(std::chrono::milliseconds(2));
    svc.pause(id2, true);
    wait_for(svc, id2, JobState::paused);
    svc.cancel(id2);
    check(wait_for(svc, id2, JobState::cancelled).state == JobState::cancelled, "cancelling a paused job ends it");
}

// The in-memory checkpoint: a job that keeps one can be stopped, and a new job started from
// its text finishes the work with the same results as an uninterrupted run.
void test_checkpoint_text() {
    std::printf("test_checkpoint_text\n");
    auto svc = make_service();
    SearchRequest req = pausable_request();

    const JobId reference = svc.submit(req);
    wait_for(svc, reference, JobState::done);
    const auto want = as_map(svc.results(reference));

    check(svc.checkpoint(reference).empty(), "a job that did not ask for one has no checkpoint text");

    req.keep_checkpoint = true;
    const JobId id = svc.submit(req);
    while (svc.poll(id).progress < 0.3) std::this_thread::sleep_for(std::chrono::milliseconds(2));
    svc.cancel(id);
    const JobStatus cut = wait_for(svc, id, JobState::cancelled);
    const std::string text = svc.checkpoint(id);
    check(cut.state == JobState::cancelled && cut.progress < 1.0, "the job is stopped part way");
    check(text.rfind("rokkdoxx-checkpoint 2 ", 0) == 0, "its checkpoint text is in the checkpoint format");

    // Same text must also be what a file checkpoint would hold.
    std::ofstream("test_text.ckpt") << text;
    TileScheduler probe(req.region, 1024);
    std::vector<Match> from_file;
    check(probe.load_checkpoint("test_text.ckpt", request_fingerprint(req), from_file) &&
              probe.candidates_done() > 0 && probe.candidates_done() <= cut.candidates_done + 1024LL * 1024 * 2,
          "the text reads back as a checkpoint covering the work done");
    std::remove("test_text.ckpt");

    SearchRequest again = req;
    again.resume_text = text;
    const JobId id2 = svc.submit(again);
    const JobStatus fin = wait_for(svc, id2, JobState::done);
    check(fin.state == JobState::done && fin.progress == 1.0, "a job started from the text finishes");
    check(as_map(svc.results(id2)) == want, "...with the results of an uninterrupted run");
    // rate * elapsed = candidates searched by this run: it must not have redone the finished part.
    check(fin.rate * fin.elapsed_s < 0.85 * static_cast<double>(fin.candidates_total),
          "...having searched only what was left, not the whole region again");

    // Text for a different search is ignored: the job just runs from scratch.
    SearchRequest other = req;
    other.seed += 1;
    other.resume_text = text;
    other.keep_checkpoint = false;
    const JobId id3 = svc.submit(other);
    SearchRequest other_plain = other;
    other_plain.resume_text.clear();
    const JobId id4 = svc.submit(other_plain);
    wait_for(svc, id3, JobState::done);
    wait_for(svc, id4, JobState::done);
    check(as_map(svc.results(id3)) == as_map(svc.results(id4)), "checkpoint text for another search is ignored");

    // Round trip through the stream API, matches included.
    TileScheduler a(Region{0, 699, 0, 499}, 100);
    Tile t;
    for (int i = 0; i < 12; ++i) { a.next(t); a.mark_done(t); }
    std::ostringstream os;
    a.write_checkpoint(os, 77, {{10, 20, 1}, {-5, 300, 0x83}});
    TileScheduler b(Region{0, 699, 0, 499}, 100);
    std::istringstream is(os.str());
    std::vector<Match> back;
    check(b.read_checkpoint(is, 77, back) && b.done_count() == 12 && back.size() == 2 && back[1].x == -5,
          "checkpoint text round-trips through the stream API");
    TileScheduler c(Region{0, 699, 0, 499}, 100);
    std::istringstream bad(os.str());
    check(!c.read_checkpoint(bad, 78, back) && c.done_count() == 0, "a wrong fingerprint changes nothing");
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
    pf.pattern = world_patch(2024, -60, 100, 100, 5, 5);  // ~any layout incl. leading '#'
    const std::string path = "test_pat.tmp";
    std::string err;
    check(save_pattern_file(path, pf, err), "save ok");

    PatternFile got;
    check(load_pattern_file(path, got, err), "load ok");
    check(got.pattern.w == 5 && got.pattern.h == 5, "dims preserved");
    check(got.pattern.cells == pf.pattern.cells, "cells preserved (no '#'-row loss)");
    check(got.pattern.knowns().size() == 25, "all 25 cells known after round-trip");
    check(got.checkpoint.empty() && !got.stop_first && got.edition == Edition::java,
          "a plain pattern has no session keys, and is Java");
    {
        std::ifstream f(path);
        const std::string text((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        check(text.find("checkpoint") == std::string::npos && text.find("stop_at_first") == std::string::npos &&
                  text.find("edition") == std::string::npos,
              "...and none are written for it (existing files stay byte-identical)");
    }
    pf.edition = Edition::bedrock;
    check(save_pattern_file(path, pf, err) && load_pattern_file(path, got, err) && got.edition == Edition::bedrock,
          "a Bedrock Edition pattern says so");
    pf.edition = Edition::java;

    // A saved session: the same file plus the progress file it names (path may have spaces).
    pf.checkpoint = "my saves/run 1.ckpt";
    pf.stop_first = true;
    check(save_pattern_file(path, pf, err), "save a session");
    PatternFile ses;
    check(load_pattern_file(path, ses, err) && ses.checkpoint == "my saves/run 1.ckpt" && ses.stop_first &&
              ses.pattern.cells == pf.pattern.cells && ses.seed == "2024",
          "a session round-trips: checkpoint path (with spaces), stop flag, and the pattern");
    std::remove(path.c_str());
}

// build_search_plan: anchor choice + duplicate-orientation collapse, plus an
// end-to-end check that a symmetric pattern still returns exactly what a plain
// 8-orientation brute force does (keyed to the anchor).
void test_format_duration() {
    std::printf("test_format_duration\n");
    check(format_duration(0) == "0s" && format_duration(-3) == "0s", "zero / negative");
    check(format_duration(59.4) == "59s", "seconds");
    check(format_duration(65) == "1m 05s", "minutes");
    check(format_duration(3723) == "1h 02m 03s", "hours");
    check(format_duration(100 * 3600.0) == "100h 00m 00s", "long runs keep counting hours");
}

void test_search_plan() {
    std::printf("test_search_plan\n");

    // A bedrock "plus" recentres to a fully D4-symmetric set -> 1 variant, all
    // 8 orientation bits.
    Pattern plus;
    plus.w = plus.h = 5;
    plus.cells.assign(25, Cell::unknown);
    auto b = [&](int i, int j) { plus.cells[static_cast<std::size_t>(j) * 5 + i] = Cell::bedrock; };
    b(2, 2);
    b(2, 0);
    b(2, 4);
    b(0, 2);
    b(4, 2);
    const std::uint32_t thr = rokkdoxx::BedrockGenerator(0).threshold(-60);
    SearchPlan pp = build_search_plan(plus.knowns(), thr, true);
    check(pp.n_variants == 1, "symmetric plus -> 1 variant (got " + std::to_string(pp.n_variants) + ")");
    check(pp.variant_mask.size() == 1 && pp.variant_mask[0] == 0xFF, "variant covers all 8 bits");
    check(pp.anchor_i == 2 && pp.anchor_j == 2, "anchor is the centre cell");
    check(pp.want[0] == 1 && pp.off_x[0] == 0 && pp.off_z[0] == 0, "cell 0 is the anchor at (0,0)");

    // A scalene right triangle (no symmetry) -> 8 distinct orientations.
    Pattern tri;
    tri.w = 3;
    tri.h = 2;
    tri.cells.assign(6, Cell::unknown);
    tri.cells[0] = Cell::bedrock;  // (0,0)
    tri.cells[2] = Cell::bedrock;  // (2,0)
    tri.cells[3] = Cell::bedrock;  // (0,1)
    SearchPlan lp = build_search_plan(tri.knowns(), thr, true);
    check(lp.n_variants == 8, "scalene shape -> 8 variants (got " + std::to_string(lp.n_variants) + ")");
    std::uint8_t all = 0;
    int bits = 0;
    for (auto m : lp.variant_mask) {
        all |= m;
        for (int k = 0; k < 8; ++k) bits += (m >> k) & 1;
    }
    check(all == 0xFF && bits == 8, "8 variant masks partition the 8 orientations");

    // End-to-end: symmetric plus, seed with real bedrock, brute vs service.
    auto svc = make_service();
    const int y = -60, cx = -700, cz = 900;
    Pattern fp = world_patch(555, y, cx, cz, 5, 5);  // 5x5 real config (asymmetric)
    SearchRequest req;
    req.seed = 555;
    req.plane_y = y;
    req.pattern = fp;
    req.region = Region::centered(cx + 2, cz + 2, 350);
    req.all_orientations = true;
    auto got = run(svc, req);
    auto want = brute(req);
    check(as_map(got) == want, "plan search == brute (" + std::to_string(got.size()) + " vs " +
                                  std::to_string(want.size()) + ")");

    req.pattern = world_patch(555, y, cx, cz, kMaxDim + 1, kMaxDim + 1);
    check(wait_for(svc, svc.submit(req), JobState::done).state == JobState::error,
          "a pattern bigger than 32x32 is refused");
}

}  // namespace

int main() {
    test_roundtrip_and_equiv();
    test_tiling_invariant();
    test_bedrock_equiv();
    test_search_plan();
    test_format_duration();
    test_cancel();
    test_scheduler_checkpoint();
    test_scheduler_spiral();
    test_scheduler_map();
    test_service_map();
    test_stop_at_first();
    test_pause_resume();
    test_checkpoint_text();
    test_pattern_io_roundtrip();
    return report();
}
