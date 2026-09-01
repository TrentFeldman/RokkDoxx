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
#include <string>
#include <thread>
#include <vector>

#include "gen/bedrock.hpp"
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
                 "  --seed <s>            world seed (numeric or text)\n"
                 "  --y <n>               bedrock plane, -64..-59 (default -60)\n"
                 "  --size <WxH>          pattern size when not loading a file\n"
                 "  --pattern <file>      load a .txt pattern (sets seed/y/center/radius too)\n"
                 "  --center <x,z>        search-region center\n"
                 "  --radius <r>          search-region half-extent (blocks)\n"
                 "  --region <x0,x1,z0,z1> explicit search region (overrides center/radius)\n"
                 "  --orientations <all|exact>\n"
                 "  --cap <n>             max matches to keep (default 1048576)\n"
                 "  --tile <n>            tile side floor (default 4096)\n"
                 "  --checkpoint <file>   resumable progress file\n"
                 "  --backend <id>        cpu | opencl:N | auto (default auto)\n"
                 "  --json               machine-readable output\n"
                 "  --bench              measure the search you asked for (rate only)\n"
                 "  --benchmark          run the standard reproducible benchmark\n"
                 "                       (fixed workload; ignores --seed/--pattern/--region)\n"
                 "  --benchmark-seconds <f>  target seconds per phase (default 2.0)\n"
                 "  --benchmark-iters <n>    measured iterations per phase (default 5)\n"
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

