// rokksearch -- headless client for the RokkDoxx search service.
//
//   rokksearch --seed <s> --y -60 --pattern p.txt --center 0,0 --radius 2000000
//   (see --help for all options)
//
// Streams a progress line to stderr; prints matches ("x z mask") to stdout.
#include <csignal>
#include <algorithm>
#include <chrono>
#include <cinttypes>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "gen/bedrock.hpp"
#include "gen/bedrock_core.h"  // rk_mix_stafford13 (benchmark digest), rk_be_coord (demo)
#include "svc/client.hpp"
#include "svc/pattern_io.hpp"
#include "svc/search_service.hpp"  // TileScheduler, for the benchmark
#include "svc/service_types.hpp"
#include "svc/workers.hpp"

using namespace rokkdoxx::svc;

namespace {

[[noreturn]] void usage(int code) {
    std::fprintf(code ? stderr : stdout,
                 "usage: rokksearch [options]\n"
                 "  --edition <e>         java (default) or bedrock; Bedrock Edition's floor is the\n"
                 "                        same in every world, so it needs no --seed\n"
                 "  --seed <s>            world seed (numeric or text)\n"
                 "  --y <n>               bedrock plane, -64..-59 (default -60)\n"
                 "  --size <WxH>          pattern size when not loading a file\n"
                 "  --pattern <file>      load a .txt pattern (sets edition/seed/y/center/radius too);\n"
                 "                        a session saved by rokktui (`s`) also resumes from its\n"
                 "                        progress file\n"
                 "  --center <x,z>        search-region center\n"
                 "  --radius <r>          search-region half-extent (blocks); -1 = whole world\n"
                 "  --region <x0,x1,z0,z1> explicit search region (overrides center/radius)\n"
                 "  --orientations <all|exact>\n"
                 "  --first               stop at the first match (the search scans outward from\n"
                 "                        the middle of the region, so this finds the nearest)\n"
                 "  --cap <n>             max matches to keep (default 1048576)\n"
                 "  --tile <n>            tile side floor (default 4096)\n"
                 "  --checkpoint <file>   resumable progress file\n"
                 "  --backend <id>        cpu | dedicated | integrated | opencl:N | auto (default auto:\n"
                 "                        dedicated GPU, else integrated GPU, else cpu)\n"
                 "  --json               machine-readable output\n"
                 "  --bench              measure the search you asked for (rate only)\n"
                 "  --benchmark          run the standard reproducible benchmark\n"
                 "                       (fixed workload; ignores --seed/--pattern/--region;\n"
                 "                       --edition bedrock benchmarks Bedrock Edition's floor)\n"
                 "  --benchmark-seconds <f>  target seconds per phase (default 2.0)\n"
                 "  --benchmark-iters <n>    measured iterations per phase (default 5)\n"
                 "  --benchmark-long <min>   sustained all-8 load for <min> minutes (15 = the\n"
                 "                           standard); checks every ~30 s sweep returns\n"
                 "                           identical matches (exit 1 if not)\n"
                 "  --demo <time>         write a pattern file to try without Minecraft: a random\n"
                 "                        seed and spot, sized so searching for it takes about <time>\n"
                 "                        on this machine: 90 or 90s, 5m, 2h; 'max' = the whole world\n"
                 "                        (uses --edition/--seed/--y/--orientations/--backend if given)\n"
                 "  --out <file>          where --demo writes (default demo_pattern.txt)\n"
                 "  --list-backends\n");
    std::exit(code);
}

bool parse2(const char* s, long long& a, long long& b, char sep) {
    const char* p = std::strchr(s, sep);
    if (!p) return false;
    a = std::atoll(std::string(s, p).c_str());
    b = std::atoll(p + 1);
    return true;
}

// "90" / "90s" / "5m" / "1.5h" -> seconds; "max" -> `max` (the whole world).
bool parse_budget(const char* text, double& seconds, bool& max) {
    max = std::strcmp(text, "max") == 0;
    if (max) {
        seconds = 0;
        return true;
    }
    char* end = nullptr;
    const double v = std::strtod(text, &end);
    const double unit = *end == '\0' || std::strcmp(end, "s") == 0 ? 1
                        : std::strcmp(end, "m") == 0               ? 60
                        : std::strcmp(end, "h") == 0               ? 3600
                                                                   : 0;
    seconds = v * unit;
    return end != text && unit > 0 && seconds > 0;
}

// ===========================================================================
// --benchmark : a fixed, reproducible workload so numbers compare across
// machines. It drives a Worker directly (no per-job kernel rebuild, no service
// overhead) and reports candidate-origins/second for the exact and all-8 cases.
// ===========================================================================
namespace bmark {

constexpr std::int64_t kSeed = 0;
constexpr int kPlaneY = -60;
constexpr int kBenchVersion = 2;
constexpr long long kMaxCandidates = 500'000'000'000LL;  // clamp per phase

// The fixed workload pattern: a 6x6 patch (a typical real size) filled from the
// generator at a fixed origin, so it is a genuine bedrock configuration for
// `kSeed`. It matches well under once per million candidates, so result
// collection stays negligible and the search does representative work. This shape has 8 distinct D4
// orientations -- the all-8 worst case.
constexpr int kPatW = 6, kPatH = 6, kFillX = 137, kFillZ = -251;

inline Pattern standard_pattern(Edition edition) {
    return world_patch(kSeed, kPlaneY, kFillX, kFillZ, kPatW, kPatH, edition);
}

// A deliberately D4-symmetric pattern: a bedrock "plus" centred in a 5x5 grid.
// Recentred on its middle cell (the rare anchor) the constraint set maps to
// itself under every rotation and mirror, so all 8 orientations collapse to
// one -- the all-8 best case. With duplicate-orientation collapse it should run
// at the exact-orientation rate.
inline Pattern symmetric_pattern() {
    constexpr int n = 5;
    Pattern p;
    p.w = n;
    p.h = n;
    p.cells.assign(static_cast<std::size_t>(n) * n, Cell::unknown);
    auto set = [&](int i, int j) { p.cells[static_cast<std::size_t>(j) * n + i] = Cell::bedrock; };
    set(2, 2);                                  // centre
    set(2, 0); set(2, 4); set(0, 2); set(4, 2); // arms
    return p;
}

// Order-independent fingerprint of a sweep's matches: tiles and GPU atomics
// finish in any order, so the hash is a sum of per-match mixes.
struct Digest {
    std::uint64_t count = 0, hash = 0;
    bool operator==(const Digest&) const = default;
};

// One timed sweep of `region` through an already-configured worker. Mirrors
// SearchService::run()'s pump loop (two tiles in flight) so the benchmark
// measures the overlapped path the service ships with.
inline double sweep(Worker& w, const Region& region, int tile_side, Digest* d = nullptr) {
    TileScheduler sched(region, tile_side);
    Tile t;
    const auto t0 = std::chrono::steady_clock::now();
    constexpr int kDepth = 2;
    int inflight = 0;
    bool more = true;
    while (true) {
        while (more && inflight < kDepth) {
            if (!sched.next(t)) {
                more = false;
                break;
            }
            w.begin_tile(t);
            ++inflight;
        }
        if (inflight == 0) break;
        const std::vector<Match> m = w.end_tile();
        --inflight;
        if (d)
            for (const Match& x : m) {
                const std::uint64_t key = (static_cast<std::uint64_t>(static_cast<std::uint32_t>(x.x)) << 32) |
                                          static_cast<std::uint32_t>(x.z);
                ++d->count;
                d->hash += rk_mix_stafford13(rk_mix_stafford13(key) ^ x.orient_mask);
            }
    }
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
}

inline long long candidates_of(long r) {
    const long long side = 2LL * r + 1;
    return side * side;
}

// radius whose region has ~`cand` candidate origins, clamped to sane bounds.
inline long radius_for(double cand, double cap = static_cast<double>(kMaxCandidates)) {
    if (cand > cap) cand = cap;
    long r = static_cast<long>((std::sqrt(cand) - 1.0) / 2.0);
    return r < 1000 ? 1000 : r;
}

// Extrapolate from one measured sweep to the radius that should take `target_s`.
inline long calibrate_from(long r, double elapsed, double target_s,
                           double cap = static_cast<double>(kMaxCandidates)) {
    if (elapsed < 1e-6) return radius_for(cap, cap);
    const double rate = static_cast<double>(candidates_of(r)) / elapsed;  // cand / s
    return radius_for(rate * target_s, cap);
}

// Warm up, then size the workload: a radius-3000 probe, then one sweep per target,
// each extrapolating the radius that takes that many seconds.
inline long calibrate(Worker& w, int tile_side, std::initializer_list<double> targets,
                      double cap = static_cast<double>(kMaxCandidates)) {
    long r = 3000;
    for (double t : targets) r = calibrate_from(r, sweep(w, Region::centered(0, 0, r), tile_side), t, cap);
    return r;
}

struct PhaseResult {
    const char* name;
    int orientations;
    long radius;
    long long candidates;
    double elapsed_s;  // one representative (median) sweep
    double g_median, g_min, g_max;
    int iters;
};

inline PhaseResult run_phase(Worker& w, const char* name, const std::vector<KnownCell>& knowns,
                             bool all_orient, int tile_side, const WorkerConfig& base,
                             double target_s, int iters) {
    WorkerConfig cfg = base;
    cfg.knowns = knowns;
    cfg.all_orientations = all_orient;
    w.configure(cfg);

    const long r = calibrate(w, tile_side, {0.4, target_s, target_s});
    const long long cand = candidates_of(r);
    const Region region = Region::centered(0, 0, r);

    std::vector<double> rates, times;
    for (int i = 0; i < iters; ++i) {
        const double el = sweep(w, region, tile_side);
        times.push_back(el);
        rates.push_back(static_cast<double>(cand) / el / 1e9);
    }
    std::sort(rates.begin(), rates.end());
    std::sort(times.begin(), times.end());

    PhaseResult pr;
    pr.name = name;
    pr.orientations = all_orient ? 8 : 1;
    pr.radius = r;
    pr.candidates = cand;
    pr.elapsed_s = times[times.size() / 2];
    pr.g_median = rates[rates.size() / 2];
    pr.g_min = rates.front();
    pr.g_max = rates.back();
    pr.iters = iters;
    return pr;
}

inline const char* host_os() {
#if defined(_WIN32)
    return "windows";
#elif defined(__APPLE__)
    return "macos";
#elif defined(__linux__)
    return "linux";
#else
    return "unix";
#endif
}
inline const char* host_arch() {
#if defined(__x86_64__) || defined(_M_X64)
    return "x86_64";
#elif defined(__aarch64__) || defined(_M_ARM64)
    return "arm64";
#elif defined(__i386__) || defined(_M_IX86)
    return "x86";
#else
    return "unknown";
#endif
}
inline std::string host_compiler() {
    char buf[64];
#if defined(__clang__)
    std::snprintf(buf, sizeof buf, "clang %d.%d.%d", __clang_major__, __clang_minor__,
                  __clang_patchlevel__);
#elif defined(__GNUC__)
    std::snprintf(buf, sizeof buf, "gcc %d.%d.%d", __GNUC__, __GNUC_MINOR__, __GNUC_PATCHLEVEL__);
#elif defined(_MSC_VER)
    std::snprintf(buf, sizeof buf, "msvc %d", _MSC_VER);
#else
    std::snprintf(buf, sizeof buf, "unknown");
#endif
    return buf;
}

// "seed 0" / "Bedrock Edition": whose floor the benchmark searches.
inline std::string workload_world(Edition edition) {
    return edition == Edition::bedrock ? "Bedrock Edition" : "seed " + std::to_string(kSeed);
}

inline void print_header(const char* title, const BackendInfo& chosen) {
    std::printf("rokksearch %s v%d\n", title, kBenchVersion);
    std::printf("backend   : %s\n", chosen.label.c_str());
    if (chosen.is_gpu)
        std::printf("device    : %s (%s)  |  %s  |  driver %s  |  %d CU\n", chosen.label.c_str(),
                    chosen.integrated ? "integrated" : "dedicated",
                    chosen.version.empty() ? "OpenCL ?" : chosen.version.c_str(),
                    chosen.driver.empty() ? "?" : chosen.driver.c_str(), chosen.units);
    std::printf("host      : %s %s  |  %u threads  |  %s\n", host_os(), host_arch(),
                std::thread::hardware_concurrency(), host_compiler().c_str());
}

// --benchmark-long: sustained load. The all-8 phase's workload, sized to
// ~kLongSweepS per sweep, swept back to back for `minutes`. Every sweep must
// return the identical match set (count + order-independent hash): a device
// that overheats, throttles into errors or flips bits shows up here, not in a
// 2-second phase. The per-sweep rates show clock drift over the run.
constexpr double kLongSweepS = 30.0;

inline int run_long(Worker& w, WorkerConfig cfg, int tile_side, double minutes, bool json,
                    const BackendInfo& chosen) {
    using clock = std::chrono::steady_clock;
    cfg.knowns = standard_pattern(cfg.edition).knowns();
    cfg.all_orientations = true;
    w.configure(cfg);

    // Calibrate the sweep size (capped at the whole world).
    const double world = 4.0 * static_cast<double>(Region::kWorldBorder) * Region::kWorldBorder;
    const long r = calibrate(w, tile_side, {2.0, kLongSweepS}, world);
    const Region region = Region::centered(0, 0, r);
    const long long cand = candidates_of(r);

    // Progress lines go to stderr in --json mode so stdout stays one object.
    FILE* out = json ? stderr : stdout;
    if (!json) {
        print_header("long benchmark", chosen);
        std::printf("workload  : all-8, %dx%d pattern, %s at y %d, r=%ld (%.2e cand/sweep)\n", kPatW, kPatH,
                    workload_world(cfg.edition).c_str(), kPlaneY, r, static_cast<double>(cand));
        std::printf("duration  : %s\n\n", format_duration(minutes * 60).c_str());
    }

    const auto t0 = clock::now();
    std::vector<double> rates;
    Digest first;
    bool consistent = true, capped = false;
    for (int n = 0;; ++n) {
        const double spent = std::chrono::duration<double>(clock::now() - t0).count();
        if (n >= 2 && spent >= minutes * 60) break;
        w.configure(cfg);  // resets the worker's match cap / truncation state
        Digest d;
        const double el = sweep(w, region, tile_side, &d);
        const bool trunc = w.truncated();
        if (n == 0) first = d;
        const bool same = d == first;
        consistent = consistent && same;
        capped = capped || trunc;
        rates.push_back(static_cast<double>(cand) / el / 1e9);
        const double left = minutes * 60 - std::chrono::duration<double>(clock::now() - t0).count();
        std::fprintf(out, "sweep %3d  %7.2f Gcand/s  %8llu matches  %016llx  %s   ETA %s\n", n + 1,
                     rates.back(), static_cast<unsigned long long>(d.count),
                     static_cast<unsigned long long>(d.hash),
                     trunc ? "CAPPED" : same ? "ok" : "MISMATCH",
                     format_duration(left).c_str());
        std::fflush(out);
    }

    std::vector<double> sorted = rates;
    std::sort(sorted.begin(), sorted.end());
    const double drift = (rates.back() / rates.front() - 1.0) * 100.0;
    const double total = std::chrono::duration<double>(clock::now() - t0).count();
    const char* verdict = !consistent ? "INCONSISTENT" : capped ? "CAPPED (hash not comparable)" : "consistent";
    if (json) {
        std::printf("{\"benchmark_version\":%d,\"mode\":\"long\",\"backend\":\"%s\",\"sweeps\":%zu,"
                    "\"elapsed_s\":%.1f,\"candidates_per_sweep\":%lld,\"matches_per_sweep\":%llu,"
                    "\"consistent\":%s,\"capped\":%s,\"gcand_s_first\":%.4f,\"gcand_s_last\":%.4f,"
                    "\"gcand_s_min\":%.4f,\"gcand_s_median\":%.4f,\"gcand_s_max\":%.4f,\"drift_pct\":%.2f}\n",
                    kBenchVersion, chosen.label.c_str(), rates.size(), total, cand,
                    static_cast<unsigned long long>(first.count), consistent ? "true" : "false",
                    capped ? "true" : "false", rates.front(), rates.back(), sorted.front(),
                    sorted[sorted.size() / 2], sorted.back(), drift);
    } else {
        std::printf("\n%zu sweeps in %s: median %.2f Gcand/s (min %.2f, max %.2f), "
                    "last vs first %+.1f%%\nresults: %s\n",
                    rates.size(), format_duration(total).c_str(), sorted[sorted.size() / 2],
                    sorted.front(), sorted.back(), drift, verdict);
    }
    return consistent && !capped ? 0 : 1;
}

// Resolve `backend_arg` to a device and build its worker (a GPU kernel compiles
// here). Returns 0, or the exit code to give up with.
inline int open_worker(const std::string& backend_arg, BackendInfo& chosen,
                       std::unique_ptr<Worker>& worker) {
    try {
        chosen = resolve_backend(backend_arg);
        worker = make_worker_factory(chosen.id)();
    } catch (const std::exception& e) {
        std::fprintf(stderr, "backend error: %s\n", e.what());
        return 1;
    }
    return 0;
}

// Candidates/second for the search `cfg` describes, in about 1.5 s: a warm-up,
// then two calibration rounds (the rate drifts a little with region size).
inline double measure_rate(Worker& w, const WorkerConfig& cfg) {
    w.configure(cfg);
    const int tile_side = std::max(4096, w.preferred_tile_side());
    const long r = calibrate(w, tile_side, {0.4, 1.0});
    return static_cast<double>(candidates_of(r)) / sweep(w, Region::centered(0, 0, r), tile_side);
}

inline int run(const std::string& backend_arg, bool json, double target_s, int iters,
               double long_minutes, Edition edition) {
    if (target_s <= 0.05) target_s = 0.05;
    if (iters < 1) iters = 1;

    BackendInfo chosen;
    std::unique_ptr<Worker> worker;
    if (const int rc = open_worker(backend_arg, chosen, worker)) return rc;

    const std::vector<KnownCell> asym = standard_pattern(edition).knowns();
    const std::vector<KnownCell> sym = symmetric_pattern().knowns();
    const WorkerConfig base = worker_config(kSeed, kPlaneY, edition);

    const int tile_side = std::max(4096, worker->preferred_tile_side());
    if (long_minutes > 0) return run_long(*worker, base, tile_side, long_minutes, json, chosen);

    const PhaseResult ex = run_phase(*worker, "exact", asym, false, tile_side, base, target_s, iters);
    const PhaseResult a8 = run_phase(*worker, "all-8", asym, true, tile_side, base, target_s, iters);
    const PhaseResult a8s =
        run_phase(*worker, "all-8-sym", sym, true, tile_side, base, target_s, iters);
    const PhaseResult phases[3] = {ex, a8, a8s};
    constexpr int kNumPhases = 3;

    const unsigned threads = std::thread::hardware_concurrency();

    if (json) {
        std::printf("{\"benchmark_version\":%d,\"backend\":\"%s\",", kBenchVersion,
                    chosen.label.c_str());
        std::printf("\"device\":{\"is_gpu\":%s,\"integrated\":%s,\"cl_version\":\"%s\","
                    "\"driver\":\"%s\",\"units\":%d},",
                    chosen.is_gpu ? "true" : "false", chosen.integrated ? "true" : "false",
                    chosen.version.c_str(), chosen.driver.c_str(), chosen.units);
        std::printf("\"host\":{\"os\":\"%s\",\"arch\":\"%s\",\"threads\":%u,\"compiler\":\"%s\"},",
                    host_os(), host_arch(), threads, host_compiler().c_str());
        std::printf("\"pattern\":{\"edition\":\"%s\",\"w\":%d,\"h\":%d,\"seed\":%lld,\"y\":%d},",
                    edition == Edition::bedrock ? "bedrock" : "java", kPatW, kPatH,
                    static_cast<long long>(kSeed), kPlaneY);
        std::printf("\"phases\":[");
        for (int i = 0; i < kNumPhases; ++i) {
            const PhaseResult& p = phases[i];
            std::printf("%s{\"name\":\"%s\",\"orientations\":%d,\"candidates\":%lld,"
                        "\"elapsed_s\":%.4f,\"gcand_s_median\":%.4f,\"gcand_s_min\":%.4f,"
                        "\"gcand_s_max\":%.4f,\"iters\":%d}",
                        i ? "," : "", p.name, p.orientations, p.candidates, p.elapsed_s, p.g_median,
                        p.g_min, p.g_max, p.iters);
        }
        std::printf("]}\n");
        return 0;
    }

    print_header("benchmark", chosen);
    std::printf("pattern   : %dx%d asymmetric + 5x5 symmetric plus, %s at y %d\n\n", kPatW, kPatH,
                workload_world(edition).c_str(), kPlaneY);
    for (const PhaseResult& p : phases) {
        std::printf("%-9s : %7.2f Gcand/s  (median of %d; min %.2f, max %.2f)   "
                    "r=%ld, %.2e cand, %.2f s\n",
                    p.name, p.g_median, p.iters, p.g_min, p.g_max, p.radius,
                    static_cast<double>(p.candidates), p.elapsed_s);
    }
    std::printf("\nG = 10^9 candidate origins checked per second.\n");
    return 0;
}

}  // namespace bmark

// ===========================================================================
// --demo <seconds> : a search you can try without opening Minecraft. A random
// seed and a random spot in a region centred on 0,0; the pattern is copied from
// that spot (so it really is there); the region is sized from this machine's
// measured rate so the search takes about `seconds`, and the pattern grows until
// a second, spurious match is unlikely.
// ===========================================================================
namespace demo {

constexpr double kMaxExtraMatches = 0.1;  // expected spurious matches we tolerate
constexpr int kMargin = 40;               // the planted spot stays this far inside the region

inline int run(const std::string& backend, Edition edition, const char* seed_s, int y, bool all_orient,
               double seconds, bool max, const std::string& out_path) {
    const bool be = edition == Edition::bedrock;
    if (y < (be ? -62 : -63) || y > -60) {
        std::fprintf(stderr, "--demo needs --y in %s..-60 (the other layers are all bedrock or all air)\n",
                     be ? "-62" : "-63");
        return 2;
    }
    std::mt19937_64 rng(std::random_device{}() ^
                        static_cast<std::uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count()));
    const std::int64_t seed = seed_s ? seed_from_string(seed_s) : static_cast<std::int64_t>(rng());
    auto uniform = [&](std::int64_t lo, std::int64_t hi) {  // inclusive
        return lo + static_cast<std::int64_t>(rng() % static_cast<std::uint64_t>(hi - lo + 1));
    };

