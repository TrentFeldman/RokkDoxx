// Host side of the GPU search: pick a device, build the kernels, and drive
// tiles through them.
//
// The kernel text is bedrock_core.h concatenated with search_tile.cl and
// compiled at runtime, so the GPU runs the exact same generation math as the
// CPU. Per tile: zero the match counter; for each kChunk x kChunk piece run
// fill_plane (draw every block of the piece plus the pattern's halo once, one
// bit each; Bedrock Edition: fill_plane_be + remap_plane_be) then match_plane
// (32 candidates per work-item, tested with word ops); then read back the
// match count and that many matches. Drawing each
// block once is what makes all 8 orientations nearly as cheap as one.
// OpenclWorker::preferred_tile_side() asks the scheduler for large
// (16384-block) tiles to keep the fixed per-dispatch cost small.
//
// match_plane is rebuilt per pattern with the first cells' offsets baked in as
// literals (specialized_source); if that build fails the generic kernel is used.
//
// begin_tile()/end_tile() ping-pong two buffer/queue slots so one tile's
// dispatch can overlap another's read-back; run_tile() is the fully
// synchronous single-slot form.
#include "opencl_worker.hpp"

#include <algorithm>
#include <atomic>
#include <stdexcept>
#include <tuple>
#include <utility>

#define CL_HPP_ENABLE_EXCEPTIONS
#define CL_HPP_TARGET_OPENCL_VERSION 120
#define CL_HPP_MINIMUM_OPENCL_VERSION 120
#define CL_TARGET_OPENCL_VERSION 120  // silences the <CL/cl.h> "defaulting to 300" pragma
#define NOMINMAX                      // in case an SDK header pulls in <windows.h>
#define CL_SILENCE_DEPRECATION        // macOS: Apple deprecated OpenCL (it still works, at 1.2)
// Apple ships only the C headers; the C++ bindings are Khronos's (brew install
// opencl-clhpp-headers), which pull in <OpenCL/opencl.h> on macOS by themselves.
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

