/*
 * bedrock_core.h -- Minecraft 26.2 Overworld bedrock-floor generation math
 * (Java Edition; Bedrock Edition's is further down), written in the common
 * subset of C++20 and OpenCL C so the CPU library and the GPU kernel share
 * one implementation.
 *
 * Host-only pieces (seeding and stepping the RNG for the fork chain in
 * bedrock.cpp) live behind
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
 *
 * How this file reaches each consumer:
 *   - C++ (host): #included by gen/bedrock.cpp, svc/workers.cpp (CpuWorker's
 *     hot loop), tools/rokksearch.cpp and the tests.
 *   - GPU: NOT #included. CMake (configure_file) or build.sh pastes this
 *     file's text into the generated kernel_sources.h as a string, and
 *     OpenclWorker compiles "this file + src/cl/search_tile.cl" at runtime.
 *     So every comment and line here is also kernel source.
 *
 * Rules for editing (both compilers must accept every line outside the host
 * guards):
 *   - plain C: no references, templates, namespaces, std::, or `//` comments
 *     (kept to C-style for one consistent dialect);
 *   - pointers in functions the kernel calls are private-address-space
 *     pointers (rk_be_chunk_rows' `rows` is a local array in the kernel);
 *   - the text must not contain the raw-string terminator kernel_sources.h
 *     uses (close paren, ROKKCL, double quote), nor the kernel declaration that
 *     opencl_worker.cpp's specialized_source() searches for (that search
 *     takes the first hit, and this file comes first).
 *   - re-run CMake's configure (or just `cmake --build`, which notices) after
 *     editing, or the GPU keeps running the old embedded copy.
 * Any change must keep tests/test_bedrock.cpp (Java vectors, BDS samples) and
 * tests/test_gpu.cpp (CPU/GPU bit-exactness) green.
 */
#ifndef ROKKDOXX_BEDROCK_CORE_H
#define ROKKDOXX_BEDROCK_CORE_H

/* Fixed-width integer names that mean the same thing in both dialects:
 * OpenCL C's ulong/long/uint/int are always 64/64/32/32 bits. */
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

/* Rotate left by k bits. k must be 1..63 on the host (a shift by 64 is
 * undefined in C); every caller passes a constant in that range. OpenCL has a
 * native rotate() builtin, which compiles to one instruction on most GPUs. */
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
 * state and a single draw.
 * The output function of Xoroshiro128++ is rotl(lo + hi, 17) + lo; the state
 * update (see rk_xoro_next) is skipped because nobody reads the next state.
 * The all-zero guard mirrors Java's Xoroshiro128PlusPlus constructor, which
 * swaps an all-zero state for the two constants above. */
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

/* Java: new Xoroshiro128PlusPlusRandom(long seed). Never all-zero, so no fallback.
 * ("Seed upgrade": the 64-bit seed is spread over 128 bits of state with two
 * SplitMix64 finalizer rounds.) */
RK_INLINE void rk_xoro_seed(rk_u64 seed, rk_u64 *lo, rk_u64 *hi) {
    rk_u64 l = seed ^ RK_SILVER_RATIO_64;
    *lo = rk_mix_stafford13(l);
    *hi = rk_mix_stafford13(l + RK_GOLDEN_RATIO_64);
}

/* Java Xoroshiro128PlusPlus.nextLong(): returns one output and advances the
 * state in place. Used by BedrockGenerator's constructor to fork the
 * per-feature factory, and by test_bedrock.cpp against Java vectors. */
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
 * pattern of `l >> 16` (arithmetic shift).
 * The Java source is
 *     long l = (long)(x * 3129871) ^ (long)z * 116129781L ^ (long)y;
 *     l = l * l * 42317861L + l * 11L;
 *     return l >> 16;
 * Note the asymmetry: x is multiplied in 32-bit int arithmetic (wraps) and
 * then sign-extended, while z is widened first and multiplied in 64 bits.
 * Getting that wrong only shows up at large |x|, which is why the tests use
 * coordinates near +/-2^31. All the 64-bit multiplies are done unsigned so
 * overflow is well-defined (two's-complement wrap, as in Java). */
RK_INLINE rk_u64 rk_block_pos_seed(rk_i32 x, rk_i32 y, rk_i32 z) {
    rk_i32 xm = (rk_i32)((rk_u32)x * (rk_u32)3129871); /* 32-bit wrap */
    rk_u64 xw = (rk_u64)(rk_i64)xm;                    /* sign-extend to 64 */
    rk_u64 zw = ((rk_u64)(rk_i64)z) * (rk_u64)116129781; /* == (long)z * 116129781L, low 64 bits */
    rk_u64 yw = (rk_u64)(rk_i64)y;

    rk_u64 u = xw ^ zw ^ yw;
    u = u * u * (rk_u64)42317861 + u * (rk_u64)11;
    return (rk_u64)((rk_i64)u >> 16); /* signed >> is arithmetic in C++20 and OpenCL C */
}

/* Top 24 bits of the positional RNG draw at (x, y, z).
 * This is the whole per-block cost of the Java search: one hash, one xor with
 * the per-seed factory (derived_lo/hi from BedrockGenerator), one Xoroshiro
 * output. `>> 40` keeps the 24 bits Java's nextFloat() would use
 * (nextLong() >>> 40). Compare the result against
 * BedrockGenerator::threshold(y): `bits < threshold` == bedrock.
 * Called per block by CpuWorker (java_bed), fill_plane and dump_plane in
 * search_tile.cl, and BedrockGenerator::is_bedrock_floor. */
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
 * wrap): output k = lx * 16 + lz, mod 4.
 *
 * So per layer: y -62 is bedrock where r >= 1 (3/4 of columns), y -61 where
 * r >= 2 (1/2), y -60 where r == 3 (1/4), y -59 never. Because a column is
 * solid up to its top, the layers are nested -- unlike Java, where each block
 * is an independent draw.
 *
 * MT19937 refresher (needed to follow rk_be_chunk_rows):
 *   - seeding (init_genrand) fills 624 words: w[0] = seed,
 *     w[i] = 1812433253 * (w[i-1] ^ (w[i-1] >> 30)) + i     (RK_MT_INIT)
 *   - output k (k < 624) is temper(twist(w[k], w[k+1], w[k+397])), where
 *     words at index >= 624 wrap around to the ALREADY-TWISTED new words,
 *     i.e. index k+397 >= 624 means new word k-227.
 * Only the first 256 outputs are needed (one per column of the chunk). */

