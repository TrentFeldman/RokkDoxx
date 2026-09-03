#include "workers.hpp"

#include <algorithm>
#include <cstdlib>
#include <stdexcept>
#include <thread>

#include "gen/bedrock_core.h"

#ifdef ROKK_ENABLE_OPENCL
#include "opencl_worker.hpp"
#endif

namespace rokkdoxx::svc {

// --- CpuWorker --------------------------------------------------------------

CpuWorker::CpuWorker(unsigned threads) : threads_(threads) {
    if (threads_ == 0) threads_ = std::thread::hardware_concurrency();
    if (threads_ == 0) threads_ = 1;
}

std::string CpuWorker::name() const { return "cpu (" + std::to_string(threads_) + " threads)"; }

void CpuWorker::configure(const WorkerConfig& cfg) {
    cfg_ = cfg;
    ordered_ = cfg.knowns;
    // Fail-fast ordering: check the rarer cell type first, so a non-matching
    // origin is usually rejected on its first cell. Bedrock is the minority
    // when the threshold sits below the half-way mark (2^23).
    const bool bedrock_rare = cfg.threshold <= (1u << 23);
    std::stable_sort(ordered_.begin(), ordered_.end(),
                     [&](const KnownCell& a, const KnownCell& b) {
                         const bool ar = (a.want == 1) == bedrock_rare;
                         const bool br = (b.want == 1) == bedrock_rare;
                         return ar && !br;
                     });
    truncated_.store(false);
    emitted_.store(0);
}

std::vector<Match> CpuWorker::run_tile(const Tile& tile) {
    const int T = static_cast<int>(threads_);
    // Each thread writes into its own bucket, so there is no locking on the hot
    // path; the buckets are concatenated at the end.
    std::vector<std::vector<Match>> buckets(static_cast<std::size_t>(T));

    const std::uint64_t dlo = cfg_.derived_lo, dhi = cfg_.derived_hi;
    const int y = cfg_.plane_y;
    const std::uint32_t thr = cfg_.threshold;
    const int gN = cfg_.all_orientations ? 8 : 1;

    // Thread t handles every T-th row of the tile (z = t, t+T, t+2T, ...).
    auto work = [&](int t) {
        auto& out = buckets[static_cast<std::size_t>(t)];
        for (int dz = t; dz < tile.h; dz += T) {
            const std::int64_t z = tile.z0 + dz;
            for (int dx = 0; dx < tile.w; ++dx) {
                const std::int64_t x = tile.x0 + dx;
                std::uint8_t mask = 0;
                for (int g = 0; g < gN; ++g) {
                    const Transform tf = kOrientations[static_cast<std::size_t>(g)];
                    bool ok = true;
                    for (const KnownCell& kc : ordered_) {
                        const int du = tf.a * kc.i + tf.b * kc.j;
                        const int dv = tf.c * kc.i + tf.d * kc.j;
                        const int bed = rk_bits24_at(dlo, dhi, static_cast<int>(x + du), y,
                                                     static_cast<int>(z + dv)) < thr;
                        if (bed != static_cast<int>(kc.want)) {
                            ok = false;
                            break;  // first mismatch -> this orientation fails
                        }
                    }
                    if (ok) mask |= static_cast<std::uint8_t>(1u << g);
                }
                if (mask) {
                    if (emitted_.fetch_add(1) < cfg_.match_cap)
                        out.push_back({static_cast<int>(x), static_cast<int>(z), mask});
                    else
                        truncated_.store(true);
                }
            }
        }
    };

    std::vector<std::thread> pool;
    pool.reserve(static_cast<std::size_t>(T));
    for (int t = 0; t < T; ++t) pool.emplace_back(work, t);
    for (auto& th : pool) th.join();

    std::vector<Match> merged;
    for (auto& b : buckets) merged.insert(merged.end(), b.begin(), b.end());
    return merged;
}

// --- backend selection ----------------------------------------------------

std::vector<BackendInfo> list_backends() {
    std::vector<BackendInfo> out;
    {
        BackendInfo cpu;
        cpu.id = "cpu";
        cpu.label = CpuWorker().name();
        cpu.units = static_cast<int>(std::thread::hardware_concurrency());
        out.push_back(std::move(cpu));
    }
#ifdef ROKK_ENABLE_OPENCL
    for (const OpenclDevice& d : opencl_list_devices()) {
        BackendInfo b;
        b.id = "opencl:" + std::to_string(d.index);
        b.label = d.label;
        b.is_gpu = true;
        b.version = d.cl_version;
        b.driver = d.driver_version;
        b.units = d.compute_units;
        out.push_back(std::move(b));
    }
#endif
    return out;
}

WorkerFactory make_worker_factory(const std::string& id) {
    std::string want = id.empty() ? "auto" : id;

#ifdef ROKK_ENABLE_OPENCL
    if (want == "auto") {
        auto devs = opencl_list_devices();
        if (!devs.empty()) want = "opencl:" + std::to_string(devs.front().index);
    }
    if (want.rfind("opencl:", 0) == 0) {
        const int idx = std::atoi(want.c_str() + 7);
        return [idx] { return std::make_unique<OpenclWorker>(idx); };
    }
#else
    if (want.rfind("opencl", 0) == 0)
        throw std::runtime_error("this build has no OpenCL support (rebuild with ROKK_ENABLE_OPENCL)");
#endif

    if (want == "auto" || want == "cpu") return [] { return std::make_unique<CpuWorker>(); };

    throw std::runtime_error("unknown backend: " + id);
}

}  // namespace rokkdoxx::svc
