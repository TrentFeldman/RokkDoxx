/*
 * bedrock_core.h -- Minecraft 26.2 Overworld bedrock-floor generation math
 * (Java Edition; Bedrock Edition's is further down), written in the common
 * subset of C++20 and OpenCL C so the CPU library and the GPU kernel share
 * one implementation.
 *
 * Host-only pieces (seeding and stepping the RNG for the fork chain in
 * bedrock.cpp, and the ceil() threshold helper) live behind
 * `#if !defined(__OPENCL_VERSION__)`.
 *
 * Java's math is integer-only. The one place vanilla uses a float --
 * `(double)nextFloat() < prob` -- is replaced by an exact integer compare
 * `bits24 < threshold`, where the host precomputes
 *     threshold = ceil(prob_double * 2^24).
 * This is exact because `nextFloat()` widened to double is exactly bits/2^24
 * and `prob_double * 2^24` is an exact power-of-two scaling, so
 *     (double)nextFloat() < prob   <=>   bits < prob_double*2^24   <=>   bits < ceil(...).
 * => identical results on every OpenCL device, no fp64, no rounding modes.
 */
#ifndef ROKKDOXX_BEDROCK_CORE_H
#define ROKKDOXX_BEDROCK_CORE_H

#if defined(__OPENCL_VERSION__)
typedef ulong rk_u64;
typedef long rk_i64;
typedef uint rk_u32;
typedef int rk_i32;
#define RK_INLINE inline
#else
#include <stdint.h>
typedef uint64_t rk_u64;
typedef int64_t rk_i64;
typedef uint32_t rk_u32;
typedef int32_t rk_i32;
#define RK_INLINE static inline
#endif

/* frac(sqrt 2) * 2^64  and  golden ratio * 2^64 -- seed-upgrade mixers and the
 * all-zero-state fallback. */
#define RK_SILVER_RATIO_64 ((rk_u64)0x6A09E667F3BCC909)
#define RK_GOLDEN_RATIO_64 ((rk_u64)0x9E3779B97F4A7C15)

RK_INLINE rk_u64 rk_rotl64(rk_u64 x, int k) {
#if defined(__OPENCL_VERSION__)
    return rotate(x, (rk_u64)k);
#else
    return (x << k) | (x >> (64 - k));
#endif
}

/* Java RandomSupport.mixStafford13 (SplitMix64 finalizer). */
RK_INLINE rk_u64 rk_mix_stafford13(rk_u64 z) {
    z = (z ^ (z >> 30)) * (rk_u64)0xBF58476D1CE4E5B9;
    z = (z ^ (z >> 27)) * (rk_u64)0x94D049BB133111EB;
    return z ^ (z >> 31);
}

/* One Xoroshiro128++ output from raw state (lo, hi), with the all-zero guard.
 * We never need to advance the state for bedrock: each block gets a fresh raw
 * state and a single draw. */
RK_INLINE rk_u64 rk_xoro_first(rk_u64 lo, rk_u64 hi) {
    if ((lo | hi) == (rk_u64)0) {
        lo = RK_GOLDEN_RATIO_64;
        hi = RK_SILVER_RATIO_64;
    }
    return rk_rotl64(lo + hi, 17) + lo;
}

#if !defined(__OPENCL_VERSION__)
/* Seeding and the full stateful step, for bedrock.cpp's fork chain and the
 * Java-vector tests. Not compiled for OpenCL (private-pointer params). */

/* Java: new Xoroshiro128PlusPlusRandom(long seed). Never all-zero, so no fallback. */
RK_INLINE void rk_xoro_seed(rk_u64 seed, rk_u64 *lo, rk_u64 *hi) {
    rk_u64 l = seed ^ RK_SILVER_RATIO_64;
    *lo = rk_mix_stafford13(l);
    *hi = rk_mix_stafford13(l + RK_GOLDEN_RATIO_64);
}

RK_INLINE rk_u64 rk_xoro_next(rk_u64 *lo, rk_u64 *hi) {
    rk_u64 l = *lo;
    rk_u64 m = *hi;
    rk_u64 n = rk_rotl64(l + m, 17) + l;
    m ^= l;
    *lo = rk_rotl64(l, 49) ^ m ^ (m << 21);
    *hi = rk_rotl64(m, 28);
    return n;
}
#endif

/* Java Mth.getSeed(x, y, z) -- the block-position hash. Returns the raw 64-bit
 * pattern of `l >> 16` (arithmetic shift). */
RK_INLINE rk_u64 rk_block_pos_seed(rk_i32 x, rk_i32 y, rk_i32 z) {
    rk_i32 xm = (rk_i32)((rk_u32)x * (rk_u32)3129871); /* 32-bit wrap */
    rk_u64 xw = (rk_u64)(rk_i64)xm;                    /* sign-extend to 64 */
    rk_u64 zw = ((rk_u64)(rk_i64)z) * (rk_u64)116129781; /* == (long)z * 116129781L, low 64 bits */
    rk_u64 yw = (rk_u64)(rk_i64)y;

    rk_u64 u = xw ^ zw ^ yw;
    u = u * u * (rk_u64)42317861 + u * (rk_u64)11;
    return (rk_u64)((rk_i64)u >> 16); /* signed >> is arithmetic in C++20 and OpenCL C */
}

