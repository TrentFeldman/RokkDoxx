// rokkd -- the RokkDoxx search daemon. Holds one SearchService and serves it
// over a Unix domain socket (see src/svc/protocol.hpp). Front-ends connect with
// `--service unix:<path>`.
//
//   rokkd [--socket <path>] [--backend cpu|opencl:N|auto]
#include <atomic>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <string>

#include "svc/daemon.hpp"
#include "svc/search_service.hpp"
#include "svc/workers.hpp"

using namespace rokkdoxx::svc;

namespace {
std::atomic<bool> g_stop{false};
void on_sig(int) { g_stop.store(true); }
}  // namespace

int main(int argc, char** argv) {
    std::string socket_path = default_socket_path();
    std::string backend;

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if ((a == "--socket" || a == "-s") && i + 1 < argc) socket_path = argv[++i];
        else if (a == "--backend" && i + 1 < argc) backend = argv[++i];
        else if (a == "-h" || a == "--help") {
            std::printf(
                "rokkd -- RokkDoxx search daemon\n"
                "  --socket <path>    Unix socket to listen on (default %s)\n"
                "  --backend <id>     cpu | opencl:N | auto (default auto)\n",
                default_socket_path().c_str());
            return 0;
        } else {
            std::fprintf(stderr, "unknown option: %s\n", a.c_str());
            return 2;
        }
    }

    std::signal(SIGINT, on_sig);
    std::signal(SIGTERM, on_sig);
    std::signal(SIGPIPE, SIG_IGN);

    SearchService service(make_worker_factory(backend));
    std::fprintf(stderr, "rokkd: backend %s, listening on %s\n", service.backend_name().c_str(),
                 socket_path.c_str());

    const int rc = serve(service, socket_path, g_stop);
    std::fprintf(stderr, "rokkd: shut down\n");
    return rc;
}
