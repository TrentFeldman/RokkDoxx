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

class SearchClient {
public:
    virtual ~SearchClient() = default;
    virtual std::string backend_name() = 0;
    virtual JobId submit(const SearchRequest&) = 0;
    virtual JobStatus poll(JobId) = 0;
    virtual std::vector<Match> results(JobId) = 0;
    virtual void cancel(JobId) = 0;
};

// `backend` selects the compute device: "" / "auto", "cpu", or "opencl:N".
std::unique_ptr<SearchClient> make_client(const std::string& backend);

}  // namespace rokkdoxx::svc