/* Top 24 bits of the positional RNG draw at (x, y, z). */
RK_INLINE rk_u32 rk_bits24_at(rk_u64 derived_lo, rk_u64 derived_hi, rk_i32 x, rk_i32 y, rk_i32 z) {
    rk_u64 h = rk_block_pos_seed(x, y, z);
    rk_u64 n = rk_xoro_first(h ^ derived_lo, derived_hi);
    return (rk_u32)(n >> 40);
}

/* --- Bedrock Edition ---------------------------------------------------------
 * A different generator, the same in every world (no seed), checked block for
 * block against Bedrock Dedicated Server 1.26.52. y = -64 and -63 are solid;
 * above that each column has a top at -63 + r, r = 0..3. For chunk (cx, cz),
 * r comes from MT19937 seeded with cx * 341872712 + cz * 132899541 (32-bit
 * wrap): output k = lx * 16 + lz, mod 4. */

#define RK_MT_INIT(v, i) ((rk_u32)1812433253u * ((v) ^ ((v) >> 30)) + (rk_u32)(i))

/* MT19937's twist of state words (a, b) with the word 397 ahead, c. */
RK_INLINE rk_u32 rk_mt_twist(rk_u32 a, rk_u32 b, rk_u32 c) {
    const rk_u32 y = (a & 0x80000000u) | (b & 0x7fffffffu);
    return c ^ (y >> 1) ^ ((y & 1u) ? 0x9908b0dfu : 0u);
}

/* The low two bits of MT19937's tempered output. Tempering is linear over
 * GF(2), so on the GPU each bit is the parity of a mask of the untempered word:
 * two popcounts. A CPU build may lack popcnt (x86-64 baseline), so it tempers. */
RK_INLINE rk_u32 rk_mt_low2(rk_u32 y) {
#if defined(__OPENCL_VERSION__)
    return (popcount(y & 0x20444009u) & 1u) | ((popcount(y & 0x40880002u) & 1u) << 1);
#else
    y ^= y >> 11;
    y ^= (y << 7) & 0x9d2c5680u;
    y ^= (y << 15) & 0xefc60000u;
    return (y ^ (y >> 18)) & 3u;
#endif
}

/* rows[lz] bit lx = bedrock at block (16cx + lx, y, 16cz + lz), need = y + 63.
 * The 256 outputs use seeded state words 0..256 and 397..623, and (from output
 * 227) the twisted words 0..28; cursors walk them instead of a 624-word state,
 * which a GPU would spill to memory. */
RK_INLINE void rk_be_chunk_rows(rk_i32 cx, rk_i32 cz, rk_i32 need, rk_u32 *rows) {
    const rk_u32 seed = (rk_u32)cx * 341872712u + (rk_u32)cz * 132899541u;
    rk_u32 a = seed; /* word k */
    rk_u32 b = seed; /* word k + 397 */
    for (int i = 1; i <= 397; ++i) b = RK_MT_INIT(b, i);
    rk_u32 c = seed, d = b; /* words j and j + 397, j = k - 227 */
    for (int lz = 0; lz < 16; ++lz) rows[lz] = 0;
    for (int lx = 0; lx < 16; ++lx)
        for (int lz = 0; lz < 16; ++lz) {
            const int k = lx * 16 + lz;
            const rk_u32 a1 = RK_MT_INIT(a, k + 1);
            rk_u32 ahead;
            if (k < 227) {
                ahead = b;
                b = RK_MT_INIT(b, k + 398);
            } else {
                const int j = k - 227;
                const rk_u32 c1 = RK_MT_INIT(c, j + 1);
                ahead = rk_mt_twist(c, c1, d);
                c = c1;
                d = RK_MT_INIT(d, j + 398);
            }
            const rk_i32 r = (rk_i32)rk_mt_low2(rk_mt_twist(a, a1, ahead));
            rows[lz] |= (rk_u32)(r >= need) << lx;
            a = a1;
        }
}

/* The game reads the column at the block position rounded through a float, so
 * past +/-2^24 an odd x or z shows its neighbour's column. */
RK_INLINE rk_i32 rk_be_coord(rk_i32 v) { return (rk_i32)(float)v; }

#if !defined(__OPENCL_VERSION__)
#include <math.h>
/* Integer cutoff for a given bedrock-floor y. `bits < threshold` == bedrock.
 * y <= -64 -> 2^24 (always); y >= -59 -> 0 (never). */
RK_INLINE rk_u32 rk_floor_threshold(int y) {
    if (y <= -64) return (rk_u32)16777216;
    if (y >= -59) return (rk_u32)0;
    double prob = 1.0 - (double)(y + 64) / 5.0;
    return (rk_u32)ceil(prob * 16777216.0);
}
#endif

#endif /* ROKKDOXX_BEDROCK_CORE_H */
