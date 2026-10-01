// Host side of the GPU search: pick a device, build the kernels, and drive
// tiles through them.
//
// The kernel text is bedrock_core.h concatenated with search_tile.cl and
// compiled at runtime, so the GPU runs the exact same generation math as the
// CPU. Per tile: zero the match counter; for each kChunk x kChunk piece run
// fill_plane (draw every block of the piece plus the pattern's halo once, one
// bit each) then match_plane (32 candidates per work-item, tested with word
// ops); then read back the match count and that many matches. Drawing each
// block once is what makes all 8 orientations nearly as cheap as one.
// OpenclWorker::preferred_tile_side() asks the scheduler for large
// (16384-block) tiles to keep the fixed per-dispatch cost small.
//
// match_plane is rebuilt per pattern with the first cells' offsets baked in as
// literals (specialized_source); if that build fails the generic kernel is used.
//
// A pattern whose halo exceeds kPlaneHaloMax falls back to search_tile (every
// work-item recomputes every cell it needs), so the plane buffer stays
// bounded. begin_tile()/end_tile() ping-pong two buffer/queue slots so one
// tile's dispatch can overlap another's read-back; run_tile() is the fully
// synchronous single-slot form.
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

// match_plane with this pattern baked in. The per-variant loop (between the rk:variants
// markers in search_tile.cl) becomes straight-line code: each variant's first
// kInlineCells cells get their offsets and wants as literals, and the rest keep the
// generic loop (inlining every cell measured slower). Same results as the generic
// kernel, ~7-20% faster on a 7900 XTX. Empty if the markers are missing.
constexpr int kInlineCells = 6;

std::string specialized_source(const SearchPlan& plan) {
    std::string src = kernel_source();
    const std::string begin = "// rk:variants-begin", end = "// rk:variants-end";
    const std::size_t fn = src.find("__kernel void match_plane");
    const std::size_t b = src.find(begin, fn);
    const std::size_t e = src.find(end, b);
    if (fn == std::string::npos || b == std::string::npos || e == std::string::npos) return {};

    const int baked = std::min(kInlineCells, plan.n_cells);
    std::string code = "\n";
    for (int v = 0; v < plan.n_variants; ++v) {
        code += "    { uint m = alive;\n";
        for (int c = 1; c < baked; ++c) {
            const std::size_t i = static_cast<std::size_t>(v) * plan.n_cells + c;
            code += std::string("      if (m) m &= ") + (plan.want[c] ? "" : "~") +
                    "plane_bits(plane, words_w, row + (" + std::to_string(plan.off_z[i]) +
                    "), col + (" + std::to_string(plan.off_x[i]) + "));\n";
        }
        if (baked < plan.n_cells)
            code += "      for (int c = " + std::to_string(baked) + "; c < n_cells && m; ++c) {"
                    " int2 o = var_off[" + std::to_string(v) + " * n_cells + c];"
                    " const uint w = plane_bits(plane, words_w, row + o.y, col + o.x);"
                    " m &= want[c] ? w : ~w; }\n";
        code += "      mv[" + std::to_string(v) + "] = m; any |= m; }\n";
    }
    src.replace(b + begin.size(), e - (b + begin.size()), code);
    return src;
}

