// Minecraft 26.2 Overworld bedrock-floor generation.
//
// Bedrock placement is a surface rule ("minecraft:bedrock_floor") using a
// `vertical_gradient` condition. For a fixed world seed this is a pure function
//   B(seed, x, y, z) -> {bedrock, not bedrock}
// with no dependence on biome, terrain noise, or chunk state -- which is what
// makes the RokkDoxx pattern search possible (see the README).
//
// Overworld noise settings use `legacy_random_source: false`, i.e. the
// Xoroshiro128++ positional RNG. This code path is unchanged since Java 1.18.
//
// Bedrock Edition has its own generator, the same in every world: see
// bedrock_core.h.
#pragma once

#include <cstdint>

namespace rokkdoxx {

enum class Edition : std::uint8_t { java, bedrock };

class BedrockGenerator {
public:
    // `vertical_gradient` anchors for "minecraft:bedrock_floor":
    //   true_at_and_below = bottom            = -64  (always bedrock)
    //   false_at_and_above = aboveBottom(5)   = -59  (never bedrock)
    static constexpr int kFloorMinY = -64;
    static constexpr int kFloorMaxY = -59;

    // Bedrock Edition ignores `world_seed`.
    explicit BedrockGenerator(std::int64_t world_seed, Edition edition = Edition::java);

    // True iff block (x, y, z) is bedrock in the Overworld bedrock floor.
    bool is_bedrock_floor(int x, int y, int z) const noexcept;

    // Per-seed state the OpenCL search kernel needs: the forked positional
    // factory seeds for "minecraft:bedrock_floor". Everything else in the
    // kernel is arithmetic on (x, y, z) and these two constants.
    std::uint64_t derived_lo() const noexcept { return lo_; }
    std::uint64_t derived_hi() const noexcept { return hi_; }

    // Integer cutoff for a plane: `rk_bits24_at(...) < threshold(y)` == bedrock.
    // Same value the kernel is handed. y in [-64, -59]; clamped outside.
    // Bedrock Edition: P(bedrock) * 2^24 at that y, for the search plan.
    std::uint32_t threshold(int y) const noexcept;

private:
    Edition edition_;
    std::uint64_t lo_ = 0, hi_ = 0;  // the forked positional factory (bedrock.cpp)
    std::uint32_t thresholds_[6];    // y = -64 .. -59
};

}  // namespace rokkdoxx
