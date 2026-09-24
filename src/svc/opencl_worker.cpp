// Host side of the GPU search: pick a device, build the kernel(s), and drive
// tiles through them.
//
// The kernel text is bedrock_core.h concatenated with search_tile.cl and
// compiled at runtime, so the GPU runs the exact same generation math as the
// CPU. Per-tile work is: zero the match counter, set kernel arguments, launch
// a 2D grid of (tile.w x tile.h) work-items, read back the match count and
// then that many matches. OpenclWorker::preferred_tile_side() asks the
// scheduler for large (16384-block) tiles to keep the fixed per-dispatch
// cost a small fraction of each tile's compute time.
//
// Two search kernels, chosen per job in configure(): k_search (plain, every
// work-item recomputes every cell it needs) and k_search_cached (a
// work-group cooperatively fills a __local cache of generated bedrock bits
// covering its tile chunk plus a halo, then reads that instead of
// recomputing -- used only when the halo fits the device's local memory
// budget; see the use_cache computation in configure() and the comment atop
// search_tile_cached in search_tile.cl). run_tile() drives either kernel
// fully synchronously (used by callers that don't pipeline); begin_tile()/
// end_tile() ping-pong two buffer/queue slots so one tile's dispatch can
// overlap another's read-back (see Worker::supports_pipelining).
#include "opencl_worker.hpp"

#include <algorithm>
#include <atomic>
#include <stdexcept>

#define CL_HPP_ENABLE_EXCEPTIONS
#define CL_HPP_TARGET_OPENCL_VERSION 120
#define CL_HPP_MINIMUM_OPENCL_VERSION 120
#define CL_TARGET_OPENCL_VERSION 120  // silences the <CL/cl.h> "defaulting to 300" pragma
#define NOMINMAX                      // in case an SDK header pulls in <windows.h>
#if __has_include(<CL/opencl.hpp>)
#include <CL/opencl.hpp>
#else
#include <CL/cl2.hpp>  // older OpenCL SDKs (some CUDA toolkits) ship only this
#endif

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

// The cached kernel needs an explicit local work-group size, but
// TileScheduler clips edge tiles to the region boundary (see
// TileScheduler::tile_at), so tile.w/tile.h aren't always multiples of the
// group size. OpenCL requires global size to be evenly divisible by a
// non-null local size, so the enqueued global size must be padded up --
// the kernel's own bounds check (folded into `anchor_ok`, applied after
// every barrier) is what makes the padding work-items harmless.
std::size_t round_up(std::size_t v, std::size_t mult) { return ((v + mult - 1) / mult) * mult; }

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
    // Two slots so begin_tile()/end_tile() can ping-pong: one slot's kernel
    // dispatch can run while the other's read-back (from the previous call)
    // is still draining.
    static constexpr int kSlots = 2;

    // Fixed work-group shape for the cached kernel: 256 threads divides
    // evenly by both 32 (NVIDIA warps, AMD RDNA wavefronts) and 64 (AMD GCN
    // wavefronts), so it degrades gracefully in occupancy rather than
    // correctness on whatever wavefront size a given device actually has.
    static constexpr int kGroupW = 16;
    static constexpr int kGroupH = 16;

    cl::Device device;
    cl::Context ctx;
    cl::CommandQueue queue[kSlots];
    cl::Program program;
    cl::Kernel k_search;
    cl::Kernel k_search_cached;
    cl::Kernel k_dump;
    std::string label;
    std::size_t local_mem_size = 0;  // CL_DEVICE_LOCAL_MEM_SIZE, queried once

    WorkerConfig cfg;
    int n_cells = 1;
    int n_variants = 1;
    cl::Buffer buf_var_off;
    cl::Buffer buf_want;
    cl::Buffer buf_var_mask;
    cl::Buffer buf_count[kSlots];
    cl::Buffer buf_xz[kSlots];
    cl::Buffer buf_orient[kSlots];
    std::uint32_t cap = 0;
    std::atomic<bool> trunc{false};

    int next_begin = 0;  // slot the next begin_tile() dispatches into
    int next_end = 0;    // slot the next end_tile() drains
    int inflight = 0;    // tiles begun but not yet ended

    // Local-memory tile cache (Step 10b), computed fresh in configure() for
    // the current pattern: whether the halo fits the device's local memory
    // budget, and if so, the halo size and the local buffer size to pass to
    // the cached kernel. use_cache == false means every dispatch falls back
    // to the plain, unmodified k_search kernel -- a real code path, not a
    // hypothetical, since pattern cell spacing is user-controlled and some
    // patterns will legitimately need it.
    bool use_cache = false;
    int halo_w = 0, halo_h = 0;
    std::size_t cache_bytes = 0;
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
    for (int i = 0; i < Impl::kSlots; ++i) impl_->queue[i] = cl::CommandQueue(impl_->ctx, impl_->device);

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
    impl_->k_search_cached = cl::Kernel(impl_->program, "search_tile_cached");
    impl_->k_dump = cl::Kernel(impl_->program, "dump_plane");

    // Device-specific, not job-specific -- query once and reuse in every
    // configure() call to size the local-memory cache budget.
    impl_->local_mem_size = static_cast<std::size_t>(impl_->device.getInfo<CL_DEVICE_LOCAL_MEM_SIZE>());
}

