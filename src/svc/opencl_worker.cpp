// Host side of the GPU search: pick a device, build the kernel, and run one
// dispatch + read-back per tile.
//
// The kernel text is bedrock_core.h concatenated with search_tile.cl and
// compiled at runtime, so the GPU runs the exact same generation math as the
// CPU. Per-tile work is: zero the match counter, set 16 kernel arguments, launch
// a 2D grid of (tile.w x tile.h) work-items, block on reading the counter back,
// then copy out that many matches. A launch + blocking read-back costs ~1-3 ms
// no matter the tile size, which is why OpenclWorker::preferred_tile_side()
// asks the scheduler for large (16384-block) tiles.
#include "opencl_worker.hpp"

#include <algorithm>
#include <atomic>
#include <stdexcept>

#define CL_HPP_ENABLE_EXCEPTIONS
#define CL_HPP_TARGET_OPENCL_VERSION 120
#define CL_HPP_MINIMUM_OPENCL_VERSION 120
#include <CL/opencl.hpp>

// Generated at configure time by CMake (build.sh writes an equivalent header):
// the text of src/gen/bedrock_core.h and src/cl/search_tile.cl as string
// literals, so the binary carries its own kernel and is relocatable.
#include "kernel_sources.h"

namespace rokkdoxx::svc {

namespace {

std::string trimmed(std::string s) {
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\0')) s.pop_back();
    return s;
}

std::string kernel_source() {
    return std::string(kBedrockCoreSrc) + "\n\n" + kSearchTileSrc;
}

std::vector<cl::Device> flat_devices(std::vector<std::string>* labels = nullptr) {
    std::vector<cl::Device> all;
    std::vector<cl::Platform> plats;
    cl::Platform::get(&plats);
    for (auto& p : plats) {
        std::vector<cl::Device> devs;
        try {
            p.getDevices(CL_DEVICE_TYPE_ALL, &devs);
        } catch (...) {
            continue;
        }
        std::string pn;
        try {
            pn = p.getInfo<CL_PLATFORM_NAME>();
        } catch (...) {
        }
        for (auto& d : devs) {
            all.push_back(d);
            if (labels) {
                std::string dn;
                try {
                    dn = d.getInfo<CL_DEVICE_NAME>();
                } catch (...) {
                }
                labels->push_back(pn + " / " + dn);
            }
        }
    }
    return all;
}

}  // namespace

std::vector<OpenclDevice> opencl_list_devices() {
    std::vector<OpenclDevice> out;
    try {
        std::vector<std::string> labels;
        auto devs = flat_devices(&labels);
        for (std::size_t i = 0; i < devs.size(); ++i) {
            OpenclDevice d;
            d.index = static_cast<int>(i);
            d.label = labels[i];
            try {
                d.device = devs[i].getInfo<CL_DEVICE_NAME>();
            } catch (...) {
            }
            try {
                d.cl_version = trimmed(devs[i].getInfo<CL_DEVICE_VERSION>());
            } catch (...) {
            }
            try {
                d.driver_version = trimmed(devs[i].getInfo<CL_DRIVER_VERSION>());
            } catch (...) {
            }
            try {
                d.compute_units = static_cast<int>(devs[i].getInfo<CL_DEVICE_MAX_COMPUTE_UNITS>());
            } catch (...) {
            }
            out.push_back(std::move(d));
        }
    } catch (...) {
    }
    return out;
}

struct OpenclWorker::Impl {
    cl::Device device;
    cl::Context ctx;
    cl::CommandQueue queue;
    cl::Program program;
    cl::Kernel k_search;
    cl::Kernel k_dump;
    std::string label;

