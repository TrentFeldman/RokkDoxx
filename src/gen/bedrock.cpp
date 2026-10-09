#include "bedrock.hpp"

#include <algorithm>

#include "bedrock_core.h"

namespace rokkdoxx {

// Reproduces BedrockReader's setup chain (Developer-Mike/minecraft-bedrock-generator):
//
//   world  = Xoroshiro(upgrade(seed))
//   d0     = world.forkPositional()                       // two consumed outputs
//   r1     = Xoroshiro_raw(md5("minecraft:bedrock_floor") ^ d0)
//   floor  = r1.forkPositional()                          // two consumed outputs
//
// == RandomState.getOrCreateRandomFactory(new ResourceLocation("bedrock_floor"))
//    then forkPositional(), in the vanilla surface system.
BedrockGenerator::BedrockGenerator(std::int64_t world_seed, Edition edition) : edition_(edition) {
    if (edition == Edition::bedrock) {
        const std::uint32_t q = 1u << 22;  // P(bedrock) at y = -64 .. -59: 1, 1, 3/4, 1/2, 1/4, 0
        const std::uint32_t t[6] = {4 * q, 4 * q, 3 * q, 2 * q, q, 0};
        std::copy(t, t + 6, thresholds_);
        return;
    }
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

    for (int i = 0; i < 6; ++i) thresholds_[i] = rk_floor_threshold(kFloorMinY + i);
}

std::uint32_t BedrockGenerator::threshold(int y) const noexcept {
    if (y <= kFloorMinY) return thresholds_[0];
    if (y >= -59) return thresholds_[5];
    return thresholds_[y - kFloorMinY];
}

bool BedrockGenerator::is_bedrock_floor(int x, int y, int z) const noexcept {
    if (edition_ == Edition::bedrock) {
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
    // bedrock_core.h. thresholds_ holds that cutoff per y.
    return rk_bits24_at(lo_, hi_, x, y, z) < thresholds_[y - kFloorMinY];
}

}  // namespace rokkdoxx
