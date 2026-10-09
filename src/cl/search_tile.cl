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

// ---------------------------------------------------------------------------
// Bedrock Edition planes, for the same match_plane. That floor is made a chunk
// at a time, so fill_plane_be gives each work-item two chunks side by side --
// 32 x 16 blocks, whole plane words (the host aligns px0 to 32, pz0 to 16) --
// and writes `raw`: every column at its own coordinates. remap_plane_be then
// writes `plane`: past +/-2^24 the game shows the column at the float-rounded
// position (rk_be_coord), one block away at most. Nearer in it would be a copy,
// so there the host has fill_plane_be write `plane` directly.

__kernel void fill_plane_be(const int plane_y, const int px0, const int pz0, const int words_w,
                            const int rows, __global uint *raw) {
    const int wx = get_global_id(0);
    const int cj = get_global_id(1);
    if (wx >= words_w || 16 * cj >= rows) return;
    uint lo[16], hi[16];
    rk_be_chunk_rows((px0 >> 4) + 2 * wx, (pz0 >> 4) + cj, plane_y + 63, lo);
    rk_be_chunk_rows((px0 >> 4) + 2 * wx + 1, (pz0 >> 4) + cj, plane_y + 63, hi);
    for (int lz = 0; lz < 16 && 16 * cj + lz < rows; ++lz)
        raw[(size_t)(16 * cj + lz) * words_w + wx] = lo[lz] | (hi[lz] << 16);
}

// `raw` has one more row than `plane`: a far row can show the row below it.
__kernel void remap_plane_be(const int px0, const int pz0, const int words_w, const int rows,
                             __global const uint *raw, __global uint *plane) {
    const int wx = get_global_id(0);
    const int r = get_global_id(1);
    if (wx >= words_w || r >= rows) return;
    __global const uint *src = raw + (size_t)(rk_be_coord(pz0 + r) - pz0) * words_w;
    const int x0 = px0 + 32 * wx;
    uint w = src[wx];
    // Bit b is x = b mod 4 (px0 is a multiple of 32). Below 2^25 (the service's
    // limit) a float steps by 2 here, ties to the multiple of 4: x = 1 mod 4 shows
    // x - 1, x = 3 mod 4 shows x + 1 (bit 31 takes the next word's bit 0).
    if (x0 >= (1 << 24) || x0 < -(1 << 24)) {
        const uint next = wx + 1 < words_w ? src[wx + 1] : 0u;
        w = (w & 0x55555555u) | ((w & 0x11111111u) << 1) | ((w & 0x11111110u) >> 1) | ((next & 1u) << 31);
    }
    plane[(size_t)r * words_w + wx] = w;
}