OpenclWorker::~OpenclWorker() {
    // Make sure no GPU op is still in flight before the buffers/queues in
    // impl_ get torn down (the pump loop always drains before returning, but
    // this is cheap insurance against an early-exit/exception path).
    if (impl_) {
        try {
            for (auto& q : impl_->queue) q.finish();
        } catch (...) {
        }
    }
}

std::string OpenclWorker::name() const { return "opencl: " + impl_->label; }

bool OpenclWorker::truncated() const { return impl_->trunc.load(); }

void OpenclWorker::configure(const WorkerConfig& cfg) {
    impl_->cfg = cfg;
    impl_->trunc.store(false);

    // Shared prep with CpuWorker: anchor pick, recentring, per-variant offset
    // tables, duplicate-orientation collapse. See build_search_plan.
    const SearchPlan plan = build_search_plan(cfg.knowns, cfg.threshold, cfg.all_orientations);
    impl_->n_cells = plan.n_cells;
    impl_->n_variants = plan.n_variants;

    std::vector<cl_int2> var_off(static_cast<std::size_t>(plan.n_variants) * plan.n_cells);
    for (std::size_t i = 0; i < var_off.size(); ++i) {
        var_off[i].s[0] = plan.off_x[i];
        var_off[i].s[1] = plan.off_z[i];
    }
    std::vector<cl_uchar> want(plan.want.begin(), plan.want.end());
    std::vector<cl_uchar> vmask(plan.variant_mask.begin(), plan.variant_mask.end());

    impl_->buf_var_off = cl::Buffer(impl_->ctx, CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR,
                                    var_off.size() * sizeof(cl_int2), var_off.data());
    impl_->buf_want = cl::Buffer(impl_->ctx, CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR,
                                 want.size() * sizeof(cl_uchar), want.data());
    impl_->buf_var_mask = cl::Buffer(impl_->ctx, CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR,
                                     vmask.size() * sizeof(cl_uchar), vmask.data());

    impl_->cap = std::min<std::uint32_t>(cfg.match_cap, 1u << 20);
    for (int i = 0; i < Impl::kSlots; ++i) {
        impl_->buf_count[i] = cl::Buffer(impl_->ctx, CL_MEM_READ_WRITE, sizeof(cl_uint));
        impl_->buf_xz[i] = cl::Buffer(impl_->ctx, CL_MEM_WRITE_ONLY, impl_->cap * sizeof(cl_int2));
        impl_->buf_orient[i] = cl::Buffer(impl_->ctx, CL_MEM_WRITE_ONLY, impl_->cap * sizeof(cl_uchar));
    }
    impl_->next_begin = 0;
    impl_->next_end = 0;
    impl_->inflight = 0;

    // Local-memory tile cache (Step 10b): the halo is the pattern's
    // world-space bounding box relative to the anchor -- how far the fill
    // must reach beyond a work-group's own tile chunk. Pattern cell spacing
    // is user-controlled, so this is computed fresh per job, not assumed.
    //
    // Measured on real hardware: the cached kernel's barriers/atomic_or cost
    // real throughput on every dispatch, gate or no gate, because a
    // barrier-synchronized work-group schedules less flexibly than fully
    // independent work-items -- the any-survivor gate only skips the FILL,
    // not that fixed sync tax. With n_variants == 1 there's no cross-variant
    // redundancy for the cache to amortize that tax against (one variant
    // means the post-anchor work is identical cached or not), and it showed
    // as a real ~30% regression on both the exact and all-8-symmetric
    // benchmark phases. With n_variants == 8 the same tax was paid back many
    // times over (~2x on all-8). So: only use the cache when there's more
    // than one variant to share halo cells across.
    std::int64_t min_dx = 0, max_dx = 0, min_dz = 0, max_dz = 0;  // anchor (0,0) always included
    for (std::int32_t v : plan.off_x) {
        min_dx = std::min<std::int64_t>(min_dx, v);
        max_dx = std::max<std::int64_t>(max_dx, v);
    }
    for (std::int32_t v : plan.off_z) {
        min_dz = std::min<std::int64_t>(min_dz, v);
        max_dz = std::max<std::int64_t>(max_dz, v);
    }
    const std::int64_t halo_w64 = std::max<std::int64_t>(max_dx, -min_dx);
    const std::int64_t halo_h64 = std::max<std::int64_t>(max_dz, -min_dz);

    // Sanity cap well beyond any real pattern, just to avoid doing further
    // arithmetic on a pathological value before the budget check below would
    // reject it anyway.
    constexpr std::int64_t kHaloSanityLimit = 20000;
    if (halo_w64 <= kHaloSanityLimit && halo_h64 <= kHaloSanityLimit) {
        const std::size_t cache_w =
            static_cast<std::size_t>(Impl::kGroupW) + 2 * static_cast<std::size_t>(halo_w64);
        const std::size_t cache_h =
            static_cast<std::size_t>(Impl::kGroupH) + 2 * static_cast<std::size_t>(halo_h64);
        const std::size_t cache_bytes = cache_w * cache_h;  // 1 uchar per cell
        // Cap at a conservative fraction of a conservative local-memory
        // figure -- leaves headroom for driver/runtime-side local
        // allocations this code doesn't control, and never assumes more
        // than 32KB even on a device that reports more.
        const std::size_t budget = std::min<std::size_t>(impl_->local_mem_size, 32 * 1024) * 3 / 4;
        impl_->use_cache = impl_->n_variants > 1 && cache_bytes > 0 && cache_bytes <= budget;
        impl_->halo_w = static_cast<int>(halo_w64);
        impl_->halo_h = static_cast<int>(halo_h64);
        impl_->cache_bytes = cache_bytes;
    } else {
        impl_->use_cache = false;
        impl_->halo_w = impl_->halo_h = 0;
        impl_->cache_bytes = 0;
    }
}

