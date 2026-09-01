// rokksearch -- headless client for the RokkDoxx search service.
//
//   rokksearch --seed <s> --y -60 --pattern p.txt --center 0,0 --radius 2000000
//   (see --help for all options)
//
// Streams a progress line to stderr; prints matches ("x z mask") to stdout.
#include <csignal>
#include <chrono>
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>

#include "svc/client.hpp"
#include "svc/pattern_io.hpp"
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
                 "  --bench              report throughput only\n"
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
    bool json = false, bench = false;
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
        else if (a == "--list-backends") {
            for (const auto& b : list_backends())
                std::printf("%-10s  %s%s\n", b.id.c_str(), b.label.c_str(), b.is_gpu ? "  [gpu]" : "");
            return 0;
        } else {
            std::fprintf(stderr, "unknown option: %s\n", a.c_str());
            usage(2);
        }
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
