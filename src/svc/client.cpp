#include "client.hpp"

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <cstring>
#include <stdexcept>

#include "protocol.hpp"
#include "search_service.hpp"
#include "workers.hpp"

namespace rokkdoxx::svc {

ServiceTarget parse_target(std::string_view s, std::string backend) {
    ServiceTarget t;
    t.backend = std::move(backend);
    if (s.empty() || s == "local" || s == "inprocess") {
        t.kind = ServiceTarget::in_process;
    } else if (s.rfind("unix:", 0) == 0) {
        t.kind = ServiceTarget::unix_socket;
        t.path = std::string(s.substr(5));
    } else {
        t.kind = ServiceTarget::unix_socket;  // a bare path is a socket path
        t.path = std::string(s);
    }
    return t;
}

namespace {

// Transport 1: the service lives in this process. Calls are direct.
class InProcessClient : public SearchClient {
public:
    explicit InProcessClient(const std::string& backend)
        : svc_(make_worker_factory(backend)) {}

    std::string backend_name() override { return svc_.backend_name(); }
    JobId submit(const SearchRequest& r) override { return svc_.submit(r); }
    JobStatus poll(JobId id) override { return svc_.poll(id); }
    std::vector<Match> results(JobId id) override { return svc_.results(id); }
    void cancel(JobId id) override { svc_.cancel(id); }

private:
    SearchService svc_;
};

// Transport 2: the service is a rokkd daemon on the other end of a Unix socket.
// One short-lived connection per call -- local socket connects are cheap and it
// keeps the client stateless.
class SocketClient : public SearchClient {
public:
    explicit SocketClient(std::string path) : path_(std::move(path)) {}

    std::string backend_name() override {
        Json r = call(op("backend"));
        return r.find("backend") ? r.find("backend")->as_str() : "remote";
    }

    JobId submit(const SearchRequest& req) override {
        Json m = op("submit");
        m.obj["req"] = request_to_json(req);
        Json r = call(m);
        return r.find("job") ? r.find("job")->as_u64() : 0;
    }

    JobStatus poll(JobId id) override {
        Json m = op("poll");
        m.obj["job"] = Json(static_cast<long long>(id));
        Json r = call(m);
        return r.find("status") ? status_from_json(*r.find("status")) : JobStatus{};
    }

    std::vector<Match> results(JobId id) override {
        Json m = op("results");
        m.obj["job"] = Json(static_cast<long long>(id));
        Json r = call(m);
        return r.find("matches") ? matches_from_json(*r.find("matches")) : std::vector<Match>{};
    }

    void cancel(JobId id) override {
        Json m = op("cancel");
        m.obj["job"] = Json(static_cast<long long>(id));
        call(m);
    }

private:
    static Json op(const char* name) {
        Json j = Json::object();
        j.obj["op"] = Json(std::string(name));
        return j;
    }

    // Connect, send one framed message, read one framed reply, close. Throws
    // std::runtime_error on any transport or protocol failure.
    Json call(const Json& msg) {
        int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
        if (fd < 0) throw std::runtime_error("socket() failed");
        sockaddr_un addr{};
        addr.sun_family = AF_UNIX;
        std::strncpy(addr.sun_path, path_.c_str(), sizeof(addr.sun_path) - 1);
        if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
            ::close(fd);
            throw std::runtime_error("cannot connect to rokkd at " + path_);
        }
        bool ok = send_message(fd, msg.dump());
        std::string in;
        ok = ok && recv_message(fd, in);
        ::close(fd);
        if (!ok) throw std::runtime_error("rokkd communication error");
        Json r;
        if (!Json::parse(in, r)) throw std::runtime_error("bad response from rokkd");
        if (r.find("ok") && !r.find("ok")->as_bool())
            throw std::runtime_error(r.find("error") ? r.find("error")->as_str() : "rokkd error");
        return r;
    }

    std::string path_;
};

}  // namespace

std::unique_ptr<SearchClient> make_client(const ServiceTarget& t) {
    if (t.kind == ServiceTarget::in_process)
        return std::make_unique<InProcessClient>(t.backend);
    return std::make_unique<SocketClient>(t.path);
}

}  // namespace rokkdoxx::svc
