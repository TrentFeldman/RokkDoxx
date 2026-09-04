// RokkDoxx OpenCL search kernel. The generation math (bedrock_core.h) is
// PREPENDED to this file by the host loader (opencl_worker.cpp) -- do not
// #include it here.
//
// One work-item per candidate origin. The host (build_search_plan) has already
// done the D4 work: `var_off` holds, per orientation-variant, every pattern
// cell's world offset *relative to a rare anchor cell*. Cell 0 is that anchor,
// at offset (0,0) for every variant -- so one bedrock test at the candidate
// rejects all orientations at once. A match reports the anchor's world position.
// The float compare vanilla does is a host-precomputed `bits < threshold` (see
// bedrock_core.h) so results are bit-identical to the CPU on any device.

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