    rokkdoxx::BedrockGenerator gen(seed, edition);
    const double pb = static_cast<double>(gen.threshold(y)) / 16777216.0;  // P(bedrock)

    // `s` x `s` of the world at (fx, fz), as a fully known pattern; `p` = the
    // chance a random spot matches it (one orientation). Past 2^24 Bedrock
    // Edition repeats columns (rk_be_coord), so each one counts once.
    auto copy_world = [&](int fx, int fz, int s, double& p) {
        Pattern pat = world_patch(seed, y, fx, fz, s, s, edition);
        std::set<std::pair<int, int>> seen;
        p = 1.0;
        for (int j = 0; j < s; ++j)
            for (int i = 0; i < s; ++i)
                if (!be || seen.insert({rk_be_coord(fx + i), rk_be_coord(fz + j)}).second)
                    p *= pat.at(i, j) == Cell::bedrock ? pb : 1.0 - pb;
        return pat;
    };

    // Time a search shaped like the real one (this seed and layer, an 8x8 pattern
    // from this world): the cost per candidate depends on the layer.
    BackendInfo chosen;
    std::unique_ptr<Worker> worker;
    if (const int rc = bmark::open_worker(backend, chosen, worker)) return rc;
    std::fprintf(stderr, "measuring speed on %s ...\n", chosen.label.c_str());
    WorkerConfig cfg = worker_config(seed, y, edition);
    cfg.all_orientations = all_orient;
    cfg.knowns = world_patch(seed, y, 137, -251, 8, 8, edition).knowns();
    const double rate = bmark::measure_rate(*worker, cfg);  // candidates / second

