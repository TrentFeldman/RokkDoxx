// OpenclWorker: one OpenCL device running the search kernel. Only compiled when
// the build finds OpenCL (ROKK_ENABLE_OPENCL). The generation math is the shared
// src/gen/bedrock_core.h, so the kernel is bit-identical to CpuWorker.
//
// Created by the factory from make_worker_factory("opencl:N") (workers.cpp),
// once per job, on the job's thread. The host-side walkthrough is at the top
// of opencl_worker.cpp; the kernels are in src/cl/search_tile.cl.
#pragma once

#include <memory>
#include <vector>

#include "workers.hpp"

namespace rokkdoxx::svc {

// Every OpenCL device on the host as BackendInfo "opencl:<N>", label
// "<platform> / <device>" (empty if no platform / ICD).
std::vector<BackendInfo> opencl_list_devices();

class OpenclWorker : public Worker {
public:
    // Opens device `device_index` (the N of "opencl:N"), creates its context
    // and queues, and compiles the generic kernels. Throws std::runtime_error
    // (with the compiler log) if the device is missing or the build fails.
    explicit OpenclWorker(int device_index);
    ~OpenclWorker() override;

    void configure(const WorkerConfig& cfg) override;
    bool truncated() const override;
    int preferred_tile_side() const override { return 16384; }

    // Two ping-ponged buffer/queue slots: begin_tile(N) dispatches into
    // whichever slot end_tile() last drained, so tile N's kernel can run
    // while tile N-1's read-back (or host-side result processing) is still
    // in flight -- hides the fixed ~1-3ms per-tile dispatch/read-back cost.
    void begin_tile(const Tile& tile) override;
    std::vector<Match> end_tile() override;

    // Direct plane dump for the bit-exactness test (row-major w*h, 1 = bedrock).
    // Java Edition only (it calls rk_bits24_at). Synchronous; independent of
    // configure(). Used by tests/test_gpu.cpp.
    std::vector<std::uint8_t> dump_plane(std::uint64_t derived_lo, std::uint64_t derived_hi,
                                         int plane_y, std::uint32_t threshold, int x0, int z0, int w,
                                         int h);

private:
    struct Impl;  // all OpenCL objects; keeps <CL/opencl.hpp> out of this header
    std::unique_ptr<Impl> impl_;

    void enqueue_search(int slot, const Tile& tile);  // non-blocking dispatch
    std::vector<Match> read_results(int slot);        // blocking read-back
};

}  // namespace rokkdoxx::svc
