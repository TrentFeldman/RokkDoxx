/*
 * bedrock_core.h -- Minecraft 26.2 Overworld bedrock-floor generation math,
 * written in the common subset of C99 / C++ / OpenCL C so the CPU library and
 * the GPU kernel share one implementation.
 *
 * Host-only pieces (the MD5 + fork chain that turns a world seed into
 * derived_lo/derived_hi, and the ceil() threshold helper) live behind
 * `#if !defined(__OPENCL_VERSION__)`.
 *
 * Everything here is integer-only. The one place vanilla uses a float --
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
/* Full stateful step -- used by the C++ RNG wrapper and its Java-vector tests.
 * Not compiled for OpenCL (private-pointer params) and not needed by the kernel. */
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
 * pattern of `l >> 16` (arithmetic shift), computed portably so signed-shift
 * semantics can't vary between compilers / OpenCL implementations. */
RK_INLINE rk_u64 rk_block_pos_seed(rk_i32 x, rk_i32 y, rk_i32 z) {
    rk_i32 xm = (rk_i32)((rk_u32)x * (rk_u32)3129871); /* 32-bit wrap */
    rk_u64 xw = (rk_u64)(rk_i64)xm;                    /* sign-extend to 64 */
    rk_u64 zw = ((rk_u64)(rk_i64)z) * (rk_u64)116129781; /* == (long)z * 116129781L, low 64 bits */
    rk_u64 yw = (rk_u64)(rk_i64)y;

    rk_u64 u = xw ^ zw ^ yw;
    u = u * u * (rk_u64)42317861 + u * (rk_u64)11;

    /* arithmetic right shift by 16 */
    rk_u64 fill = ((rk_u64)0 - (u >> 63)) << 48;
    return (u >> 16) | fill;
}

/* Top 24 bits of the positional RNG draw at (x, y, z). */
RK_INLINE rk_u32 rk_bits24_at(rk_u64 derived_lo, rk_u64 derived_hi, rk_i32 x, rk_i32 y, rk_i32 z) {
    rk_u64 h = rk_block_pos_seed(x, y, z);
    rk_u64 n = rk_xoro_first(h ^ derived_lo, derived_hi);
    return (rk_u32)(n >> 40);
}

/* Bedrock test on a single plane: 1 if bedrock, 0 otherwise.
 * `threshold` is precomputed by the host for this y (see rk_floor_threshold). */
RK_INLINE int rk_is_bedrock_floor(rk_u64 derived_lo, rk_u64 derived_hi, rk_i32 x, rk_i32 y,
                                  rk_i32 z, rk_u32 threshold) {
    return rk_bits24_at(derived_lo, derived_hi, x, y, z) < threshold ? 1 : 0;
}

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
