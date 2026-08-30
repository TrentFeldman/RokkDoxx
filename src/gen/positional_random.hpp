// Thin C++ wrapper over bedrock_core.h: Minecraft's PositionalRandomFactory /
// RandomSource.forkPositional() for Xoroshiro, plus the block-position hash
// (net.minecraft.util.Mth.getSeed / yarn MathHelper.hashCode).
#pragma once

#include <cstdint>

#include "bedrock_core.h"
#include "xoroshiro128pp.hpp"

namespace rokkdoxx {

// Mth.getSeed(x, y, z) as a signed 64-bit value (arithmetic >>16).
inline std::int64_t block_pos_seed(int x, int y, int z) noexcept {
    return static_cast<std::int64_t>(rk_block_pos_seed(x, y, z));
}

// PositionalRandomFactory for Xoroshiro == a pair of forked 64-bit seeds
// (two consumed outputs of the source).
struct PositionalRandom {
    std::uint64_t seed_lo;
    std::uint64_t seed_hi;

    static PositionalRandom fork(Xoroshiro128PP& source) noexcept {
        const std::uint64_t a = source.next();
        const std::uint64_t b = source.next();
        return PositionalRandom{a, b};
    }

    // PositionalRandomFactory.at(x, y, z): new Xoroshiro(getSeed ^ seedLo, seedHi).
    Xoroshiro128PP at(int x, int y, int z) const noexcept {
        const std::uint64_t h = static_cast<std::uint64_t>(rk_block_pos_seed(x, y, z));
        return Xoroshiro128PP::from_raw(h ^ seed_lo, seed_hi);
    }

    // Top 24 bits of the draw at (x, y, z) -- the value compared against the
    // bedrock threshold. Shares rk_bits24_at with the OpenCL kernel.
    std::uint32_t bits24_at(int x, int y, int z) const noexcept {
        return rk_bits24_at(seed_lo, seed_hi, x, y, z);
    }
};

}  // namespace rokkdoxx
