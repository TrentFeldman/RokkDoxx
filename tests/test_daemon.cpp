// The daemon path (rokkd serve loop + SocketClient) returns exactly what an
// in-process SearchService does for the same request.
#include <atomic>
#include <chrono>
#include <cstdio>
#include <map>
#include <string>
#include <thread>
#include <unistd.h>

#include "gen/bedrock.hpp"
#include "svc/client.hpp"
#include "svc/daemon.hpp"
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

SearchRequest make_request() {
    rokkdoxx::BedrockGenerator gen(987654321);
    SearchRequest req;
    req.seed = 987654321;
    req.plane_y = -60;
    req.pattern.w = 5;
    req.pattern.h = 5;
    req.pattern.cells.assign(25, Cell::unknown);
    const int cx = -400, cz = 250;
    for (int j = 0; j < 5; ++j)
        for (int i = 0; i < 5; ++i)
            req.pattern.cells[static_cast<std::size_t>(j) * 5 + i] =
                gen.is_bedrock_floor(cx + i, -60, cz + j) ? Cell::bedrock : Cell::not_bedrock;
    req.region = Region::centered(cx, cz, 250);
    req.all_orientations = true;
    return req;
}

std::vector<Match> run(SearchClient& c, const SearchRequest& req) {
    JobId id = c.submit(req);
    for (;;) {
        JobStatus st = c.poll(id);
        if (st.state == JobState::done || st.state == JobState::cancelled) break;
        if (st.state == JobState::error) {
            std::printf("  FAIL: %s\n", st.error.c_str());
            ++g_fail;
            return {};
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return c.results(id);
}

std::map<std::pair<int, int>, std::uint8_t> as_map(const std::vector<Match>& v) {
    std::map<std::pair<int, int>, std::uint8_t> m;
    for (auto& x : v) m[{x.x, x.z}] = x.orient_mask;
    return m;
}

}  // namespace

int main() {
    std::printf("test_daemon\n");
    const std::string sock = "test_rokkd_" + std::to_string(getpid()) + ".sock";

    SearchService service(make_worker_factory("cpu"));
    std::atomic<bool> stop{false};
    std::thread server([&] { serve(service, sock, stop); });
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    const SearchRequest req = make_request();

    // reference via in-process client
    auto local = make_client(parse_target("local", "cpu"));
    auto want = as_map(run(*local, req));

    // via the socket
    try {
        auto remote = make_client(parse_target("unix:" + sock));
        check(remote->backend_name().find("cpu") != std::string::npos, "remote backend is cpu");
        auto got = as_map(run(*remote, req));
        check(got == want, "socket results match in-process (" + std::to_string(got.size()) +
                               " vs " + std::to_string(want.size()) + ")");
        check(!got.empty(), "found at least the fill origin");
    } catch (const std::exception& e) {
        std::printf("  FAIL: %s\n", e.what());
        ++g_fail;
    }

    stop.store(true);
    server.join();

    if (g_fail == 0) {
        std::printf("\nALL PASS\n");
        return 0;
    }
    std::printf("\n%d FAILURE(S)\n", g_fail);
    return 1;
}
