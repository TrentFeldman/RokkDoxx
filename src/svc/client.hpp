// SearchClient: what a front-end talks to. Two transports behind one interface:
//   - InProcessClient  -> owns a SearchService in the same process
//   - SocketClient      -> talks to a rokkd daemon over a Unix socket
// Selected by a target string: "local" (default) or "unix:/path/to.sock".
//
// Responsibilities: the SearchClient interface and its two transports.
// Not this file's job: the wire format (protocol.*) or the service itself
// (search_service.*).
#pragma once

#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "service_types.hpp"

namespace rokkdoxx::svc {

struct ServiceTarget {
    enum Kind { in_process, unix_socket } kind = in_process;
    std::string path;
    std::string backend;  // for in_process: which compute backend ("", "cpu", "opencl:0")
};

ServiceTarget parse_target(std::string_view s, std::string backend = "");

class SearchClient {
public:
    virtual ~SearchClient() = default;
    virtual std::string backend_name() = 0;
    virtual JobId submit(const SearchRequest&) = 0;
    virtual JobStatus poll(JobId) = 0;
    virtual std::vector<Match> results(JobId) = 0;
    virtual void cancel(JobId) = 0;
};

std::unique_ptr<SearchClient> make_client(const ServiceTarget&);

}  // namespace rokkdoxx::svc
