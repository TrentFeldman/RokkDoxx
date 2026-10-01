// RokkDoxx OpenCL search kernel. The generation math (bedrock_core.h) is
// PREPENDED to this file by the host loader (opencl_worker.cpp) -- do not
// #include it here.
//
// The host (build_search_plan) has already done the D4 work: `var_off` holds,
// per orientation-variant, every pattern cell's world offset *relative to a
// rare anchor cell*. Cell 0 is that anchor, at offset (0,0) for every variant
// -- so one bedrock test at the candidate rejects all orientations at once. A
// match reports the anchor's world position. The float compare vanilla does is
// a host-precomputed `bits < threshold` (see bedrock_core.h) so results are
// bit-identical to the CPU on any device.
//
// The main path is fill_plane + match_plane (bottom of this file). search_tile
// -- one work-item per candidate, recomputing every cell it needs -- is the
// fallback for patterns whose halo is too big for the plane buffer.

__kernel void search_tile(const ulong derived_lo, const ulong derived_hi, const int plane_y,
                          const uint threshold, const int origin_x, const int origin_z,
                          const int tile_w, const int tile_h, const int n_cells,
                          const int n_variants, __constant const int2 *var_off,
                          __constant const uchar *want, __constant const uchar *var_mask,
                          const uint match_cap, __global volatile uint *match_count,
                          __global int2 *match_xz, __global uchar *match_orient) {
    const int gx = get_global_id(0);
    const int gz = get_global_id(1);
    if (gx >= tile_w || gz >= tile_h) return;
    const int x = origin_x + gx;
    const int z = origin_z + gz;

    // Shared anchor: cell 0 sits at (x, z) for every variant.
    const uint abits = rk_bits24_at(derived_lo, derived_hi, x, plane_y, z);
    if (((abits < threshold) ? 1 : 0) != (int)want[0]) return;

    uchar mask = 0;
    for (int v = 0; v < n_variants; ++v) {
        __constant const int2 *off = var_off + (size_t)v * n_cells;
        bool ok = true;
        for (int c = 1; c < n_cells; ++c) {
            const uint bits =
                rk_bits24_at(derived_lo, derived_hi, x + off[c].x, plane_y, z + off[c].y);
            if (((bits < threshold) ? 1 : 0) != (int)want[c]) {
                ok = false;
                break;
            }
        }
        if (ok) mask |= var_mask[v];
    }

    if (mask) {
        const uint idx = atomic_inc(match_count);
        if (idx < match_cap) {
            match_xz[idx] = (int2)(x, z);
            match_orient[idx] = mask;
        }
    }
}

// Bit-exactness check helper: dump `bits < threshold` for a rectangle.
__kernel void dump_plane(const ulong derived_lo, const ulong derived_hi, const int plane_y,
                         const uint threshold, const int origin_x, const int origin_z, const int w,
                         const int h, __global uchar *out) {
    const int gx = get_global_id(0);
    const int gz = get_global_id(1);
    if (gx >= w || gz >= h) return;
    const uint bits = rk_bits24_at(derived_lo, derived_hi, origin_x + gx, plane_y, origin_z + gz);
    out[(size_t)gz * w + gx] = (bits < threshold) ? 1 : 0;
}

// ---------------------------------------------------------------------------
// Bit-plane search: fill_plane draws every world cell of a chunk (plus the
// pattern's halo) exactly once into a 1-bit-per-block plane; match_plane then
// tests 32 neighbouring candidates at a time with word ops. RNG cost is ~1
// draw per candidate no matter how many orientation variants there are.
//
// Plane layout: row r, bit k  <->  world (px0 + k, pz0 + r); bit k lives in
// word k/32 at bit k%32. `words_w` includes one spare word so the unaligned
// two-word read in plane_bits never runs off the row.

__kernel void fill_plane(const ulong derived_lo, const ulong derived_hi, const int plane_y,
                         const uint threshold, const int px0, const int pz0, const int words_w,
                         const int rows, __global uint *plane) {
    const int wx = get_global_id(0);
    const int wz = get_global_id(1);
    if (wx >= words_w || wz >= rows) return;
    const int x0 = px0 + 32 * wx;
    const int z = pz0 + wz;
    uint w = 0;
    for (int b = 0; b < 32; ++b)
        w |= ((rk_bits24_at(derived_lo, derived_hi, x0 + b, plane_y, z) < threshold) ? 1u : 0u) << b;
    plane[(size_t)wz * words_w + wx] = w;
}

// 32 plane bits starting at bit `start` of `row`.
inline uint plane_bits(__global const uint *plane, const int words_w, const int row, const int start) {
    __global const uint *p = plane + (size_t)row * words_w + (start >> 5);
    const int s = start & 31;
    return s ? (p[0] >> s) | (p[1] << (32 - s)) : p[0];
}

// One work-item = candidates (origin_x + 32*wx + b, origin_z + gz), b = 0..31.
// The plane was filled from (origin_x - halo_w, origin_z - halo_h).
__kernel void match_plane(const int origin_x, const int origin_z, const int tile_w,
                          const int tile_h, const int halo_w, const int halo_h,
                          const int words_w, __global const uint *plane, const int n_cells,
                          const int n_variants, __constant const int2 *var_off,
                          __constant const uchar *want, __constant const uchar *var_mask,
                          const uint match_cap, __global volatile uint *match_count,
                          __global int2 *match_xz, __global uchar *match_orient) {
    const int wx = get_global_id(0);
    const int gz = get_global_id(1);
    if (wx >= ((tile_w + 31) >> 5) || gz >= tile_h) return;
    const int col = 32 * wx + halo_w;  // plane bit of candidate b = 0
    const int row = gz + halo_h;

    // Shared anchor: cell 0 sits on the candidate itself for every variant.
    const uint a = plane_bits(plane, words_w, row, col);
    uint alive = want[0] ? a : ~a;
    const int rem = tile_w - 32 * wx;
    if (rem < 32) alive &= (1u << rem) - 1u;
    if (!alive) return;

    uint mv[8];
    uint any = 0;
    // The host swaps everything between these markers for straight-line code with the
    // pattern's first cells baked in (opencl_worker.cpp, specialized_source). It must
    // set mv[] and any exactly as this loop does -- this version is also the fallback.
    // rk:variants-begin
    for (int v = 0; v < n_variants; ++v) {
        __constant const int2 *off = var_off + (size_t)v * n_cells;
        uint m = alive;
        for (int c = 1; c < n_cells && m; ++c) {
            const uint w = plane_bits(plane, words_w, row + off[c].y, col + off[c].x);
            m &= want[c] ? w : ~w;
        }
        mv[v] = m;
        any |= m;
    }
    // rk:variants-end

    while (any) {
        const uint low = any & (0u - any);
        any ^= low;
        uchar mask = 0;
        for (int v = 0; v < n_variants; ++v)
            if (mv[v] & low) mask |= var_mask[v];
        const uint idx = atomic_inc(match_count);
        if (idx < match_cap) {
            match_xz[idx] = (int2)(origin_x + 32 * wx + (31 - (int)clz(low)), origin_z + gz);
            match_orient[idx] = mask;
        }
    }
}