// Work-group shape for the plane kernels: 32x8 (measured fastest on a 7900 XTX),
// halved until it fits `limit` -- the most work-items either kernel can launch
// with. Most GPUs allow 256+, but Apple's OpenCL can cap a kernel lower, and its
// CPU device allows 1; an oversized group is a hard CL_INVALID_WORK_GROUP_SIZE.
std::pair<std::size_t, std::size_t> fit_local(std::size_t limit) {
    std::size_t w = 32, h = 8;
    while (w * h > limit && w * h > 1) {
        if (h > 1) h /= 2;
        else w /= 2;
    }
    return {w, h};
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
            // Kind first: if a later query throws, "auto" can still pick by it.
            try {
                d.is_cpu = (devs[i].getInfo<CL_DEVICE_TYPE>() & CL_DEVICE_TYPE_CPU) != 0;
                d.integrated = !d.is_cpu && devs[i].getInfo<CL_DEVICE_HOST_UNIFIED_MEMORY>() != CL_FALSE;
                d.compute_units = static_cast<int>(devs[i].getInfo<CL_DEVICE_MAX_COMPUTE_UNITS>());
                d.cl_version = trimmed(devs[i].getInfo<CL_DEVICE_VERSION>());
                d.driver_version = trimmed(devs[i].getInfo<CL_DRIVER_VERSION>());
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
    cl::Kernel k_dump;

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

    // fill_plane + match_plane. A tile is processed in kChunk x kChunk pieces so
    // the per-slot plane buffer has a fixed size (~34 MB at halo 31, the most a
    // 32x32 pattern can reach) whatever tile side the scheduler picked.
    static constexpr int kChunk = 16384;
    std::size_t local_w = 32, local_h = 8;  // see fit_local; set by pick_local()
    std::size_t be_local_w = 32, be_local_h = 8;  // the same for the Bedrock Edition kernels
    cl::Kernel k_fill, k_match;
    cl::Kernel k_fill_be, k_remap_be;  // Bedrock Edition's fill: chunks into buf_raw, then remap
    cl::Program specialized_program;  // the current pattern's match_plane (one at a time)
    std::string specialized_src;      // its source, so an unchanged pattern isn't rebuilt
    cl::Buffer buf_plane[kSlots];
    cl::Buffer buf_raw[kSlots];  // Bedrock Edition only
    int halo_w = 0, halo_h = 0;

    // Call whenever k_fill / k_match change: the specialized match_plane can
    // have a smaller launch limit than the generic one.
    void pick_local() {
        std::size_t limit = 256;  // unqueryable: assume the old default
        try {
            limit = std::min(k_fill.getWorkGroupInfo<CL_KERNEL_WORK_GROUP_SIZE>(device),
                             k_match.getWorkGroupInfo<CL_KERNEL_WORK_GROUP_SIZE>(device));
        } catch (const cl::Error&) {
        }
        std::tie(local_w, local_h) = fit_local(limit);
    }
};

OpenclWorker::OpenclWorker(int device_index) : impl_(std::make_unique<Impl>()) {
    auto devs = flat_devices();
    if (devs.empty()) throw std::runtime_error("no OpenCL devices found");
    if (device_index < 0 || device_index >= static_cast<int>(devs.size()))
        throw std::runtime_error("OpenCL device index out of range");

    impl_->device = devs[static_cast<std::size_t>(device_index)];
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
    impl_->k_dump = cl::Kernel(impl_->program, "dump_plane");
    impl_->k_fill = cl::Kernel(impl_->program, "fill_plane");
    impl_->k_match = cl::Kernel(impl_->program, "match_plane");
    impl_->k_fill_be = cl::Kernel(impl_->program, "fill_plane_be");
    impl_->k_remap_be = cl::Kernel(impl_->program, "remap_plane_be");
    impl_->pick_local();
    std::size_t be_limit = 256;
    try {
        be_limit = std::min(impl_->k_fill_be.getWorkGroupInfo<CL_KERNEL_WORK_GROUP_SIZE>(impl_->device),
                            impl_->k_remap_be.getWorkGroupInfo<CL_KERNEL_WORK_GROUP_SIZE>(impl_->device));
    } catch (const cl::Error&) {
    }
    std::tie(impl_->be_local_w, impl_->be_local_h) = fit_local(be_limit);
}

// = default: OpenCL keeps objects alive until their queued commands finish.
OpenclWorker::~OpenclWorker() = default;

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
    // Bedrock Edition's plane starts on a chunk-pair boundary: up to 31 / 15 more
    // halo on the low side, and buf_raw holds whole chunks plus a row.
    const bool be = cfg.edition == Edition::bedrock;
    const std::size_t pad = be ? 32 : 0;
    const std::size_t words = (Impl::kChunk + 2 * halo_w + 31 + pad) / 32 + 1;
    const std::size_t rows = Impl::kChunk + 2 * halo_h + pad;
    for (int i = 0; i < Impl::kSlots; ++i) {
        impl_->buf_plane[i] = cl::Buffer(impl_->ctx, CL_MEM_READ_WRITE, words * rows * sizeof(cl_uint));
        impl_->buf_raw[i] = be ? cl::Buffer(impl_->ctx, CL_MEM_READ_WRITE, words * rows * sizeof(cl_uint))
                               : cl::Buffer();
    }

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
    impl_->pick_local();
}