    WorkerConfig cfg;
    int n_orient = 1;
    cl::Buffer buf_rel;
    cl::Buffer buf_want;
    cl::Buffer buf_count;
    cl::Buffer buf_xz;
    cl::Buffer buf_orient;
    std::uint32_t cap = 0;
    std::atomic<bool> trunc{false};
};

OpenclWorker::OpenclWorker(int device_index) : impl_(std::make_unique<Impl>()) {
    std::vector<std::string> labels;
    auto devs = flat_devices(&labels);
    if (devs.empty()) throw std::runtime_error("no OpenCL devices found");
    if (device_index < 0 || device_index >= static_cast<int>(devs.size()))
        throw std::runtime_error("OpenCL device index out of range");

    impl_->device = devs[static_cast<std::size_t>(device_index)];
    impl_->label = labels[static_cast<std::size_t>(device_index)];
    impl_->ctx = cl::Context(impl_->device);
    impl_->queue = cl::CommandQueue(impl_->ctx, impl_->device);

    impl_->program = cl::Program(impl_->ctx, kernel_source());
    try {
        impl_->program.build("-cl-std=CL1.2");
    } catch (const cl::Error&) {
        std::string log;
        try {
            log = impl_->program.getBuildInfo<CL_PROGRAM_BUILD_LOG>(impl_->device);
        } catch (...) {
        }
        throw std::runtime_error("OpenCL kernel build failed:\n" + log);
    }
    impl_->k_search = cl::Kernel(impl_->program, "search_tile");
    impl_->k_dump = cl::Kernel(impl_->program, "dump_plane");
}

OpenclWorker::~OpenclWorker() = default;

std::string OpenclWorker::name() const { return "opencl: " + impl_->label; }

bool OpenclWorker::truncated() const { return impl_->trunc.load(); }

void OpenclWorker::configure(const WorkerConfig& cfg) {
    impl_->cfg = cfg;
    impl_->n_orient = cfg.all_orientations ? 8 : 1;
    impl_->trunc.store(false);

    std::vector<cl_int2> rel;
    std::vector<cl_uchar> want;
    // Fail-fast ordering, same rule as CpuWorker.
    auto ks = cfg.knowns;
    const bool bedrock_rare = cfg.threshold <= (1u << 23);
    std::stable_sort(ks.begin(), ks.end(), [&](const KnownCell& a, const KnownCell& b) {
        return ((a.want == 1) == bedrock_rare) && ((b.want == 1) != bedrock_rare);
    });
    for (const auto& kc : ks) {
        cl_int2 v;
        v.s[0] = kc.i;
        v.s[1] = kc.j;
        rel.push_back(v);
        want.push_back(static_cast<cl_uchar>(kc.want));
    }
    if (rel.empty()) {
        cl_int2 v;
        v.s[0] = 0;
        v.s[1] = 0;
        rel.push_back(v);
        want.push_back(1);
    }

    impl_->buf_rel = cl::Buffer(impl_->ctx, CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR,
                                rel.size() * sizeof(cl_int2), rel.data());
    impl_->buf_want = cl::Buffer(impl_->ctx, CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR,
                                 want.size() * sizeof(cl_uchar), want.data());

    impl_->cap = std::min<std::uint32_t>(cfg.match_cap, 1u << 20);
    impl_->buf_count = cl::Buffer(impl_->ctx, CL_MEM_READ_WRITE, sizeof(cl_uint));
    impl_->buf_xz = cl::Buffer(impl_->ctx, CL_MEM_WRITE_ONLY, impl_->cap * sizeof(cl_int2));
    impl_->buf_orient = cl::Buffer(impl_->ctx, CL_MEM_WRITE_ONLY, impl_->cap * sizeof(cl_uchar));
}

std::vector<Match> OpenclWorker::run_tile(const Tile& tile) {
    cl_uint zero = 0;
    impl_->queue.enqueueWriteBuffer(impl_->buf_count, CL_TRUE, 0, sizeof(cl_uint), &zero);

    cl::Kernel& k = impl_->k_search;
    cl_uint a = 0;
    k.setArg(a++, static_cast<cl_ulong>(impl_->cfg.derived_lo));
    k.setArg(a++, static_cast<cl_ulong>(impl_->cfg.derived_hi));
    k.setArg(a++, static_cast<cl_int>(impl_->cfg.plane_y));
    k.setArg(a++, static_cast<cl_uint>(impl_->cfg.threshold));
    k.setArg(a++, static_cast<cl_int>(tile.x0));
    k.setArg(a++, static_cast<cl_int>(tile.z0));
    k.setArg(a++, static_cast<cl_int>(tile.w));
    k.setArg(a++, static_cast<cl_int>(tile.h));
    k.setArg(a++, static_cast<cl_int>(impl_->cfg.knowns.size()));
    k.setArg(a++, static_cast<cl_int>(impl_->n_orient));
    k.setArg(a++, impl_->buf_rel);
    k.setArg(a++, impl_->buf_want);
    k.setArg(a++, static_cast<cl_uint>(impl_->cap));
    k.setArg(a++, impl_->buf_count);
    k.setArg(a++, impl_->buf_xz);
    k.setArg(a++, impl_->buf_orient);

    impl_->queue.enqueueNDRangeKernel(k, cl::NullRange,
                                      cl::NDRange(static_cast<std::size_t>(tile.w),
                                                  static_cast<std::size_t>(tile.h)),
                                      cl::NullRange);

    cl_uint n = 0;
    impl_->queue.enqueueReadBuffer(impl_->buf_count, CL_TRUE, 0, sizeof(cl_uint), &n);
    const cl_uint keep = std::min(n, impl_->cap);

    std::vector<Match> out;
    if (keep) {
        std::vector<cl_int2> xz(keep);
        std::vector<cl_uchar> om(keep);
        impl_->queue.enqueueReadBuffer(impl_->buf_xz, CL_TRUE, 0, keep * sizeof(cl_int2), xz.data());
        impl_->queue.enqueueReadBuffer(impl_->buf_orient, CL_TRUE, 0, keep * sizeof(cl_uchar),
                                       om.data());
        out.reserve(keep);
        for (cl_uint i = 0; i < keep; ++i)
            out.push_back({xz[i].s[0], xz[i].s[1], static_cast<std::uint8_t>(om[i])});
    }
    if (n > impl_->cap) impl_->trunc.store(true);
    return out;
}

std::vector<std::uint8_t> OpenclWorker::dump_plane(std::uint64_t dlo, std::uint64_t dhi, int plane_y,
                                                  std::uint32_t threshold, int x0, int z0, int w,
                                                  int h) {
    cl::Buffer out(impl_->ctx, CL_MEM_WRITE_ONLY,
                   static_cast<std::size_t>(w) * h * sizeof(cl_uchar));
    cl::Kernel& k = impl_->k_dump;
    cl_uint a = 0;
    k.setArg(a++, static_cast<cl_ulong>(dlo));
    k.setArg(a++, static_cast<cl_ulong>(dhi));
    k.setArg(a++, static_cast<cl_int>(plane_y));
    k.setArg(a++, static_cast<cl_uint>(threshold));
    k.setArg(a++, static_cast<cl_int>(x0));
    k.setArg(a++, static_cast<cl_int>(z0));
    k.setArg(a++, static_cast<cl_int>(w));
    k.setArg(a++, static_cast<cl_int>(h));
    k.setArg(a++, out);
    impl_->queue.enqueueNDRangeKernel(
        k, cl::NullRange,
        cl::NDRange(static_cast<std::size_t>(w), static_cast<std::size_t>(h)), cl::NullRange);
    std::vector<std::uint8_t> host(static_cast<std::size_t>(w) * h);
    impl_->queue.enqueueReadBuffer(out, CL_TRUE, 0, host.size() * sizeof(cl_uchar), host.data());
    return host;
}

}  // namespace rokkdoxx::svc