    // A random square of the world with ~seconds * rate candidates (10% spare).
    const std::int64_t border = Region::kWorldBorder;
    const std::int64_t r = max ? border
                               : std::max<std::int64_t>(
                                     64, static_cast<std::int64_t>((std::sqrt(rate * seconds * 0.9) - 1) / 2));
    const bool whole = 2 * r + 1 >= 2 * border;  // `max`, or a budget longer than the world
    // Always centred on 0,0 (spawn), like someone who has no idea where their build is: only
    // the planted spot below is random.
    const std::int64_t cx = 0, cz = 0;
    const Region region = whole ? Region::world() : Region::centered(cx, cz, r);
    const std::int64_t px = uniform(region.x0 + kMargin, region.x1 - kMargin);
    const std::int64_t pz = uniform(region.z0 + kMargin, region.z1 - kMargin);

    // Grow a square pattern copied from the world at (px, pz) until the expected
    // number of other places it matches (area * orientations * P(pattern)) is small.
    const double area = static_cast<double>(region.candidates());
    PatternFile pf;
    double extra = 0;
    int fx = 0, fz = 0;
    for (int s = 4; s <= 32 && (s == 4 || extra > kMaxExtraMatches); ++s) {
        double p;
        fx = static_cast<int>(px) - s / 2;
        fz = static_cast<int>(pz) - s / 2;
        pf.pattern = copy_world(fx, fz, s, p);
        extra = area * (all_orient ? 8 : 1) * p;
    }
    pf.edition = edition;
    pf.seed = std::to_string(seed);
    pf.y = y;
    pf.center_x = std::to_string(cx);
    pf.center_z = std::to_string(cz);
    pf.radius = whole ? "-1" : std::to_string(r);
    pf.all_orientations = all_orient;
    std::string err;
    if (!save_pattern_file(out_path, pf, err)) {
        std::fprintf(stderr, "%s\n", err.c_str());
        return 1;
    }