/* One step of MT19937's seeding recurrence: word i from word i-1 (= v). */
#define RK_MT_INIT(v, i) ((rk_u32)1812433253u * ((v) ^ ((v) >> 30)) + (rk_u32)(i))

/* MT19937's twist of state words (a, b) with the word 397 ahead, c.
 * a = w[k] supplies the top bit, b = w[k+1] the low 31 bits; 0x9908b0df is
 * MT19937's twist matrix constant. Returns the new w[k] (untempered). */
RK_INLINE rk_u32 rk_mt_twist(rk_u32 a, rk_u32 b, rk_u32 c) {
    const rk_u32 y = (a & 0x80000000u) | (b & 0x7fffffffu);
    return c ^ (y >> 1) ^ ((y & 1u) ? 0x9908b0dfu : 0u);
}

/* The low two bits of MT19937's tempered output. Tempering is linear over
 * GF(2), so on the GPU each bit is the parity of a mask of the untempered word:
 * two popcounts. A CPU build may lack popcnt (x86-64 baseline), so it tempers.
 * Both branches return exactly temper(y) & 3 == the game's `out % 4`
 * (tests/test_bedrock.cpp compares against std::mt19937). */
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
 * which a GPU would spill to memory.
 *
 * `need` is the smallest r whose column reaches this y: a block is bedrock iff
 * r >= need (need <= 0: always; need >= 4: never). Callers pass y + 63.
 * `rows` must hold 16 words; bits 16..31 of each are left 0.
 *
 * Cursor meanings, for output k:
 *   a  = seeded w[k]        a1 = seeded w[k+1]
 *   b  = seeded w[k+397]    (used while k < 227, i.e. while k+397 < 624)
 *   c  = seeded w[j], d = seeded w[j+397], with j = k - 227: from k = 227 on,
 *        "word k+397" has wrapped to the new w[j], which is twist(c, c1, d).
 * Each cursor only ever moves forward one word per output, so the whole chunk
 * costs about 940 seeding steps and 285 twists, all in registers.
 *
 * Called by: fill_plane_be (search_tile.cl, 2 chunks per work-item),
 * CpuWorker::run_tile (prefills every chunk a tile can touch), and
 * BedrockGenerator::is_bedrock_floor (one chunk per call). */
RK_INLINE void rk_be_chunk_rows(rk_i32 cx, rk_i32 cz, rk_i32 need, rk_u32 *rows) {
    const rk_u32 seed = (rk_u32)cx * 341872712u + (rk_u32)cz * 132899541u;
    rk_u32 a = seed; /* word k */
    rk_u32 b = seed; /* word k + 397 */
    for (int i = 1; i <= 397; ++i) b = RK_MT_INIT(b, i); /* b = seeded w[397] */
    rk_u32 c = seed, d = b; /* words j and j + 397, j = k - 227 */
    for (int lz = 0; lz < 16; ++lz) rows[lz] = 0;
    for (int lx = 0; lx < 16; ++lx)
        for (int lz = 0; lz < 16; ++lz) {
            const int k = lx * 16 + lz; /* the game draws x-major: output k is column (lx, lz) */
            const rk_u32 a1 = RK_MT_INIT(a, k + 1);
            rk_u32 ahead; /* w[k+397] as MT19937 sees it at output k */
            if (k < 227) {
                ahead = b;
                b = RK_MT_INIT(b, k + 398);
            } else {
                const int j = k - 227;
                const rk_u32 c1 = RK_MT_INIT(c, j + 1);
                ahead = rk_mt_twist(c, c1, d); /* the already-twisted new w[j] */
                c = c1;
                d = RK_MT_INIT(d, j + 398);
            }
            const rk_i32 r = (rk_i32)rk_mt_low2(rk_mt_twist(a, a1, ahead));
            rows[lz] |= (rk_u32)(r >= need) << lx;
            a = a1;
        }
}

/* The game reads the column at the block position rounded through a float, so
 * past +/-2^24 an odd x or z shows its neighbour's column.
 * (A float has a 24-bit mantissa: beyond 2^24 only even integers are
 * representable, beyond 2^25 only multiples of 4, and so on. The cast rounds
 * to nearest, ties to even.) Callers convert BOTH x and z through this before
 * picking the chunk and the bit. The service refuses Bedrock Edition regions
 * past +/-2^25 because the GPU's remap_plane_be only handles the one-block
 * shift of the 2^24..2^25 band. */
RK_INLINE rk_i32 rk_be_coord(rk_i32 v) { return (rk_i32)(float)v; }

#endif /* ROKKDOXX_BEDROCK_CORE_H */