// The plane kernels run with an explicit work-group size, but piece sizes are
// arbitrary (TileScheduler clips edge tiles to the region), and OpenCL wants
// the global size to be a multiple of a non-null local size -- so it is padded
// up. Both kernels bounds-check, which makes the padding work-items harmless.
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
            try {
                d.is_cpu = (devs[i].getInfo<CL_DEVICE_TYPE>() & CL_DEVICE_TYPE_CPU) != 0;
                d.integrated = !d.is_cpu && devs[i].getInfo<CL_DEVICE_HOST_UNIFIED_MEMORY>() != CL_FALSE;
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

    cl::Device device;
    cl::Context ctx;
    cl::CommandQueue queue[kSlots];
    cl::Program program;
    cl::Kernel k_search;
    cl::Kernel k_dump;
    std::string label;

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

    // Bit-plane path (fill_plane + match_plane). A tile is processed in
    // kChunk x kChunk pieces so the per-slot plane buffer has a fixed size
    // (~34 MB at halo 31) whatever tile side the scheduler picked. A halo
    // past kPlaneHaloMax -- a huge, sparse pattern -- uses k_search instead.
    static constexpr int kChunk = 16384;
    static constexpr int kPlaneHaloMax = 1024;
    static constexpr int kLocalW = 32, kLocalH = 8;  // measured fastest on a 7900 XTX
    cl::Kernel k_fill, k_match;
    cl::Program specialized_program;  // the current pattern's match_plane (one at a time)
    std::string specialized_src;      // its source, so an unchanged pattern isn't rebuilt
    cl::Buffer buf_plane[kSlots];
    bool use_plane = false;
    int halo_w = 0, halo_h = 0;
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
    impl_->k_dump = cl::Kernel(impl_->program, "dump_plane");
    impl_->k_fill = cl::Kernel(impl_->program, "fill_plane");
    impl_->k_match = cl::Kernel(impl_->program, "match_plane");
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

    // The halo is the pattern's world-space reach from the anchor over every
    // variant: how far fill_plane must draw beyond a piece's own candidates.
    // Pattern cell spacing is user-controlled, so it is computed per job.
    std::int64_t min_dx = 0, max_dx = 0, min_dz = 0, max_dz = 0;  // anchor (0,0) always included
    for (std::int32_t v : plan.off_x) {
        min_dx = std::min<std::int64_t>(min_dx, v);
        max_dx = std::max<std::int64_t>(max_dx, v);
    }
    for (std::int32_t v : plan.off_z) {
        min_dz = std::min<std::int64_t>(min_dz, v);
        max_dz = std::max<std::int64_t>(max_dz, v);
    }
    const std::int64_t halo_w = std::max(max_dx, -min_dx);
    const std::int64_t halo_h = std::max(max_dz, -min_dz);
    impl_->halo_w = static_cast<int>(halo_w);
    impl_->halo_h = static_cast<int>(halo_h);
    impl_->use_plane = halo_w <= Impl::kPlaneHaloMax && halo_h <= Impl::kPlaneHaloMax;
    if (impl_->use_plane) {
        const std::size_t words = (Impl::kChunk + 2 * halo_w + 31) / 32 + 1;
        const std::size_t rows = Impl::kChunk + 2 * halo_h;
        for (int i = 0; i < Impl::kSlots; ++i)
            impl_->buf_plane[i] = cl::Buffer(impl_->ctx, CL_MEM_READ_WRITE, words * rows * sizeof(cl_uint));

        // ~200 ms the first time a pattern is seen (the driver caches repeats).
        try {
            const std::string src = specialized_source(plan);
            if (src.empty()) throw std::runtime_error("kernel markers missing");
            if (src != impl_->specialized_src) {
                cl::Program p(impl_->ctx, src);
                p.build("-cl-std=CL1.2");
                impl_->specialized_program = p;
                impl_->specialized_src = src;
            }
            impl_->k_match = cl::Kernel(impl_->specialized_program, "match_plane");
        } catch (const std::exception&) {
            impl_->k_match = cl::Kernel(impl_->program, "match_plane");  // oh-shit fallback: generic, silently
        }
    }
}