// Zero the counter and launch the kernel into `slot`, non-blocking. Setting
// kernel args and enqueueing happen back-to-back on this thread, so reusing
// the single impl_->k_search object across slots is safe: OpenCL captures a
// kernel's argument values at enqueue time, not at execution time, so a later
// setArg() (for the other slot) can never retroactively change a command
// that's already been enqueued.
void OpenclWorker::enqueue_search(int slot, const Tile& tile) {
    // Blocking: this is a 4-byte write, negligible cost either way, and a
    // non-blocking write here would need the source pointer to stay valid
    // until the driver actually performs the DMA -- `zero` being a stack
    // local that dies when this function returns would then be a real
    // dangling-pointer bug (the double-buffering win comes from not blocking
    // on the multi-KB *read-back*, not from this).
    cl_uint zero = 0;
    impl_->queue[slot].enqueueWriteBuffer(impl_->buf_count[slot], CL_TRUE, 0, sizeof(cl_uint), &zero);

    if (impl_->use_cache) {
        cl::Kernel& k = impl_->k_search_cached;
        cl_uint a = 0;
        k.setArg(a++, static_cast<cl_ulong>(impl_->cfg.derived_lo));
        k.setArg(a++, static_cast<cl_ulong>(impl_->cfg.derived_hi));
        k.setArg(a++, static_cast<cl_int>(impl_->cfg.plane_y));
        k.setArg(a++, static_cast<cl_uint>(impl_->cfg.threshold));
        k.setArg(a++, static_cast<cl_int>(tile.x0));
        k.setArg(a++, static_cast<cl_int>(tile.z0));
        k.setArg(a++, static_cast<cl_int>(tile.w));
        k.setArg(a++, static_cast<cl_int>(tile.h));
        k.setArg(a++, static_cast<cl_int>(impl_->n_cells));
        k.setArg(a++, static_cast<cl_int>(impl_->n_variants));
        k.setArg(a++, impl_->buf_var_off);
        k.setArg(a++, impl_->buf_want);
        k.setArg(a++, impl_->buf_var_mask);
        k.setArg(a++, static_cast<cl_uint>(impl_->cap));
        k.setArg(a++, impl_->buf_count[slot]);
        k.setArg(a++, impl_->buf_xz[slot]);
        k.setArg(a++, impl_->buf_orient[slot]);
        k.setArg(a++, static_cast<cl_int>(impl_->halo_w));
        k.setArg(a++, static_cast<cl_int>(impl_->halo_h));
        k.setArg(a++, cl::Local(impl_->cache_bytes));

        // Global size must be a multiple of the local size -- pad tiles the
        // scheduler clipped to the region edge; the kernel's own bounds
        // check (folded into `anchor_ok`, applied after every barrier)
        // makes the padding work-items harmless.
        const std::size_t gw = round_up(static_cast<std::size_t>(tile.w), Impl::kGroupW);
        const std::size_t gh = round_up(static_cast<std::size_t>(tile.h), Impl::kGroupH);
        impl_->queue[slot].enqueueNDRangeKernel(
            k, cl::NullRange, cl::NDRange(gw, gh),
            cl::NDRange(static_cast<std::size_t>(Impl::kGroupW), static_cast<std::size_t>(Impl::kGroupH)));
        return;
    }

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
    k.setArg(a++, static_cast<cl_int>(impl_->n_cells));
    k.setArg(a++, static_cast<cl_int>(impl_->n_variants));
    k.setArg(a++, impl_->buf_var_off);
    k.setArg(a++, impl_->buf_want);
    k.setArg(a++, impl_->buf_var_mask);
    k.setArg(a++, static_cast<cl_uint>(impl_->cap));
    k.setArg(a++, impl_->buf_count[slot]);
    k.setArg(a++, impl_->buf_xz[slot]);
    k.setArg(a++, impl_->buf_orient[slot]);

    impl_->queue[slot].enqueueNDRangeKernel(k, cl::NullRange,
                                            cl::NDRange(static_cast<std::size_t>(tile.w),
                                                        static_cast<std::size_t>(tile.h)),
                                            cl::NullRange);
}

