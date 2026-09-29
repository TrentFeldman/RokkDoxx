#include "bedrock.hpp"

#include "xoroshiro128pp.hpp"

namespace rokkdoxx {

namespace {

// Reproduces BedrockReader's setup chain (Developer-Mike/minecraft-bedrock-generator):
//
//   world  = Xoroshiro(upgrade(seed))
//   d0     = world.forkPositional()                       // two consumed outputs
//   r1     = Xoroshiro_raw(md5("minecraft:bedrock_floor") ^ d0)
//   floor  = r1.forkPositional()                          // two consumed outputs
//
// == RandomState.getOrCreateRandomFactory(new ResourceLocation("bedrock_floor"))
//    then forkPositional(), in the vanilla surface system.
PositionalRandom make_floor_factory(std::uint64_t world_seed) {
    Xoroshiro128PP world = Xoroshiro128PP::from_seed(world_seed);
    const PositionalRandom d0 = PositionalRandom::fork(world);

    // md5("minecraft:bedrock_floor") as two big-endian longs -- a constant, so
    // it's baked in (tests/reference/bedrock_ref.py recomputes it via hashlib).
    constexpr std::uint64_t hlo = 0xbbf7928b7bf1d285ULL;
    constexpr std::uint64_t hhi = 0xc4dc7cf90e1b3b94ULL;

    Xoroshiro128PP r1 = Xoroshiro128PP::from_raw(hlo ^ d0.seed_lo, hhi ^ d0.seed_hi);
    return PositionalRandom::fork(r1);
}

}  // namespace

BedrockGenerator::BedrockGenerator(std::int64_t world_seed)
    : floor_(make_floor_factory(static_cast<std::uint64_t>(world_seed))) {
    for (int i = 0; i < 6; ++i) thresholds_[i] = rk_floor_threshold(kFloorMinY + i);
}

std::uint32_t BedrockGenerator::threshold(int y) const noexcept {
    if (y <= kFloorMinY) return thresholds_[0];
    if (y >= -59) return thresholds_[5];
    return thresholds_[y - kFloorMinY];
}

bool BedrockGenerator::is_bedrock_floor(int x, int y, int z) const noexcept {
    if (y <= kFloorMinY) return true;   // y == -64: always bedrock
    if (y > kFloorMaxY) return false;   // y >= -58: never bedrock (floor)

    // VerticalGradientCondition. Vanilla computes
    //   place bedrock  iff  (double)nextFloat() < Mth.map(y, -64, -59, 1.0, 0.0)
    // which is exactly  rk_bits24_at(...) < ceil(prob_double * 2^24)  -- see
    // bedrock_core.h. thresholds_ holds that cutoff per y.
    return floor_.bits24_at(x, y, z) < thresholds_[y - kFloorMinY];
}

}  // namespace rokkdoxx
