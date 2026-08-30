// OpenCL bit-exactness + search parity. Requires a working OpenCL device; if
// none is present the test prints a note and passes (so CI without a GPU is
// green -- run it on the real device to actually exercise the kernel).
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <map>
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
    for (const P& p : ps) {
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
        req.all_orientations = true;

        auto cpu = run(make_worker_factory("cpu"), req);
        auto gpu = run([] { return std::make_unique<OpenclWorker>(0); }, req);
        check(cpu == gpu, "seed " + std::to_string(p.seed) + " parity (" + std::to_string(cpu.size()) +
                              " cpu vs " + std::to_string(gpu.size()) + " gpu)");
        bool origin = false;
        for (auto& [k, v] : gpu)
            if (k.first == p.cx && k.second == p.cz && (v & 1)) origin = true;
        check(origin, "seed " + std::to_string(p.seed) + " fill origin found on GPU");
    }
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

    if (g_fail == 0) {
        std::printf("\nALL PASS\n");
        return 0;
    }
    std::printf("\n%d FAILURE(S)\n", g_fail);
    return 1;
}
