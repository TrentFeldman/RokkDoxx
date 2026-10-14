// BedrockGenerator: per-seed setup + single-block queries on top of the shared
// math in bedrock_core.h. See bedrock.hpp for who uses it.
#include "bedrock.hpp"

#include <algorithm>

#include "bedrock_core.h"

namespace rokkdoxx {

namespace {
// threshold(y) for y = -64 .. -59, as P(bedrock) * 2^24. Java: ceil(prob * 2^24)
// with prob = vanilla's Mth.map(y, -64, -59, 1.0, 0.0) = 1.0, 0.8, 0.6, 0.4,
// 0.2, 0 (bedrock_core.h's header says why ceil() makes the integer compare
// exact). Bedrock Edition: 1, 1, 3/4, 1/2, 1/4, 0.
constexpr std::uint32_t kJavaThresholds[6] = {16777216, 13421773, 10066330, 6710887, 3355444, 0};
constexpr std::uint32_t kBedrockThresholds[6] = {16777216, 16777216, 12582912, 8388608, 4194304, 0};
}  // namespace

// Reproduces BedrockReader's setup chain (Developer-Mike/minecraft-bedrock-generator):
//
//   world  = Xoroshiro(upgrade(seed))
//   d0     = world.forkPositional()                       // two consumed outputs
//   r1     = Xoroshiro_raw(md5("minecraft:bedrock_floor") ^ d0)
//   floor  = r1.forkPositional()                          // two consumed outputs
//
// == RandomState.getOrCreateRandomFactory(new ResourceLocation("bedrock_floor"))
//    then forkPositional(), in the vanilla surface system.
//
// The result is two 64-bit numbers (lo_, hi_) that are all a block query needs
// from the seed: rk_bits24_at() xors the block hash into lo_ and draws once.
// Doing this chain once per job (in svc::worker_config) is what lets the GPU
// kernel take the seed as two plain ulongs.
BedrockGenerator::BedrockGenerator(std::int64_t world_seed, Edition edition) : edition_(edition) {
    if (edition == Edition::bedrock) return;  // no seed chain: the floor is the same in every world
    std::uint64_t lo, hi;
    rk_xoro_seed(static_cast<std::uint64_t>(world_seed), &lo, &hi);
    const std::uint64_t d0_lo = rk_xoro_next(&lo, &hi);
    const std::uint64_t d0_hi = rk_xoro_next(&lo, &hi);

    // md5("minecraft:bedrock_floor") as two big-endian longs -- a constant, so
    // it's baked in (tests/reference/bedrock_ref.py recomputes it via hashlib).
    lo = 0xbbf7928b7bf1d285ULL ^ d0_lo;
    hi = 0xc4dc7cf90e1b3b94ULL ^ d0_hi;
    if ((lo | hi) == 0) {  // Java's all-zero-state fallback for a raw state
        lo = RK_GOLDEN_RATIO_64;
        hi = RK_SILVER_RATIO_64;
    }
    lo_ = rk_xoro_next(&lo, &hi);
    hi_ = rk_xoro_next(&lo, &hi);
}

// Clamped table lookup: below the floor behaves like y = -64 (always), above
// it like y = -59 (never).
std::uint32_t BedrockGenerator::threshold(int y) const noexcept {
    const std::uint32_t* t = edition_ == Edition::bedrock ? kBedrockThresholds : kJavaThresholds;
    return t[std::clamp(y, kFloorMinY, kFloorMaxY) - kFloorMinY];
}

bool BedrockGenerator::is_bedrock_floor(int x, int y, int z) const noexcept {
    if (edition_ == Edition::bedrock) {
        // Round the coordinates the way the game does (only matters past
        // +/-2^24), then generate the column's chunk and pick its bit.
        // `>> 4` / `& 15` are floor-division / modulo by 16 that also work for
        // negative coordinates (arithmetic shift), so -1 lands in chunk -1 at 15.
        const int sx = rk_be_coord(x), sz = rk_be_coord(z);
        std::uint32_t rows[16];
        rk_be_chunk_rows(sx >> 4, sz >> 4, y + 63, rows);
        return (rows[sz & 15] >> (sx & 15)) & 1u;
    }
    if (y <= kFloorMinY) return true;   // y == -64: always bedrock
    if (y > kFloorMaxY) return false;   // y >= -58: never bedrock (floor)

    // VerticalGradientCondition. Vanilla computes
    //   place bedrock  iff  (double)nextFloat() < Mth.map(y, -64, -59, 1.0, 0.0)
    // which is exactly  rk_bits24_at(...) < ceil(prob_double * 2^24)  -- see
    // bedrock_core.h. kJavaThresholds holds that cutoff per y.
    return rk_bits24_at(lo_, hi_, x, y, z) < kJavaThresholds[y - kFloorMinY];
}

}  // namespace rokkdoxx
