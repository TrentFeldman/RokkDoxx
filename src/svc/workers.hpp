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
#include <deque>
#include <string>
#include <vector>

#include "service_types.hpp"

namespace rokkdoxx::svc {

// Scans a tile with `threads` std::threads, each taking every T-th row. Spawns
// fresh threads per tile (no pool): a 4096^2 tile is tens of milliseconds of
// work, so thread start-up is a small fraction of it.
// Synchronous: begin_tile scans the tile, end_tile hands back the oldest result.
class CpuWorker : public Worker {
public:
    explicit CpuWorker(unsigned threads = 0);  // 0 -> hardware_concurrency

    void configure(const WorkerConfig& cfg) override;
    void begin_tile(const Tile& tile) override { done_.push_back(run_tile(tile)); }
    std::vector<Match> end_tile() override {
        std::vector<Match> m = std::move(done_.front());
        done_.pop_front();
        return m;
    }
    bool truncated() const override { return truncated_.load(); }

private:
    std::vector<Match> run_tile(const Tile& tile);

    unsigned threads_;
    WorkerConfig cfg_;
    SearchPlan plan_;  // shared anchor + per-variant offsets (build_search_plan)
    std::atomic<bool> truncated_{false};
    // Matches emitted since configure(), across all tiles of the job (the GPU
    // counts per tile instead); shared by the scan threads, hence atomic.
    std::atomic<std::uint64_t> emitted_{0};
    std::deque<std::vector<Match>> done_;  // scanned, not yet collected
};

// --- backend selection -------------------------------------------------

// One compute device as the front-ends show it (rokksearch --list-backends,
// the TUI's backend field, the benchmark header).
struct BackendInfo {
    std::string id;       // "cpu", "opencl:0", ...
    std::string label;    // human-readable
    bool is_gpu = false;
    bool integrated = false;  // a GPU that shares host memory (iGPU / APU)
    std::string version;  // GPU: CL_DEVICE_VERSION; CPU: empty
    std::string driver;   // GPU: CL_DRIVER_VERSION; CPU: empty
    int units = 0;        // GPU: compute units; CPU: hardware threads
};

// Every backend runnable on this build + host. "cpu" is always first; OpenCL
// devices follow in platform x device order (their index is the N of
// "opencl:N"). Queries the OpenCL driver each call, so it is not free.
std::vector<BackendInfo> list_backends();

// Resolve a backend name to one of list_backends(). `id` is "cpu", "opencl:N",
// "dedicated" / "integrated" (the first GPU of that kind), or "auto" (or empty):
// dedicated GPU, else integrated GPU, else cpu.
// Throws std::runtime_error if `id` is unknown or unavailable.
BackendInfo resolve_backend(const std::string& id);

// A factory for the backend `id` names (see resolve_backend). Throws the same way.
// Only the name is resolved here; the device itself is opened (and an OpenCL
// kernel compiled) each time the factory is called, i.e. per job.
// Called by InProcessClient (client.cpp), rokksearch's open_worker, and tests.
WorkerFactory make_worker_factory(const std::string& id);

}  // namespace rokkdoxx::svc
