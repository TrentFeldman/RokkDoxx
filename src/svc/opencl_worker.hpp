// OpenclWorker: one OpenCL device running the search kernel. Only compiled when
// the build finds OpenCL (ROKK_ENABLE_OPENCL). The generation math is the shared
// src/gen/bedrock_core.h, so the kernel is bit-identical to CpuWorker.
#pragma once

#include <memory>
#include <string>
#include <vector>

#include "service_types.hpp"

namespace rokkdoxx::svc {

struct OpenclDevice {
    int index = 0;  // position in the flattened platform x device list
    std::string label;
    std::string platform;
    std::string device;
    std::string cl_version;      // CL_DEVICE_VERSION, e.g. "OpenCL 2.0 AMD-APP..."
    std::string driver_version;  // CL_DRIVER_VERSION
    int compute_units = 0;       // CL_DEVICE_MAX_COMPUTE_UNITS
};

// Enumerate every OpenCL device on the host (empty if no platform / ICD).
std::vector<OpenclDevice> opencl_list_devices();

class OpenclWorker : public Worker {
public:
    explicit OpenclWorker(int device_index);
    ~OpenclWorker() override;

    std::string name() const override;
    void configure(const WorkerConfig& cfg) override;
    std::vector<Match> run_tile(const Tile& tile) override;
    bool truncated() const override;
    int preferred_tile_side() const override { return 16384; }

    // Direct plane dump for the bit-exactness test (row-major w*h, 1 = bedrock).
    std::vector<std::uint8_t> dump_plane(std::uint64_t derived_lo, std::uint64_t derived_hi,
                                         int plane_y, std::uint32_t threshold, int x0, int z0, int w,
                                         int h);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace rokkdoxx::svc
