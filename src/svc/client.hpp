// SearchClient: the thin handle a front-end holds. It owns a SearchService that
// runs in the same process -- there is no daemon and no IPC.
//
// The interface is kept as a seam so a front-end never touches SearchService
// directly; if an out-of-process transport is ever needed again, it slots in
// here.
//
// Responsibilities: the SearchClient interface + the in-process implementation.
// Not this file's job: the service itself (search_service.*) or the compute
// (workers.*, opencl_worker.*).
#pragma once

#include <memory>
#include <string>
#include <vector>

#include "service_types.hpp"

namespace rokkdoxx::svc {

// Every method forwards to the SearchService of the same name; see
// search_service.hpp for the exact semantics. Typical use (rokksearch main()):
//
//   auto client = make_client("auto");
//   JobId id = client->submit(req);
//   while (true) { JobStatus st = client->poll(id); ...; if (ended) break; sleep; }
//   std::vector<Match> found = client->results(id);
//
// Destroying the client cancels and joins any job still running.
class SearchClient {
public:
    virtual ~SearchClient() = default;
    virtual std::string backend_name() = 0;  // human-readable device label
    virtual JobId submit(const SearchRequest&) = 0;
    virtual JobStatus poll(JobId) = 0;
    virtual std::vector<Match> results(JobId) = 0;
    virtual void cancel(JobId) = 0;
    virtual void pause(JobId, bool on) = 0;     // see SearchService::pause
    virtual std::string checkpoint(JobId) = 0;  // see SearchService::checkpoint
};

// `backend` selects the compute device: "" / "auto", "cpu", "dedicated",
// "integrated", or "opencl:N" (see resolve_backend). Throws std::runtime_error
// if it names nothing usable. It lists the devices (asks the OpenCL driver)
// but opens none; each job opens its own when it starts.
std::unique_ptr<SearchClient> make_client(const std::string& backend);

}  // namespace rokkdoxx::svc
