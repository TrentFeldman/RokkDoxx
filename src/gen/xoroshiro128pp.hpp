// Thin C++ wrapper over the shared integer core in bedrock_core.h. The actual
// Xoroshiro128++ / seed-upgrade math lives there so the CPU library and the
// OpenCL kernel can't drift apart.
//
// Ported 1:1 from Minecraft's XoroshiroRandomSource / RandomSupport; unchanged
// since Java 1.18, still current in 26.2. Cross-checked against
// Developer-Mike/minecraft-bedrock-generator (Java) and
// silversquirl/bedrock-finder-118 (Zig).
#pragma once

#include <cstdint>

#include "bedrock_core.h"

namespace rokkdoxx {

inline constexpr std::uint64_t kSilverRatio64 = RK_SILVER_RATIO_64;
inline constexpr std::uint64_t kGoldenRatio64 = RK_GOLDEN_RATIO_64;

inline std::uint64_t mix_stafford13(std::uint64_t z) noexcept { return rk_mix_stafford13(z); }

struct Xoroshiro128PP {
    std::uint64_t lo;
    std::uint64_t hi;

    // Java: new Xoroshiro128PlusPlusRandomImpl(long, long) -- raw state + all-zero fallback.
    static Xoroshiro128PP from_raw(std::uint64_t lo, std::uint64_t hi) noexcept {
        if ((lo | hi) == 0ULL) return Xoroshiro128PP{kGoldenRatio64, kSilverRatio64};
        return Xoroshiro128PP{lo, hi};
    }

    // Java: new Xoroshiro128PlusPlusRandom(long seed).
    static Xoroshiro128PP from_seed(std::uint64_t seed) noexcept {
        const std::uint64_t l = seed ^ kSilverRatio64;
        const std::uint64_t m = l + kGoldenRatio64;
        return from_raw(rk_mix_stafford13(l), rk_mix_stafford13(m));
    }

    std::uint64_t next() noexcept { return rk_xoro_next(&lo, &hi); }

    // Java nextFloat(): next(24) * 0x1.0p-24f. Exact: the shifted value is < 2^24.
    float next_float() noexcept { return static_cast<float>(next() >> 40) * 5.9604645e-8f; }
};

}  // namespace rokkdoxx