    // Matches are reported at the pattern's anchor cell, not its corner.
    const SearchPlan plan = build_search_plan(pf.pattern.knowns(), gen.threshold(y), all_orient);
    std::printf("wrote       : %s\n", out_path.c_str());
    if (be) std::printf("edition     : Bedrock (no seed)   (y %d)\n", y);
    else std::printf("seed        : %lld   (y %d)\n", static_cast<long long>(seed), y);
    std::printf("speed       : %.1f Gcand/s on %s (%s)\n", rate / 1e9, chosen.label.c_str(),
                all_orient ? "all 8 orientations" : "exact");
    std::printf("search      : %s, %.2e candidates, about %s\n",
                whole ? "whole world" : ("center " + std::to_string(cx) + "," + std::to_string(cz) +
                                          " radius " + std::to_string(r)).c_str(),
                area, format_duration(area / rate).c_str());
    std::printf("pattern     : %dx%d, expect %.2g other matches\n", pf.pattern.w, pf.pattern.h, extra);
    std::printf("expect match: %lld %lld\n", static_cast<long long>(fx + plan.anchor_i),
                static_cast<long long>(fz + plan.anchor_j));
    std::printf("try         : rokksearch --pattern %s   |   rokktui --load %s\n", out_path.c_str(),
                out_path.c_str());
    return 0;
}

}  // namespace demo

}  // namespace

