#include "daemon.hpp"

#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "protocol.hpp"

namespace rokkdoxx::svc {

std::string default_socket_path() {
    if (const char* r = std::getenv("XDG_RUNTIME_DIR"))
        return std::string(r) + "/rokkd.sock";
    return "/tmp/rokkd-" + std::to_string(getuid()) + ".sock";
}

namespace {

Json err(const std::string& msg) {
    Json j = Json::object();
    j.obj["ok"] = Json(false);
    j.obj["error"] = Json(msg);
    return j;
}

void handle_connection(SearchService& svc, int cfd) {
    std::string in;
    if (!recv_message(cfd, in)) return;
    Json req;
    Json resp;
    if (!Json::parse(in, req) || !req.is_obj()) {
        resp = err("bad request");
    } else {
        const std::string op = req.find("op") ? req.find("op")->as_str() : "";
        if (op == "backend") {
            resp = Json::object();
            resp.obj["ok"] = Json(true);
            resp.obj["backend"] = Json(svc.backend_name());
        } else if (op == "submit") {
            SearchRequest sr;
            const Json* rj = req.find("req");
            if (!rj || !request_from_json(*rj, sr)) {
                resp = err("bad req payload");
            } else {
                resp = Json::object();
                resp.obj["ok"] = Json(true);
                resp.obj["job"] = Json(static_cast<long long>(svc.submit(sr)));
            }
        } else if (op == "poll") {
            JobId id = req.find("job") ? req.find("job")->as_u64() : 0;
            resp = Json::object();
            resp.obj["ok"] = Json(true);
            resp.obj["status"] = status_to_json(svc.poll(id));
        } else if (op == "results") {
            JobId id = req.find("job") ? req.find("job")->as_u64() : 0;
            resp = Json::object();
            resp.obj["ok"] = Json(true);
            resp.obj["matches"] = matches_to_json(svc.results(id));
        } else if (op == "cancel") {
            JobId id = req.find("job") ? req.find("job")->as_u64() : 0;
            svc.cancel(id);
            resp = Json::object();
            resp.obj["ok"] = Json(true);
        } else {
            resp = err("unknown op: " + op);
        }
    }
    send_message(cfd, resp.dump());
}

}  // namespace

int serve(SearchService& service, const std::string& socket_path, std::atomic<bool>& stop) {
    ::unlink(socket_path.c_str());

    int sfd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (sfd < 0) {
        std::perror("socket");
        return 1;
    }

    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    if (socket_path.size() >= sizeof(addr.sun_path)) {
        std::fprintf(stderr, "socket path too long\n");
        ::close(sfd);
        return 1;
    }
    std::strncpy(addr.sun_path, socket_path.c_str(), sizeof(addr.sun_path) - 1);

    if (::bind(sfd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        std::perror("bind");
        ::close(sfd);
        return 1;
    }
    if (::listen(sfd, 16) < 0) {
        std::perror("listen");
        ::close(sfd);
        ::unlink(socket_path.c_str());
        return 1;
    }

    while (!stop.load()) {
        pollfd pfd{sfd, POLLIN, 0};
        int pr = ::poll(&pfd, 1, 200);
        if (pr <= 0) continue;
        int cfd = ::accept(sfd, nullptr, nullptr);
        if (cfd < 0) continue;
        handle_connection(service, cfd);
        ::close(cfd);
    }

    ::close(sfd);
    ::unlink(socket_path.c_str());
    return 0;
}

}  // namespace rokkdoxx::svc
