#include "client.hpp"

#include "search_service.hpp"
#include "workers.hpp"

namespace rokkdoxx::svc {

namespace {

// The service lives in this process; calls forward straight to it.
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

}  // namespace

std::unique_ptr<SearchClient> make_client(const std::string& backend) {
    return std::make_unique<InProcessClient>(backend);
}

}  // namespace rokkdoxx::svc