// Zero the counter and launch the kernels into `slot`, non-blocking. Setting
// kernel args and enqueueing happen back-to-back on this thread, so reusing
// one cl::Kernel object across slots and pieces is safe: OpenCL captures a
// kernel's argument values at enqueue time, not at execution time, so a later
// setArg() (for the other slot) can never retroactively change a command
// that's already been enqueued.
void OpenclWorker::enqueue_search(int slot, const Tile& tile) {
    impl_->queue[slot].enqueueFillBuffer(impl_->buf_count[slot], cl_uint{0}, 0, sizeof(cl_uint));

    const int hw = impl_->halo_w, hh = impl_->halo_h;
    const std::size_t lw = impl_->local_w, lh = impl_->local_h;
    const cl::NDRange local(lw, lh);
    for (int cz = 0; cz < tile.h; cz += Impl::kChunk)
        for (int cx = 0; cx < tile.w; cx += Impl::kChunk) {
            const std::int64_t x0 = tile.x0 + cx, z0 = tile.z0 + cz;
            const int w = std::min(Impl::kChunk, tile.w - cx);
            const int h = std::min(Impl::kChunk, tile.h - cz);
            const bool be = impl_->cfg.edition == Edition::bedrock;
            // The plane's corner, and so the halo match_plane is told about on the
            // low side: Bedrock Edition's is rounded down to a chunk pair.
            const std::int64_t px = be ? (x0 - hw) & ~std::int64_t{31} : x0 - hw;
            const std::int64_t pz = be ? (z0 - hh) & ~std::int64_t{15} : z0 - hh;
            const int lo_w = static_cast<int>(x0 - px), lo_h = static_cast<int>(z0 - pz);
            const int words_w = (w + lo_w + hw + 31) / 32 + 1;
            const int rows = h + lo_h + hh;

            cl_uint a = 0;
            if (be) {
                const int raw_rows = (rows + 1 + 15) / 16 * 16;
                // Within +/-2^24 the remap is a copy: fill the plane directly.
                const int kNear = 1 << 24;
                const bool near = px >= -kNear && px + 32 * words_w <= kNear && pz >= -kNear &&
                                  pz + raw_rows <= kNear;
                cl::Kernel& f = impl_->k_fill_be;
                f.setArg(a++, static_cast<cl_int>(impl_->cfg.plane_y));
                f.setArg(a++, static_cast<cl_int>(px));
                f.setArg(a++, static_cast<cl_int>(pz));
                f.setArg(a++, static_cast<cl_int>(words_w));
                f.setArg(a++, static_cast<cl_int>(raw_rows));
                f.setArg(a++, near ? impl_->buf_plane[slot] : impl_->buf_raw[slot]);
                const std::size_t bw = impl_->be_local_w, bh = impl_->be_local_h;
                impl_->queue[slot].enqueueNDRangeKernel(
                    f, cl::NullRange, cl::NDRange(round_up(words_w, bw), round_up(raw_rows / 16, bh)),
                    cl::NDRange(bw, bh));
                if (!near) {
                    cl::Kernel& r = impl_->k_remap_be;
                    a = 0;
                    r.setArg(a++, static_cast<cl_int>(px));
                    r.setArg(a++, static_cast<cl_int>(pz));
                    r.setArg(a++, static_cast<cl_int>(words_w));
                    r.setArg(a++, static_cast<cl_int>(rows));
                    r.setArg(a++, impl_->buf_raw[slot]);
                    r.setArg(a++, impl_->buf_plane[slot]);
                    impl_->queue[slot].enqueueNDRangeKernel(
                        r, cl::NullRange, cl::NDRange(round_up(words_w, bw), round_up(rows, bh)),
                        cl::NDRange(bw, bh));
                }
            } else {
                cl::Kernel& f = impl_->k_fill;
                f.setArg(a++, static_cast<cl_ulong>(impl_->cfg.derived_lo));
                f.setArg(a++, static_cast<cl_ulong>(impl_->cfg.derived_hi));
                f.setArg(a++, static_cast<cl_int>(impl_->cfg.plane_y));
                f.setArg(a++, static_cast<cl_uint>(impl_->cfg.threshold));
                f.setArg(a++, static_cast<cl_int>(px));
                f.setArg(a++, static_cast<cl_int>(pz));
                f.setArg(a++, static_cast<cl_int>(words_w));
                f.setArg(a++, static_cast<cl_int>(rows));
                f.setArg(a++, impl_->buf_plane[slot]);
                impl_->queue[slot].enqueueNDRangeKernel(
                    f, cl::NullRange,
                    cl::NDRange(round_up(words_w, lw), round_up(rows, lh)), local);
            }

            cl::Kernel& m = impl_->k_match;
            a = 0;
            m.setArg(a++, static_cast<cl_int>(x0));
            m.setArg(a++, static_cast<cl_int>(z0));
            m.setArg(a++, static_cast<cl_int>(w));
            m.setArg(a++, static_cast<cl_int>(h));
            m.setArg(a++, static_cast<cl_int>(lo_w));
            m.setArg(a++, static_cast<cl_int>(lo_h));
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
                cl::NDRange(round_up((w + 31) / 32, lw), round_up(h, lh)), local);
        }
}

// Blocking read-back of whatever `slot`'s kernel found. The blocking
// enqueueReadBuffer call is enough by itself to wait for that slot's queue to
// finish (in-order queue: the read is ordered after the fill + kernels), so no
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
    // blocking read in read_results() waits for this slot's fill + kernels too
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
