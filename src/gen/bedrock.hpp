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
#pragma once

#include <cstdint>

#include "positional_random.hpp"

namespace rokkdoxx {

class BedrockGenerator {
public:
    // `vertical_gradient` anchors for "minecraft:bedrock_floor":
    //   true_at_and_below = bottom            = -64  (always bedrock)
    //   false_at_and_above = aboveBottom(5)   = -59  (never bedrock)
    static constexpr int kFloorMinY = -64;
    static constexpr int kFloorMaxY = -59;

    // Default sampling plane for the M(x, z) plane function: the highest y with
    // non-trivial variation (P(bedrock) = 0.2).
    static constexpr int kDefaultPlaneY = -60;

    explicit BedrockGenerator(std::int64_t world_seed);

    // True iff block (x, y, z) is bedrock in the Overworld bedrock floor.
    bool is_bedrock_floor(int x, int y, int z) const noexcept;

    // M(x, z): bedrock presence on a single horizontal plane.
    bool floor_plane(int x, int z, int y = kDefaultPlaneY) const noexcept {
        return is_bedrock_floor(x, y, z);
    }

    // Per-seed state the OpenCL search kernel needs: the forked positional
    // factory seeds for "minecraft:bedrock_floor". Everything else in the
    // kernel is arithmetic on (x, y, z) and these two constants.
    std::uint64_t derived_lo() const noexcept { return floor_.seed_lo; }
    std::uint64_t derived_hi() const noexcept { return floor_.seed_hi; }

    // Integer cutoff for a plane: `rk_bits24_at(...) < threshold(y)` == bedrock.
    // Same value the kernel is handed. y in [-64, -59]; clamped outside.
    std::uint32_t threshold(int y) const noexcept;

private:
    PositionalRandom floor_;
    std::uint32_t thresholds_[6];  // y = -64 .. -59
};

}  // namespace rokkdoxx