// Zero the counter and launch the kernel(s) into `slot`, non-blocking. Setting
// kernel args and enqueueing happen back-to-back on this thread, so reusing
// one cl::Kernel object across slots and pieces is safe: OpenCL captures a
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

    if (impl_->use_plane) {
        const int hw = impl_->halo_w, hh = impl_->halo_h;
        const cl::NDRange local(Impl::kLocalW, Impl::kLocalH);
        for (int cz = 0; cz < tile.h; cz += Impl::kChunk)
            for (int cx = 0; cx < tile.w; cx += Impl::kChunk) {
                const std::int64_t x0 = tile.x0 + cx, z0 = tile.z0 + cz;
                const int w = std::min(Impl::kChunk, tile.w - cx);
                const int h = std::min(Impl::kChunk, tile.h - cz);
                const int words_w = (w + 2 * hw + 31) / 32 + 1;
                const int rows = h + 2 * hh;

                cl::Kernel& f = impl_->k_fill;
                cl_uint a = 0;
                f.setArg(a++, static_cast<cl_ulong>(impl_->cfg.derived_lo));
                f.setArg(a++, static_cast<cl_ulong>(impl_->cfg.derived_hi));
                f.setArg(a++, static_cast<cl_int>(impl_->cfg.plane_y));
                f.setArg(a++, static_cast<cl_uint>(impl_->cfg.threshold));
                f.setArg(a++, static_cast<cl_int>(x0 - hw));
                f.setArg(a++, static_cast<cl_int>(z0 - hh));
                f.setArg(a++, static_cast<cl_int>(words_w));
                f.setArg(a++, static_cast<cl_int>(rows));
                f.setArg(a++, impl_->buf_plane[slot]);
                impl_->queue[slot].enqueueNDRangeKernel(
                    f, cl::NullRange,
                    cl::NDRange(round_up(words_w, Impl::kLocalW), round_up(rows, Impl::kLocalH)), local);

                cl::Kernel& m = impl_->k_match;
                a = 0;
                m.setArg(a++, static_cast<cl_int>(x0));
                m.setArg(a++, static_cast<cl_int>(z0));
                m.setArg(a++, static_cast<cl_int>(w));
                m.setArg(a++, static_cast<cl_int>(h));
                m.setArg(a++, static_cast<cl_int>(hw));
                m.setArg(a++, static_cast<cl_int>(hh));
                m.setArg(a++, static_cast<cl_int>(words_w));
                m.setArg(a++, impl_->buf_plane[slot]);
                m.setArg(a++, static_cast<cl_int>(impl_->n_cells));
                m.setArg(a++, static_cast<cl_int>(impl_->n_variants));
                m.setArg(a++, impl_->buf_var_off);
                m.setArg(a++, impl_->buf_want);
                m.setArg(a++, impl_->buf_var_mask);
                m.setArg(a++, static_cast<cl_uint>(impl_->cap));
                m.setArg(a++, impl_->buf_count[slot]);
                m.setArg(a++, impl_->buf_xz[slot]);
                m.setArg(a++, impl_->buf_orient[slot]);
                impl_->queue[slot].enqueueNDRangeKernel(
                    m, cl::NullRange,
                    cl::NDRange(round_up((w + 31) / 32, Impl::kLocalW), round_up(h, Impl::kLocalH)), local);
            }
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
    impl_->queue[slot].enqueueNDRangeKernel(
        k, cl::NullRange,
        cl::NDRange(static_cast<std::size_t>(tile.w), static_cast<std::size_t>(tile.h)), cl::NullRange);
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
    // Fully synchronous single-slot path (the pump uses begin/end_tile). The
    // blocking read in read_results() waits for this slot's write+kernel too
    // (in-order queue).
    enqueue_search(0, tile);
    return read_results(0);
}

void OpenclWorker::begin_tile(const Tile& tile) {
    const int s = impl_->next_begin;
    enqueue_search(s, tile);
    impl_->queue[s].flush();  // push to the device now; don't wait for a later blocking call
    impl_->next_begin = (s + 1) % Impl::kSlots;
}

std::vector<Match> OpenclWorker::end_tile() {
    const int s = impl_->next_end;
    std::vector<Match> out = read_results(s);
    impl_->next_end = (s + 1) % Impl::kSlots;
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
    impl_->queue[0].enqueueNDRangeKernel(
        k, cl::NullRange,
        cl::NDRange(static_cast<std::size_t>(w), static_cast<std::size_t>(h)), cl::NullRange);
    std::vector<std::uint8_t> host(static_cast<std::size_t>(w) * h);
    impl_->queue[0].enqueueReadBuffer(out, CL_TRUE, 0, host.size() * sizeof(cl_uchar), host.data());
    return host;
}

}  // namespace rokkdoxx::svc