// Blocking read-back of whatever `slot`'s kernel found. The blocking
// enqueueReadBuffer call is enough by itself to wait for that slot's queue to
// finish (in-order queue: the read is ordered after the write+kernel), so no
// separate finish()/wait is needed here.
std::vector<Match> OpenclWorker::read_results(int slot) {
    cl_uint n = 0;
    impl_->queue[slot].enqueueReadBuffer(impl_->buf_count[slot], CL_TRUE, 0, sizeof(cl_uint), &n);
    const cl_uint keep = std::min(n, impl_->cap);

    std::vector<Match> out;
    if (keep) {
        std::vector<cl_int2> xz(keep);
        std::vector<cl_uchar> om(keep);
        impl_->queue[slot].enqueueReadBuffer(impl_->buf_xz[slot], CL_TRUE, 0, keep * sizeof(cl_int2),
                                             xz.data());
        impl_->queue[slot].enqueueReadBuffer(impl_->buf_orient[slot], CL_TRUE, 0, keep * sizeof(cl_uchar),
                                             om.data());
        out.reserve(keep);
        for (cl_uint i = 0; i < keep; ++i)
            out.push_back({xz[i].s[0], xz[i].s[1], static_cast<std::uint8_t>(om[i])});
    }
    if (n > impl_->cap) impl_->trunc.store(true);
    return out;
}

std::vector<Match> OpenclWorker::run_tile(const Tile& tile) {
    // Fully synchronous single-slot path: used by the benchmark's warm-up
    // sweeps and any non-pipelined caller. The blocking read in
    // read_results() waits for this slot's write+kernel too (in-order
    // queue), so this behaves exactly as it did before begin_tile/end_tile
    // existed.
    enqueue_search(0, tile);
    return read_results(0);
}

void OpenclWorker::begin_tile(const Tile& tile) {
    const int s = impl_->next_begin;
    enqueue_search(s, tile);
    impl_->queue[s].flush();  // push to the device now; don't wait for a later blocking call
    impl_->next_begin = (s + 1) % Impl::kSlots;
    ++impl_->inflight;
}

std::vector<Match> OpenclWorker::end_tile() {
    const int s = impl_->next_end;
    std::vector<Match> out = read_results(s);
    impl_->next_end = (s + 1) % Impl::kSlots;
    --impl_->inflight;
    return out;
}

int OpenclWorker::pending_tiles() const { return impl_->inflight; }

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
    impl_->queue[0].enqueueNDRangeKernel(
        k, cl::NullRange,
        cl::NDRange(static_cast<std::size_t>(w), static_cast<std::size_t>(h)), cl::NullRange);
    std::vector<std::uint8_t> host(static_cast<std::size_t>(w) * h);
    impl_->queue[0].enqueueReadBuffer(out, CL_TRUE, 0, host.size() * sizeof(cl_uchar), host.data());
    return host;
}

}  // namespace rokkdoxx::svc