// ===========================================================================
// --benchmark : a fixed, reproducible workload so numbers compare across
// machines. It drives a Worker directly (no per-job kernel rebuild, no service
// overhead) and reports candidate-origins/second for the exact and all-8 cases.
// ===========================================================================
namespace bmark {

constexpr std::int64_t kSeed = 0;
constexpr int kPlaneY = -60;
constexpr int kBenchVersion = 1;
constexpr long long kMaxCandidates = 500'000'000'000LL;  // clamp per phase

// The fixed workload pattern: a 6x6 patch (a typical real size) filled from the
// generator at a fixed origin, so it is a genuine bedrock configuration for
// `kSeed`. It matches ~once in the whole benchmark region, so result collection
// is free and the search does representative work.
constexpr int kPatW = 6, kPatH = 6, kFillX = 137, kFillZ = -251;

inline Pattern standard_pattern() {
    rokkdoxx::BedrockGenerator gen(kSeed);
    Pattern p;
    p.w = kPatW;
    p.h = kPatH;
    p.cells.assign(static_cast<std::size_t>(kPatW) * kPatH, Cell::unknown);
    for (int j = 0; j < kPatH; ++j)
        for (int i = 0; i < kPatW; ++i)
            p.cells[static_cast<std::size_t>(j) * kPatW + i] =
                gen.is_bedrock_floor(kFillX + i, kPlaneY, kFillZ + j) ? Cell::bedrock
                                                                      : Cell::not_bedrock;
    return p;
}

// One timed sweep of `region` through an already-configured worker.
inline double sweep(Worker& w, const Region& region, int tile_side) {
    TileScheduler sched(region, tile_side);
    Tile t;
    const auto t0 = std::chrono::steady_clock::now();
    while (sched.next(t)) (void)w.run_tile(t);
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
}

inline long long candidates_of(long r) {
    const long long side = 2LL * r + 1;
    return side * side;
}

// radius whose region has ~`cand` candidate origins, clamped to sane bounds.
inline long radius_for(double cand) {
    if (cand > static_cast<double>(kMaxCandidates)) cand = static_cast<double>(kMaxCandidates);
    long r = static_cast<long>((std::sqrt(cand) - 1.0) / 2.0);
    return r < 1000 ? 1000 : r;
}

// Extrapolate from one measured sweep to the radius that should take `target_s`.
inline long calibrate_from(long r, double elapsed, double target_s) {
    if (elapsed < 1e-6) return radius_for(static_cast<double>(kMaxCandidates));
    const double rate = static_cast<double>(candidates_of(r)) / elapsed;  // cand / s
    return radius_for(rate * target_s);
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

inline PhaseResult run_phase(Worker& w, const char* name, bool all_orient, int tile_side,
                             const WorkerConfig& base, double target_s, int iters) {
    WorkerConfig cfg = base;
    cfg.all_orientations = all_orient;
    w.configure(cfg);

    // Warm up (clocks, caches, driver), then calibrate: probe -> extrapolate to
    // ~target_s -> one refine sweep at that size -> extrapolate again. Two
    // rounds because the per-candidate rate drifts a little with region size.
    double e = sweep(w, Region::centered(0, 0, 3000), tile_side);          // discarded
    long r = calibrate_from(3000, e, 0.4);
    e = sweep(w, Region::centered(0, 0, r), tile_side);                    // discarded (warm)
    r = calibrate_from(r, e, target_s);
    e = sweep(w, Region::centered(0, 0, r), tile_side);                    // discarded
    r = calibrate_from(r, e, target_s);

    const long long cand = candidates_of(r);
    const Region region = Region::centered(0, 0, r);

    std::vector<double> rates, times;
    for (int i = 0; i < iters; ++i) {
        const double e = sweep(w, region, tile_side);
        times.push_back(e);
        rates.push_back(static_cast<double>(cand) / e / 1e9);
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

inline int run(const std::string& backend_arg, bool json, double target_s, int iters) {
    if (target_s <= 0.05) target_s = 0.05;
    if (iters < 1) iters = 1;

    // Resolve the backend so we can also report its details.
    const auto backends = list_backends();
    if (backends.empty()) {
        std::fprintf(stderr, "no compute backend available\n");
        return 1;
    }
    BackendInfo chosen = backends.front();
    const std::string want = backend_arg.empty() ? "auto" : backend_arg;
    if (want == "auto") {
        for (const auto& b : backends)
            if (b.is_gpu) {
                chosen = b;
                break;
            }
    } else {
        bool found = false;
        for (const auto& b : backends)
            if (b.id == want) {
                chosen = b;
                found = true;
                break;
            }
        if (!found) {
            std::fprintf(stderr, "unknown backend: %s\n", want.c_str());
            return 2;
        }
    }

    std::unique_ptr<Worker> worker;
    try {
        worker = make_worker_factory(chosen.id)();  // builds the GPU kernel once, here
    } catch (const std::exception& e) {
        std::fprintf(stderr, "backend error: %s\n", e.what());
        return 1;
    }

    rokkdoxx::BedrockGenerator gen(kSeed);
    const Pattern pattern = standard_pattern();
    WorkerConfig base;
    base.derived_lo = gen.derived_lo();
    base.derived_hi = gen.derived_hi();
    base.plane_y = kPlaneY;
    base.threshold = gen.threshold(kPlaneY);
    base.knowns = pattern.knowns();
    base.match_cap = 1u << 20;

    const int tile_side = std::max(4096, worker->preferred_tile_side());

    const PhaseResult ex = run_phase(*worker, "exact", false, tile_side, base, target_s, iters);
    const PhaseResult a8 = run_phase(*worker, "all-8", true, tile_side, base, target_s, iters);
    const PhaseResult phases[2] = {ex, a8};

    const unsigned threads = std::thread::hardware_concurrency();

    if (json) {
        std::printf("{\"benchmark_version\":%d,\"backend\":\"%s\",", kBenchVersion,
                    chosen.label.c_str());
        std::printf("\"device\":{\"is_gpu\":%s,\"cl_version\":\"%s\",\"driver\":\"%s\",\"units\":%d},",
                    chosen.is_gpu ? "true" : "false", chosen.version.c_str(), chosen.driver.c_str(),
                    chosen.units);
        std::printf("\"host\":{\"os\":\"%s\",\"arch\":\"%s\",\"threads\":%u,\"compiler\":\"%s\"},",
                    host_os(), host_arch(), threads, host_compiler().c_str());
        std::printf("\"pattern\":{\"w\":%d,\"h\":%d,\"seed\":%lld,\"y\":%d},", kPatW, kPatH,
                    static_cast<long long>(kSeed), kPlaneY);
        std::printf("\"phases\":[");
        for (int i = 0; i < 2; ++i) {
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

    std::printf("rokksearch benchmark v%d\n", kBenchVersion);
    std::printf("backend   : %s\n", chosen.label.c_str());
    if (chosen.is_gpu)
        std::printf("device    : %s  |  %s  |  driver %s  |  %d CU\n", chosen.label.c_str(),
                    chosen.version.empty() ? "OpenCL ?" : chosen.version.c_str(),
                    chosen.driver.empty() ? "?" : chosen.driver.c_str(), chosen.units);
    std::printf("host      : %s %s  |  %u threads  |  %s\n", host_os(), host_arch(), threads,
                host_compiler().c_str());
    std::printf("pattern   : %dx%d from seed %lld at y %d   (fixed workload)\n\n", kPatW, kPatH,
                static_cast<long long>(kSeed), kPlaneY);
    for (const PhaseResult& p : phases) {
        std::printf("%-6s : %7.2f Gcand/s  (median of %d; min %.2f, max %.2f)   "
                    "r=%ld, %.2e cand, %.2f s\n",
                    p.name, p.g_median, p.iters, p.g_min, p.g_max, p.radius,
                    static_cast<double>(p.candidates), p.elapsed_s);
    }
    std::printf("\nG = 10^9 candidate origins checked per second.\n");
    return 0;
}

}  // namespace bmark

}  // namespace

int main(int argc, char** argv) {
#ifdef SIGPIPE
    std::signal(SIGPIPE, SIG_IGN);  // don't die if the reader of stdout goes away
#endif
    std::string pattern_path, checkpoint, backend;
    const char *seed_s = nullptr, *size_s = nullptr, *center_s = nullptr, *radius_s = nullptr,
               *region_s = nullptr, *orient_s = nullptr;
    int y = -60;
    std::uint32_t cap = 1u << 20;
    int tile = 4096;
    bool json = false, bench = false, benchmark = false;
    double benchmark_seconds = 2.0;
    int benchmark_iters = 5;
    bool have_y = false;

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto val = [&](const char* name) -> const char* {
            if (i + 1 >= argc) usage(2);
            return argv[++i];
            (void)name;
        };
        if (a == "-h" || a == "--help") usage(0);
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
        else if (a == "--json") json = true;
        else if (a == "--bench") bench = true;
        else if (a == "--benchmark") benchmark = true;
        else if (a == "--benchmark-seconds") benchmark_seconds = std::atof(val("benchmark-seconds"));
        else if (a == "--benchmark-iters") benchmark_iters = std::atoi(val("benchmark-iters"));
        else if (a == "--list-backends") {
            for (const auto& b : list_backends())
                std::printf("%-10s  %s%s\n", b.id.c_str(), b.label.c_str(), b.is_gpu ? "  [gpu]" : "");
            return 0;
        } else {
            std::fprintf(stderr, "unknown option: %s\n", a.c_str());
            usage(2);
        }
    }

    if (benchmark) {
        if (seed_s || !pattern_path.empty() || region_s || center_s)
            std::fprintf(stderr, "note: --benchmark uses a fixed workload; search args ignored\n");
        return bmark::run(backend, json, benchmark_seconds, benchmark_iters);
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
        req.seed = seed_from_string(pf.seed);
        if (!have_y) y = pf.y;
        if (!center_s && !region_s) {
            long long cx = std::atoll(pf.center_x.c_str()), cz = std::atoll(pf.center_z.c_str());
            long long r = std::atoll(pf.radius.c_str());
            req.region = Region::centered(cx, cz, r);
        }
        req.all_orientations = pf.all_orientations;
    }

    if (seed_s) req.seed = seed_from_string(seed_s);
    req.plane_y = y;
    if (orient_s) req.all_orientations = std::strcmp(orient_s, "exact") != 0;
    req.match_cap = cap;
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
        long long a, b;
        const char* p1 = std::strchr(region_s, ',');
        if (!p1) usage(2);
        const char* p2 = std::strchr(p1 + 1, ',');
        const char* p3 = p2 ? std::strchr(p2 + 1, ',') : nullptr;
        if (!p2 || !p3) usage(2);
        req.region.x0 = std::atoll(std::string(region_s, p1).c_str());
        req.region.x1 = std::atoll(std::string(p1 + 1, p2).c_str());
        req.region.z0 = std::atoll(std::string(p2 + 1, p3).c_str());
        req.region.z1 = std::atoll(p3 + 1);
        (void)a;
        (void)b;
    } else if (center_s) {
        long long cx, cz;
        if (!parse2(center_s, cx, cz, ',')) usage(2);
        long long r = radius_s ? std::atoll(radius_s) : 5000;
        req.region = Region::centered(cx, cz, r);
    } else if (req.region.candidates() <= 1 && !pattern_path.empty()) {
        // region came from the file
    } else if (!region_s && !center_s && pattern_path.empty()) {
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
            std::fprintf(stderr, "\r%-9s %6.2f%%  %lld/%lld  matches=%" PRIu64 "  %.0f M/s   ",
                         to_string(st.state), st.progress * 100.0,
                         static_cast<long long>(st.candidates_done),
                         static_cast<long long>(st.candidates_total),
                         static_cast<std::uint64_t>(st.matches), st.rate / 1e6);
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
        std::fprintf(stderr, "%zu match(es)%s in %.2fs\n", matches.size(),
                     fin.truncated ? " (capped)" : "", fin.elapsed_s);
    }
    return 0;
}
