#include "workers.hpp"

#include <algorithm>
#include <cstdlib>
#include <stdexcept>
#include <thread>
#include <tuple>
#include <utility>

#include "gen/bedrock.hpp"
#include "gen/bedrock_core.h"

#ifdef ROKK_ENABLE_OPENCL
#include "opencl_worker.hpp"
#endif

namespace rokkdoxx::svc {

// --- search prep ----------------------------------------------------------

Pattern world_patch(std::int64_t seed, int y, int x0, int z0, int w, int h, Edition edition) {
    const BedrockGenerator gen(seed, edition);
    Pattern p;
    p.w = w;
    p.h = h;
    p.cells.resize(static_cast<std::size_t>(w) * h);
    for (int j = 0; j < h; ++j)
        for (int i = 0; i < w; ++i)
            p.cells[static_cast<std::size_t>(j) * w + i] =
                gen.is_bedrock_floor(x0 + i, y, z0 + j) ? Cell::bedrock : Cell::not_bedrock;
    return p;
}

WorkerConfig worker_config(std::int64_t seed, int plane_y, Edition edition) {
    const BedrockGenerator gen(seed, edition);
    WorkerConfig cfg;
    cfg.edition = edition;
    cfg.derived_lo = gen.derived_lo();
    cfg.derived_hi = gen.derived_hi();
    cfg.plane_y = plane_y;
    cfg.threshold = gen.threshold(plane_y);
    return cfg;
}

SearchPlan build_search_plan(std::vector<KnownCell> knowns, std::uint32_t threshold,
                             bool all_orientations) {
    SearchPlan plan;
    const bool bedrock_rare = threshold <= (1u << 23);
    const std::uint8_t rare_want = bedrock_rare ? 1 : 0;

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

    auto java_bed = [&](std::int64_t x, std::int64_t z) {
        return static_cast<int>(rk_bits24_at(dlo, dhi, static_cast<int>(x), y, static_cast<int>(z)) < thr);
    };

    // Bedrock Edition makes a whole chunk at a time, so draw every chunk the
    // pattern can reach from this tile first: 16 row masks each (bedrock_core.h).
    std::vector<std::uint32_t> chunks;
    int c0x = 0, c0z = 0, ncx = 0;
    if (cfg_.edition == Edition::bedrock) {
        const auto [lox, hix] = std::minmax_element(p.off_x.begin(), p.off_x.end());
        const auto [loz, hiz] = std::minmax_element(p.off_z.begin(), p.off_z.end());
        c0x = rk_be_coord(static_cast<int>(tile.x0 + *lox)) >> 4;
        c0z = rk_be_coord(static_cast<int>(tile.z0 + *loz)) >> 4;
        ncx = (rk_be_coord(static_cast<int>(tile.x0 + tile.w - 1 + *hix)) >> 4) - c0x + 1;
        const int ncz = (rk_be_coord(static_cast<int>(tile.z0 + tile.h - 1 + *hiz)) >> 4) - c0z + 1;
        chunks.resize(static_cast<std::size_t>(ncx) * ncz * 16);
        std::vector<std::thread> fill;
        for (int t = 0; t < T; ++t)
            fill.emplace_back([&, t] {
                for (int j = t; j < ncz; j += T)
                    for (int i = 0; i < ncx; ++i)
                        rk_be_chunk_rows(c0x + i, c0z + j, y + 63,
                                         &chunks[(static_cast<std::size_t>(j) * ncx + i) * 16]);
            });
        for (auto& th : fill) th.join();
    }
    auto be_bed = [&](std::int64_t x, std::int64_t z) {
        const int sx = rk_be_coord(static_cast<int>(x)), sz = rk_be_coord(static_cast<int>(z));
        const std::size_t c = static_cast<std::size_t>((sz >> 4) - c0z) * ncx + ((sx >> 4) - c0x);
        return static_cast<int>((chunks[c * 16 + (sz & 15)] >> (sx & 15)) & 1u);
    };

    // Thread t handles every T-th row of the tile (z = t, t+T, t+2T, ...).
    // (x, z) is the candidate world position of the anchor cell (plan cell 0).
    auto work = [&](int t, auto bed) {
        auto& out = buckets[static_cast<std::size_t>(t)];
        for (int dz = t; dz < tile.h; dz += T) {
            const std::int64_t z = tile.z0 + dz;
            for (int dx = 0; dx < tile.w; ++dx) {
                const std::int64_t x = tile.x0 + dx;

                // Shared anchor: one test rejects every orientation at once.
                const int abed = bed(x, z);
                if (abed != static_cast<int>(p.want[0])) continue;

                std::uint8_t mask = 0;
                for (int v = 0; v < p.n_variants; ++v) {
                    const std::int32_t* ox = &p.off_x[static_cast<std::size_t>(v) * nc];
                    const std::int32_t* oz = &p.off_z[static_cast<std::size_t>(v) * nc];
                    bool ok = true;
                    for (int c = 1; c < nc; ++c) {
                        if (bed(x + ox[c], z + oz[c]) != static_cast<int>(p.want[c])) {
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
    for (int t = 0; t < T; ++t)
        if (cfg_.edition == Edition::bedrock) pool.emplace_back([&, t] { work(t, be_bed); });
        else pool.emplace_back([&, t] { work(t, java_bed); });
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
        cpu.units = static_cast<int>(std::max(1u, std::thread::hardware_concurrency()));  // as CpuWorker
        cpu.label = "cpu (" + std::to_string(cpu.units) + " threads)";
        out.push_back(std::move(cpu));
    }
#ifdef ROKK_ENABLE_OPENCL
    for (const OpenclDevice& d : opencl_list_devices()) {
        BackendInfo b;
        b.id = "opencl:" + std::to_string(d.index);
        b.label = d.label;
        b.is_gpu = !d.is_cpu;
        b.integrated = d.integrated;
        b.version = d.cl_version;
        b.driver = d.driver_version;
        b.units = d.compute_units;
        out.push_back(std::move(b));
    }
#endif
    return out;
}

BackendInfo resolve_backend(const std::string& id) {
    const std::string want = id.empty() ? "auto" : id;
    const std::vector<BackendInfo> all = list_backends();
    auto first = [&](auto pred) -> const BackendInfo* {
        auto it = std::find_if(all.begin(), all.end(), pred);
        return it == all.end() ? nullptr : &*it;
    };
    auto dedicated = [](const BackendInfo& b) { return b.is_gpu && !b.integrated; };
    auto integrated = [](const BackendInfo& b) { return b.is_gpu && b.integrated; };

    const BackendInfo* b = nullptr;
    if (want == "auto") {
        b = first(dedicated);
        if (!b) b = first(integrated);
        if (!b) b = &all.front();  // the cpu entry
    } else if (want == "dedicated") {
        b = first(dedicated);
    } else if (want == "integrated") {
        b = first(integrated);
    } else {
        b = first([&](const BackendInfo& x) { return x.id == want; });
    }
    if (b) return *b;

#ifndef ROKK_ENABLE_OPENCL
    if (want.rfind("opencl", 0) == 0 || want == "dedicated" || want == "integrated")
        throw std::runtime_error("this build has no OpenCL support (rebuild with ROKK_ENABLE_OPENCL)");
#endif
    if (want == "dedicated" || want == "integrated")
        throw std::runtime_error("no " + want + " GPU found (see --list-backends)");
    throw std::runtime_error("unknown backend: " + id + " (see --list-backends)");
}

WorkerFactory make_worker_factory(const std::string& id) {
    const BackendInfo b = resolve_backend(id);
    if (b.id == "cpu") return [] { return std::make_unique<CpuWorker>(); };
#ifdef ROKK_ENABLE_OPENCL
    const int idx = std::atoi(b.id.c_str() + 7);  // "opencl:N"
    return [idx] { return std::make_unique<OpenclWorker>(idx); };
#else
    throw std::runtime_error("unknown backend: " + id);
#endif
}

}  // namespace rokkdoxx::svc