int main(int argc, char** argv) {
#ifdef SIGPIPE
    std::signal(SIGPIPE, SIG_IGN);  // don't die if the reader of stdout goes away
#endif
    std::string pattern_path, checkpoint, backend;
    const char* edition_s = nullptr;
    const char *seed_s = nullptr, *size_s = nullptr, *center_s = nullptr, *radius_s = nullptr,
               *region_s = nullptr, *orient_s = nullptr;
    int y = -60;
    std::uint32_t cap = 1u << 20;
    int tile = 4096;
    bool json = false, bench = false, benchmark = false, first = false;
    double benchmark_seconds = 2.0;
    int benchmark_iters = 5;
    double benchmark_long = 0;
    const char* demo_arg = nullptr;
    std::string out_path = "demo_pattern.txt";
    bool have_y = false;

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto val = [&](const char* /*hint*/) -> const char* {
            if (i + 1 >= argc) usage(2);
            return argv[++i];
        };
        if (a == "-h" || a == "--help") usage(0);
        else if (a == "--edition") edition_s = val("edition");
        else if (a == "--seed") seed_s = val("seed");
        else if (a == "--y") { y = std::atoi(val("y")); have_y = true; }
        else if (a == "--size") size_s = val("size");
        else if (a == "--pattern") pattern_path = val("pattern");
        else if (a == "--center") center_s = val("center");
        else if (a == "--radius") radius_s = val("radius");
        else if (a == "--region") region_s = val("region");
        else if (a == "--orientations") orient_s = val("orientations");
        else if (a == "--cap") cap = static_cast<std::uint32_t>(std::strtoul(val("cap"), nullptr, 10));
        else if (a == "--tile") tile = std::atoi(val("tile"));
        else if (a == "--checkpoint") checkpoint = val("checkpoint");
        else if (a == "--backend") backend = val("backend");
        else if (a == "--first") first = true;
        else if (a == "--json") json = true;
        else if (a == "--bench") bench = true;
        else if (a == "--benchmark") benchmark = true;
        else if (a == "--benchmark-seconds") benchmark_seconds = std::atof(val("benchmark-seconds"));
        else if (a == "--benchmark-iters") benchmark_iters = std::atoi(val("benchmark-iters"));
        else if (a == "--benchmark-long") { benchmark = true; benchmark_long = std::atof(val("benchmark-long")); }
        else if (a == "--demo") demo_arg = val("demo");
        else if (a == "--out") out_path = val("out");
        else if (a == "--list-backends") {
            for (const auto& b : list_backends())
                std::printf("%-10s  %s%s\n", b.id.c_str(), b.label.c_str(),
                            !b.is_gpu ? "" : b.integrated ? "  [integrated gpu]" : "  [dedicated gpu]");
            return 0;
        } else {
            std::fprintf(stderr, "unknown option: %s\n", a.c_str());
            usage(2);
        }
    }

    if (edition_s && std::strcmp(edition_s, "java") != 0 && std::strcmp(edition_s, "bedrock") != 0) {
        std::fprintf(stderr, "--edition wants java or bedrock (got '%s')\n", edition_s);
        return 2;
    }
    const Edition edition = edition_s && std::strcmp(edition_s, "bedrock") == 0 ? Edition::bedrock : Edition::java;

    if (demo_arg) {
        double seconds;
        bool max;
        if (!parse_budget(demo_arg, seconds, max)) {
            std::fprintf(stderr, "--demo wants a time like 90, 90s, 5m, 2h, or max (got '%s')\n", demo_arg);
            return 2;
        }
        return demo::run(backend, edition, seed_s, y, !orient_s || std::strcmp(orient_s, "exact") != 0,
                         seconds, max, out_path);
    }

    if (benchmark) {
        if (seed_s || !pattern_path.empty() || region_s || center_s)
            std::fprintf(stderr, "note: --benchmark uses a fixed workload; search args ignored\n");
        return bmark::run(backend, json, benchmark_seconds, benchmark_iters, benchmark_long, edition);
    }

    SearchRequest req;
    PatternFile pf;
    if (!pattern_path.empty()) {
        std::string err;
        if (!load_pattern_file(pattern_path, pf, err)) {
            std::fprintf(stderr, "%s\n", err.c_str());
            return 1;
        }
        req.pattern = pf.pattern;
        req.edition = pf.edition;
        req.seed = seed_from_string(pf.seed);
        if (!have_y) y = pf.y;
        if (!center_s && !region_s) {
            long long cx = std::atoll(pf.center_x.c_str()), cz = std::atoll(pf.center_z.c_str());
            long long r = std::atoll(pf.radius.c_str());
            req.region = r == -1 ? Region::world() : Region::centered(cx, cz, r);
        }
        req.all_orientations = pf.all_orientations;
        // A saved session (from rokktui's `s`) names its progress file: resume it.
        if (checkpoint.empty()) checkpoint = pf.checkpoint;
        if (pf.stop_first) first = true;
    }

    if (edition_s) req.edition = edition;
    if (seed_s) req.seed = seed_from_string(seed_s);
    req.plane_y = y;
    if (orient_s) req.all_orientations = std::strcmp(orient_s, "exact") != 0;
    req.match_cap = cap;
    req.stop_at_first_match = first;
    req.tile_side = tile;
    req.checkpoint_path = checkpoint;

    if (size_s && req.pattern.w == 0) {
        long long w, h;
        if (parse2(size_s, w, h, 'x')) {
            req.pattern.w = static_cast<int>(w);
            req.pattern.h = static_cast<int>(h);
            req.pattern.cells.assign(static_cast<std::size_t>(w) * h, Cell::unknown);
        }
    }

    if (region_s) {
        const char* p1 = std::strchr(region_s, ',');
        if (!p1) usage(2);
        const char* p2 = std::strchr(p1 + 1, ',');
        const char* p3 = p2 ? std::strchr(p2 + 1, ',') : nullptr;
        if (!p2 || !p3) usage(2);
        req.region.x0 = std::atoll(std::string(region_s, p1).c_str());
        req.region.x1 = std::atoll(std::string(p1 + 1, p2).c_str());
        req.region.z0 = std::atoll(std::string(p2 + 1, p3).c_str());
        req.region.z1 = std::atoll(p3 + 1);
    } else if (center_s) {
        long long cx, cz;
        if (!parse2(center_s, cx, cz, ',')) usage(2);
        long long r = radius_s ? std::atoll(radius_s) : 5000;
        req.region = r == -1 ? Region::world() : Region::centered(cx, cz, r);
    } else if (pattern_path.empty()) {  // otherwise the region came from the file
        std::fprintf(stderr, "need --center/--radius or --region (or a --pattern file that has them)\n");
        return 2;
    }

    std::unique_ptr<SearchClient> client;
    JobId job = 0;
    JobStatus fin;
    std::vector<Match> matches;
    try {
        client = make_client(backend);
        std::fprintf(stderr, "backend: %s\n", client->backend_name().c_str());
        job = client->submit(req);
        for (;;) {
            JobStatus st = client->poll(job);
            std::fprintf(stderr, "\r%-9s %6.2f%%  %lld/%lld  matches=%" PRIu64 "  %.0f M/s  ETA %s   ",
                         to_string(st.state), st.progress * 100.0,
                         static_cast<long long>(st.candidates_done),
                         static_cast<long long>(st.candidates_total),
                         static_cast<std::uint64_t>(st.matches), st.rate / 1e6,
                         st.rate > 0 ? format_duration(st.eta_s).c_str() : "--");
            std::fflush(stderr);
            if (st.state == JobState::done || st.state == JobState::cancelled) break;
            if (st.state == JobState::error) {
                std::fprintf(stderr, "\nerror: %s\n", st.error.c_str());
                return 1;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        std::fprintf(stderr, "\n");
        fin = client->poll(job);
        matches = client->results(job);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "\nservice error: %s\n", e.what());
        return 1;
    }

    if (bench) {
        std::printf("%s: %.3f Gcand/s  (%lld candidates in %.2fs, %" PRIu64 " matches%s)\n",
                    client->backend_name().c_str(), fin.rate / 1e9,
                    static_cast<long long>(fin.candidates_total), fin.elapsed_s, fin.matches,
                    fin.truncated ? ", capped" : "");
        return 0;
    }
    if (json) {
        std::printf("{\"backend\":\"%s\",\"elapsed_s\":%.3f,\"candidates\":%lld,\"truncated\":%s,\"matches\":[",
                    client->backend_name().c_str(), fin.elapsed_s,
                    static_cast<long long>(fin.candidates_total), fin.truncated ? "true" : "false");
        for (std::size_t i = 0; i < matches.size(); ++i)
            std::printf("%s[%d,%d,%u]", i ? "," : "", matches[i].x, matches[i].z, matches[i].orient_mask);
        std::printf("]}\n");
    } else {
        for (const auto& m : matches) std::printf("%d %d %u\n", m.x, m.z, m.orient_mask);
        std::fprintf(stderr, "%zu match(es)%s in %.2fs%s\n", matches.size(),
                     fin.truncated ? " (capped)" : "", fin.elapsed_s,
                     fin.stopped_early ? " (stopped at the first match; rest of the region not searched)" : "");
    }
    return 0;
}
