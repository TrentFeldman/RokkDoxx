// RokkDoxx OpenCL search kernel. The generation math (bedrock_core.h) is
// PREPENDED to this file by the host loader (opencl_worker.cpp) -- do not
// #include it here.
//
// Pure integer work: one work-item per candidate origin, `rk_bits24_at` per
// pattern cell it has to check, early-out on the first mismatch. The float
// compare vanilla does is a host-precomputed `bits < threshold` (see
// bedrock_core.h) so results are bit-identical to the CPU on any device.

// D4: the 8 orientations. (du, dv) = (m.x*i + m.y*j, m.z*i + m.w*j).
// Must match rokkdoxx::svc::kOrientations.
__constant int4 RK_D4[8] = {
    (int4)(1, 0, 0, 1),   (int4)(0, -1, 1, 0),  (int4)(-1, 0, 0, -1), (int4)(0, 1, -1, 0),
    (int4)(-1, 0, 0, 1),  (int4)(0, 1, 1, 0),   (int4)(1, 0, 0, -1),  (int4)(0, -1, -1, 0),
};

__kernel void search_tile(const ulong derived_lo, const ulong derived_hi, const int plane_y,
                          const uint threshold, const int origin_x, const int origin_z,
                          const int tile_w, const int tile_h, const int n_cells,
                          const int n_orient, __constant const int2 *cell_rel,
                          __constant const uchar *cell_want, const uint match_cap,
                          __global volatile uint *match_count, __global int2 *match_xz,
                          __global uchar *match_orient) {
    const int gx = get_global_id(0);
    const int gz = get_global_id(1);
    if (gx >= tile_w || gz >= tile_h) return;
    const int x = origin_x + gx;
    const int z = origin_z + gz;

    uchar mask = 0;
    for (int g = 0; g < n_orient; ++g) {
        const int4 m = RK_D4[g];
        bool ok = true;
        for (int c = 0; c < n_cells; ++c) {
            const int2 r = cell_rel[c];
            const int du = m.x * r.x + m.y * r.y;
            const int dv = m.z * r.x + m.w * r.y;
            const uint bits = rk_bits24_at(derived_lo, derived_hi, x + du, plane_y, z + dv);
            const int bed = (bits < threshold) ? 1 : 0;
            if (bed != (int)cell_want[c]) {
                ok = false;
                break;
            }
        }
        if (ok) mask |= (uchar)(1u << g);
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
