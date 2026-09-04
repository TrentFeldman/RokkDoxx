#include "workers.hpp"

#include <algorithm>
#include <cstdlib>
#include <stdexcept>
#include <thread>
#include <tuple>
#include <utility>

#include "gen/bedrock_core.h"

#ifdef ROKK_ENABLE_OPENCL
#include "opencl_worker.hpp"
#endif

namespace rokkdoxx::svc {

// --- search plan ----------------------------------------------------------

SearchPlan build_search_plan(std::vector<KnownCell> knowns, std::uint32_t threshold,
                             bool all_orientations) {
    SearchPlan plan;
    const bool bedrock_rare = threshold <= (1u << 23);
    const std::uint8_t rare_want = bedrock_rare ? 1 : 0;

    // Nothing constrains the search -> "match any bedrock block".
    if (knowns.empty()) {
        plan.n_cells = 1;
        plan.n_variants = 1;
        plan.want = {1};
        plan.off_x = {0};
        plan.off_z = {0};
        plan.variant_mask = {static_cast<std::uint8_t>(all_orientations ? 0xFF : 0x01)};
        return plan;
    }

    // Fail-fast order: rarer cell type first, so a wrong candidate is usually
    // rejected on cell 1 (of each variant).
    std::stable_sort(knowns.begin(), knowns.end(), [&](const KnownCell& a, const KnownCell& b) {
        return ((a.want == 1) == bedrock_rare) && ((b.want == 1) != bedrock_rare);
    });

    // Anchor = the rare-type cell nearest the centroid of all knowns, so the
    // match coordinate lands roughly in the middle of the shape. Falls back to
    // the overall-nearest cell when the pattern has no rare-type cell.
    double cx = 0, cz = 0;
    for (const KnownCell& k : knowns) {
        cx += k.i;
        cz += k.j;
    }
    cx /= static_cast<double>(knowns.size());
    cz /= static_cast<double>(knowns.size());
    auto dist2 = [&](const KnownCell& k) {
        return (k.i - cx) * (k.i - cx) + (k.j - cz) * (k.j - cz);
    };
    KnownCell anchor = knowns.front();
    for (const KnownCell& k : knowns) {
        const bool kr = k.want == rare_want, ar = anchor.want == rare_want;
        if (kr != ar) {
            if (kr) anchor = k;
            continue;
        }
        const double kd = dist2(k), ad = dist2(anchor);
        if (kd < ad || (kd == ad && std::tie(k.j, k.i) < std::tie(anchor.j, anchor.i))) anchor = k;
    }
    plan.anchor_i = anchor.i;
    plan.anchor_j = anchor.j;

    // Cell order: anchor first, then the rest in the fail-fast order above.
    std::vector<KnownCell> cells;
    cells.reserve(knowns.size());
    cells.push_back(anchor);
    for (const KnownCell& k : knowns)
        if (!(k.i == anchor.i && k.j == anchor.j)) cells.push_back(k);

    plan.n_cells = static_cast<int>(cells.size());
    plan.want.resize(cells.size());
    for (std::size_t c = 0; c < cells.size(); ++c) plan.want[c] = cells[c].want;

    // Recentre each cell on the anchor, apply every orientation, and collapse
    // orientations whose transformed (offset, want) set is identical.
    const int gN = all_orientations ? 8 : 1;
    std::vector<std::vector<std::tuple<int, int, int>>> canon;
    for (int g = 0; g < gN; ++g) {
        const Transform tf = kOrientations[static_cast<std::size_t>(g)];
        std::vector<std::int32_t> ox(cells.size()), oz(cells.size());
        std::vector<std::tuple<int, int, int>> key(cells.size());
        for (std::size_t c = 0; c < cells.size(); ++c) {
            const int di = cells[c].i - anchor.i;
            const int dj = cells[c].j - anchor.j;
            ox[c] = tf.a * di + tf.b * dj;
            oz[c] = tf.c * di + tf.d * dj;
            key[c] = {ox[c], oz[c], cells[c].want};
        }
        std::sort(key.begin(), key.end());

        int found = -1;
        for (std::size_t v = 0; v < canon.size(); ++v)
            if (canon[v] == key) {
                found = static_cast<int>(v);
                break;
            }
        if (found >= 0) {
            plan.variant_mask[static_cast<std::size_t>(found)] |= static_cast<std::uint8_t>(1u << g);
        } else {
            canon.push_back(std::move(key));
            plan.variant_mask.push_back(static_cast<std::uint8_t>(1u << g));
            plan.off_x.insert(plan.off_x.end(), ox.begin(), ox.end());
            plan.off_z.insert(plan.off_z.end(), oz.begin(), oz.end());
        }
    }
    plan.n_variants = static_cast<int>(plan.variant_mask.size());
    return plan;
}

// --- CpuWorker --------------------------------------------------------------

CpuWorker::CpuWorker(unsigned threads) : threads_(threads) {
    if (threads_ == 0) threads_ = std::thread::hardware_concurrency();
    if (threads_ == 0) threads_ = 1;
}

std::string CpuWorker::name() const { return "cpu (" + std::to_string(threads_) + " threads)"; }

void CpuWorker::configure(const WorkerConfig& cfg) {
    cfg_ = cfg;
    plan_ = build_search_plan(cfg.knowns, cfg.threshold, cfg.all_orientations);
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
    const SearchPlan& p = plan_;
    const int nc = p.n_cells;

    // Thread t handles every T-th row of the tile (z = t, t+T, t+2T, ...).
    // (x, z) is the candidate world position of the anchor cell (plan cell 0).
    auto work = [&](int t) {
        auto& out = buckets[static_cast<std::size_t>(t)];
        for (int dz = t; dz < tile.h; dz += T) {
            const std::int64_t z = tile.z0 + dz;
            for (int dx = 0; dx < tile.w; ++dx) {
                const std::int64_t x = tile.x0 + dx;

                // Shared anchor: one test rejects every orientation at once.
                const int abed = rk_bits24_at(dlo, dhi, static_cast<int>(x), y,
                                              static_cast<int>(z)) < thr;
                if (abed != static_cast<int>(p.want[0])) continue;

                std::uint8_t mask = 0;
                for (int v = 0; v < p.n_variants; ++v) {
                    const std::int32_t* ox = &p.off_x[static_cast<std::size_t>(v) * nc];
                    const std::int32_t* oz = &p.off_z[static_cast<std::size_t>(v) * nc];
                    bool ok = true;
                    for (int c = 1; c < nc; ++c) {
                        const int bed = rk_bits24_at(dlo, dhi, static_cast<int>(x + ox[c]), y,
                                                     static_cast<int>(z + oz[c])) < thr;
                        if (bed != static_cast<int>(p.want[c])) {
                            ok = false;
                            break;  // first mismatch -> this variant fails
                        }
                    }
                    if (ok) mask |= p.variant_mask[static_cast<std::size_t>(v)];
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
