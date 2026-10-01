// OpenCL bit-exactness + search parity. Requires a working OpenCL device; if
// none is present the test prints a note and passes (so CI without a GPU is
// green -- run it on the real device to actually exercise the kernel).
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <algorithm>
#include <map>
#include <numeric>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "gen/bedrock.hpp"
#include "svc/opencl_worker.hpp"
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

struct Case {
    std::int64_t seed;
    int x0, z0, w, h;
};
// Same adversarial coordinates as tests/diff_test.py.
const std::vector<Case> kCases = {
    {0, 0, 0, 48, 48},
    {1, 0, 0, 64, 40},
    {-1, -32, -32, 40, 40},
    {3257840388504953787LL, -1000, -1000, 50, 50},
    {-4000000000000LL, 29999900, -29999900, 24, 24},
    {42, 2147483000, -2147483000, 16, 16},
    {9223372036854775807LL, 123, -456, 33, 17},
    {-9223372036854775807LL - 1, -7, 9, 20, 20},
};

void test_dump_bit_exact(OpenclWorker& w) {
    std::printf("test_dump_bit_exact\n");
    for (const Case& c : kCases) {
        rokkdoxx::BedrockGenerator gen(c.seed);
        for (int y = -63; y <= -60; ++y) {
            const std::uint32_t thr = gen.threshold(y);
            auto gpu = w.dump_plane(gen.derived_lo(), gen.derived_hi(), y, thr, c.x0, c.z0, c.w, c.h);
            bool ok = true;
            for (int j = 0; j < c.h && ok; ++j)
                for (int i = 0; i < c.w && ok; ++i) {
                    const bool cpu = gen.is_bedrock_floor(c.x0 + i, y, c.z0 + j);
                    if (static_cast<bool>(gpu[static_cast<std::size_t>(j) * c.w + i]) != cpu)
                        ok = false;
                }
            check(ok, "seed " + std::to_string(c.seed) + " y=" + std::to_string(y) + " dump matches");
        }
    }
}

std::map<std::pair<int, int>, std::uint8_t> run(WorkerFactory f, const SearchRequest& req) {
    SearchService svc(std::move(f));
    JobId id = svc.submit(req);
    for (;;) {
        JobStatus st = svc.poll(id);
        if (st.state == JobState::done || st.state == JobState::cancelled) break;
        if (st.state == JobState::error) {
            std::printf("  FAIL: job error: %s\n", st.error.c_str());
            ++g_fail;
            return {};
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(3));
    }
    std::map<std::pair<int, int>, std::uint8_t> m;
    for (auto& x : svc.results(id)) m[{x.x, x.z}] = x.orient_mask;
    return m;
}

void test_search_parity() {
    std::printf("test_search_parity\n");
    struct P {
        std::int64_t seed;
        int y, cx, cz, w, h, radius;
    };
    const std::vector<P> ps = {
        {777, -60, 500, -300, 6, 6, 500},
        {42, -61, -48, 80, 5, 8, 400},
        {3257840388504953787LL, -60, 1200, -800, 7, 5, 350},
    };
    for (const P& p : ps)
        for (bool all : {true, false}) {
            rokkdoxx::BedrockGenerator gen(p.seed);
            SearchRequest req;
            req.seed = p.seed;
            req.plane_y = p.y;
            req.pattern.w = p.w;
            req.pattern.h = p.h;
            req.pattern.cells.assign(static_cast<std::size_t>(p.w) * p.h, Cell::unknown);
            for (int j = 0; j < p.h; ++j)
                for (int i = 0; i < p.w; ++i)
                    req.pattern.cells[static_cast<std::size_t>(j) * p.w + i] =
                        gen.is_bedrock_floor(p.cx + i, p.y, p.cz + j) ? Cell::bedrock : Cell::not_bedrock;
            req.region = Region::centered(p.cx + 1, p.cz + 1, p.radius);
            req.all_orientations = all;
            const std::string tag = "seed " + std::to_string(p.seed) + (all ? " all-8" : " exact");

            auto cpu = run(make_worker_factory("cpu"), req);
            auto gpu = run([] { return std::make_unique<OpenclWorker>(0); }, req);
            check(cpu == gpu, tag + " parity (" + std::to_string(cpu.size()) + " cpu vs " +
                              std::to_string(gpu.size()) + " gpu)");
            // Matches report the anchor cell's world position, so the identity
            // orientation lands at the fill origin + the anchor offset.
            const SearchPlan plan = build_search_plan(req.pattern.knowns(), gen.threshold(p.y), true);
            bool origin = false;
            for (auto& [k, v] : gpu)
                if (k.first == p.cx + plan.anchor_i && k.second == p.cz + plan.anchor_j && (v & 1))
                    origin = true;
            check(origin, tag + " fill origin found on GPU");
        }
}

