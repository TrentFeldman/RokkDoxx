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

// Cached variant of search_tile: a work-group cooperatively fills a __local
// cache of `bits24 < threshold` results covering its tile chunk plus a halo
// margin (the pattern's world-space bounding box, computed host-side from
// var_off), then the per-cell/per-variant loop reads that cache instead of
// recomputing rk_bits24_at for every cell -- neighbouring candidates' cells
// overlap heavily in world space, so this removes real redundant RNG work.
// The fill is gated behind a cheap group-wide "did anyone survive their own
// anchor test" check: the anchor test itself is NOT shared between
// neighbours (each work-item's own anchor position is unique to it), so a
// group where nobody survives their anchor bails before ever touching the
// (comparatively expensive) fill.
//
// Host-side selection: OpenclWorker::configure() only dispatches this kernel
// when the halo fits a conservative fraction of the device's local memory
// (queried, not assumed) and the enqueued global size is padded up to a
// multiple of the work-group size -- see opencl_worker.cpp. Falls back to
// plain search_tile above otherwise.
//
// IMPORTANT: every work-item in a work-group must reach every barrier() in
// this kernel together, including padding work-items past tile_w/tile_h
// (from that size padding) and work-items whose own anchor test already
// failed. There is deliberately no `return` before the final barrier --
// bounds/anchor checks are folded into booleans used *after* it. Adding an
// early return before a barrier here is a correctness bug (undefined
// behaviour on real hardware, not just a wrong answer), not a style choice.
__kernel void search_tile_cached(const ulong derived_lo, const ulong derived_hi,
                                 const int plane_y, const uint threshold, const int origin_x,
                                 const int origin_z, const int tile_w, const int tile_h,
                                 const int n_cells, const int n_variants,
                                 __constant const int2 *var_off, __constant const uchar *want,
                                 __constant const uchar *var_mask, const uint match_cap,
                                 __global volatile uint *match_count, __global int2 *match_xz,
                                 __global uchar *match_orient, const int halo_w, const int halo_h,
                                 __local uchar *cache) {
    const int gx = get_global_id(0);
    const int gz = get_global_id(1);
    const int lx = get_local_id(0);
    const int lz = get_local_id(1);
    const int gw = get_local_size(0);
    const int gh = get_local_size(1);
    const int lid = lz * gw + lx;
    const int x = origin_x + gx;
    const int z = origin_z + gz;

    const bool in_tile = gx < tile_w && gz < tile_h;

    // Shared anchor, direct: unlike the halo cells below, this is *not*
    // redundant across neighbours (every work-item's own anchor world
    // position is unique to it), so there's nothing to gain by caching it.
    const uint abits = rk_bits24_at(derived_lo, derived_hi, x, plane_y, z);
    const bool anchor_ok = in_tile && (((abits < threshold) ? 1 : 0) == (int)want[0]);

    // Cheap common-case bailout: if nobody in the group survived their own
    // anchor test, skip the halo fill -- a whole-group uniform return here
    // is safe (every work-item takes the same branch, so none of them are
    // left waiting on a barrier a returned work-item never reaches).
    __local volatile uint any_survivor;
    if (lid == 0) any_survivor = 0;
    barrier(CLK_LOCAL_MEM_FENCE);
    if (anchor_ok) atomic_or(&any_survivor, 1u);
    barrier(CLK_LOCAL_MEM_FENCE);
    if (any_survivor == 0) return;

    // Cooperative halo fill: every work-item participates regardless of its
    // own anchor result or tile-bounds membership, so the barrier below is
    // reached uniformly by the whole group.
    const int group_x0 = origin_x + get_group_id(0) * gw;
    const int group_z0 = origin_z + get_group_id(1) * gh;
    const int cache_w = gw + 2 * halo_w;
    const int cache_h = gh + 2 * halo_h;
    const int cache_n = cache_w * cache_h;
    const int group_n = gw * gh;
    for (int idx = lid; idx < cache_n; idx += group_n) {
        const int cx = idx % cache_w;
        const int cz = idx / cache_w;
        const uint bits = rk_bits24_at(derived_lo, derived_hi, group_x0 + cx - halo_w, plane_y,
                                        group_z0 + cz - halo_h);
        cache[idx] = (bits < threshold) ? (uchar)1 : (uchar)0;
    }
    barrier(CLK_LOCAL_MEM_FENCE);

    if (!anchor_ok) return;  // every work-item already hit the barrier above -- safe now

    uchar mask = 0;
    for (int v = 0; v < n_variants; ++v) {
        __constant const int2 *off = var_off + (size_t)v * n_cells;
        bool ok = true;
        for (int c = 1; c < n_cells; ++c) {
            const int cx = lx + halo_w + off[c].x;
            const int cz = lz + halo_h + off[c].y;
            if (cache[cz * cache_w + cx] != want[c]) {
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
