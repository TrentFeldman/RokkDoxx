// The always-available compute backend (CpuWorker) and the backend picker.
//
// Responsibilities:
//   - CpuWorker: a multi-threaded CPU scan of a tile. It is the fallback when
//     there is no GPU, and the correctness reference the GPU is checked against.
//   - list_backends / make_worker_factory: name the devices this build can use
//     and hand back a factory for the one the caller asked for.
// Not this file's job: the GPU path (opencl_worker.*, compiled only when OpenCL
// is found), or orchestration (search_service.*).
#pragma once

#include <atomic>
#include <string>
#include <vector>

#include "service_types.hpp"

namespace rokkdoxx::svc {

class CpuWorker : public Worker {
public:
    explicit CpuWorker(unsigned threads = 0);  // 0 -> hardware_concurrency

    std::string name() const override;
    void configure(const WorkerConfig& cfg) override;
    std::vector<Match> run_tile(const Tile& tile) override;
    bool truncated() const override { return truncated_.load(); }

private:
    unsigned threads_;
    WorkerConfig cfg_;
    std::vector<KnownCell> ordered_;  // known cells in fail-fast order
    std::atomic<bool> truncated_{false};
    std::atomic<std::uint64_t> emitted_{0};
};

// --- backend selection -------------------------------------------------

struct BackendInfo {
    std::string id;      // "cpu", "opencl:0", ...
    std::string label;   // human-readable
    bool is_gpu = false;
};

// Every backend runnable on this build + host.
std::vector<BackendInfo> list_backends();

// A factory for backend `id`. "auto" (or empty) -> first GPU if any, else cpu.
// Throws std::runtime_error if `id` is unknown or unavailable.
WorkerFactory make_worker_factory(const std::string& id);

}  // namespace rokkdoxx::svc