// The kernel is rebuilt per pattern with its first cells baked in as literals, so
// the pattern *is* part of the code: check many shapes against the CPU. 1..12 known
// cells straddle the baked-in prefix (6), random layouts, all-8 and exact, mixed
// layers. Fixed seed so a failure reproduces. Cells come from the real world, so
// there is always at least the planted match.
void test_random_patterns() {
    std::printf("test_random_patterns\n");
    std::mt19937_64 rng(20260930);
    for (int t = 0; t < 24; ++t) {
        const int k = t % 12 + 1, w = 8, h = 8;
        const std::int64_t seed = static_cast<std::int64_t>(rng());
        const int y = -63 + static_cast<int>(rng() % 4), cx = static_cast<int>(rng() % 20001) - 10000,
                  cz = static_cast<int>(rng() % 20001) - 10000;
        rokkdoxx::BedrockGenerator gen(seed);
        std::vector<int> cells(w * h);
        std::iota(cells.begin(), cells.end(), 0);
        std::shuffle(cells.begin(), cells.end(), rng);

        SearchRequest req;
        req.seed = seed;
        req.plane_y = y;
        req.pattern.w = w;
        req.pattern.h = h;
        req.pattern.cells.assign(static_cast<std::size_t>(w) * h, Cell::unknown);
        for (int n = 0; n < k; ++n) {
            const int i = cells[n] % w, j = cells[n] / w;
            req.pattern.cells[cells[n]] =
                gen.is_bedrock_floor(cx + i, y, cz + j) ? Cell::bedrock : Cell::not_bedrock;
        }
        req.region = Region::centered(cx + 4, cz + 4, 250);
        req.all_orientations = t % 3 != 0;
        const std::string tag = "random #" + std::to_string(t) + " (" + std::to_string(k) + " cells, y " +
                                std::to_string(y) + (req.all_orientations ? ", all-8)" : ", exact)");
        auto cpu = run(make_worker_factory("cpu"), req);
        auto gpu = run([] { return std::make_unique<OpenclWorker>(0); }, req);
        check(!cpu.empty() && cpu == gpu, tag + " parity (" + std::to_string(cpu.size()) + " cpu vs " +
                                              std::to_string(gpu.size()) + " gpu)");
    }
}

// Three known cells spread `reach` blocks apart inside an otherwise-unknown
// grid. reach 200 runs the bit-plane kernels with a big halo; reach 1025 is
// past OpenclWorker's kPlaneHaloMax, so it exercises the search_tile fallback.
void test_large_halo(int reach) {
    std::printf("test_large_halo(%d)\n", reach);
    const std::int64_t seed = 999;
    rokkdoxx::BedrockGenerator gen(seed);
    const int y = -60;
    const int cx = 300, cz = -200;
    const int n = 2 * reach + 1;  // grid spans 0..2*reach, centre at (reach,reach)

    Pattern pat;
    pat.w = n;
    pat.h = n;
    pat.cells.assign(static_cast<std::size_t>(n) * n, Cell::unknown);
    auto set = [&](int i, int j) {
        pat.cells[static_cast<std::size_t>(j) * n + i] =
            gen.is_bedrock_floor(cx + i, y, cz + j) ? Cell::bedrock : Cell::not_bedrock;
    };
    set(reach, reach);      // centre
    set(0, reach);          // `reach` blocks west
    set(2 * reach, reach);  // `reach` blocks east

    SearchRequest req;
    req.seed = seed;
    req.plane_y = y;
    req.pattern = pat;
    req.region = Region::centered(cx + 1, cz + 1, 500);
    req.all_orientations = true;

    auto cpu = run(make_worker_factory("cpu"), req);
    auto gpu = run([] { return std::make_unique<OpenclWorker>(0); }, req);
    check(cpu == gpu, "large-halo parity (" + std::to_string(cpu.size()) + " cpu vs " +
                          std::to_string(gpu.size()) + " gpu)");
}

}  // namespace

int main() {
    auto devs = opencl_list_devices();
    if (devs.empty()) {
        std::printf("no OpenCL device available -- skipping GPU tests (this is not a failure)\n");
        return 0;
    }
    std::printf("OpenCL device: %s\n", devs.front().label.c_str());
    try {
        OpenclWorker w(0);
        test_dump_bit_exact(w);
    } catch (const std::exception& e) {
        std::printf("  FAIL: OpenclWorker init: %s\n", e.what());
        ++g_fail;
    }
    test_search_parity();
    test_random_patterns();
    test_large_halo(200);
    test_large_halo(1025);

    if (g_fail == 0) {
        std::printf("\nALL PASS\n");
        return 0;
    }
    std::printf("\n%d FAILURE(S)\n", g_fail);
    return 1;
}
