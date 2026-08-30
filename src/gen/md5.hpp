// Minimal MD5 (RFC 1321). Host-side only: used once per BedrockGenerator to
// hash the surface-rule id "minecraft:bedrock_floor" exactly as Minecraft's
// PositionalRandomFactory.fromHashOf(ResourceLocation) does
// (com.google.common.hash.Hashing.md5()).
#pragma once

#include <array>
#include <cstdint>
#include <cstring>
#include <string_view>

namespace rokkdoxx {

namespace md5_detail {

inline constexpr std::uint32_t rotl32(std::uint32_t x, std::uint32_t c) noexcept {
    return (x << c) | (x >> (32 - c));
}

inline constexpr std::array<std::uint32_t, 64> kS = {
    7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22,
    5, 9,  14, 20, 5, 9,  14, 20, 5, 9,  14, 20, 5, 9,  14, 20,
    4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
    6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21};

inline constexpr std::array<std::uint32_t, 64> kK = {
    0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a,
    0xa8304613, 0xfd469501, 0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be,
    0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821, 0xf61e2562, 0xc040b340,
    0x265e5a51, 0xe9b6c7aa, 0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8,
    0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8,
    0x676f02d9, 0x8d2a4c8a, 0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c,
    0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70, 0x289b7ec6, 0xeaa127fa,
    0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
    0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92,
    0xffeff47d, 0x85845dd1, 0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1,
    0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391};

}  // namespace md5_detail

// Returns the 16-byte MD5 digest of `data`.
inline std::array<std::uint8_t, 16> md5(std::string_view data) {
    using namespace md5_detail;

    std::uint32_t a0 = 0x67452301, b0 = 0xefcdab89, c0 = 0x98badcfe, d0 = 0x10325476;

    const std::uint64_t bit_len = static_cast<std::uint64_t>(data.size()) * 8ULL;

    // Process full 64-byte chunks straight from `data`, then a tail block (or two).
    auto process = [&](const std::uint8_t* p) {
        std::uint32_t m[16];
        for (int i = 0; i < 16; ++i) {
            m[i] = static_cast<std::uint32_t>(p[i * 4]) |
                   (static_cast<std::uint32_t>(p[i * 4 + 1]) << 8) |
                   (static_cast<std::uint32_t>(p[i * 4 + 2]) << 16) |
                   (static_cast<std::uint32_t>(p[i * 4 + 3]) << 24);
        }
        std::uint32_t A = a0, B = b0, C = c0, D = d0;
        for (int i = 0; i < 64; ++i) {
            std::uint32_t F;
            int g;
            if (i < 16) {
                F = (B & C) | (~B & D);
                g = i;
            } else if (i < 32) {
                F = (D & B) | (~D & C);
                g = (5 * i + 1) % 16;
            } else if (i < 48) {
                F = B ^ C ^ D;
                g = (3 * i + 5) % 16;
            } else {
                F = C ^ (B | ~D);
                g = (7 * i) % 16;
            }
            F = F + A + kK[i] + m[g];
            A = D;
            D = C;
            C = B;
            B = B + rotl32(F, kS[i]);
        }
        a0 += A;
        b0 += B;
        c0 += C;
        d0 += D;
    };

    const auto* bytes = reinterpret_cast<const std::uint8_t*>(data.data());
    const std::size_t n = data.size();
    std::size_t off = 0;
    for (; off + 64 <= n; off += 64) {
        process(bytes + off);
    }

    // Tail.
    const std::size_t rem = n - off;
    std::array<std::uint8_t, 128> tail{};
    std::memcpy(tail.data(), bytes + off, rem);
    tail[rem] = 0x80;
    const std::size_t tail_len = (rem + 1 <= 56) ? 64 : 128;
    for (int i = 0; i < 8; ++i) {
        tail[tail_len - 8 + i] = static_cast<std::uint8_t>((bit_len >> (8 * i)) & 0xFF);
    }
    process(tail.data());
    if (tail_len == 128) {
        process(tail.data() + 64);
    }

    std::array<std::uint8_t, 16> out{};
    const std::uint32_t words[4] = {a0, b0, c0, d0};
    for (int w = 0; w < 4; ++w) {
        out[w * 4 + 0] = static_cast<std::uint8_t>(words[w] & 0xFF);
        out[w * 4 + 1] = static_cast<std::uint8_t>((words[w] >> 8) & 0xFF);
        out[w * 4 + 2] = static_cast<std::uint8_t>((words[w] >> 16) & 0xFF);
        out[w * 4 + 3] = static_cast<std::uint8_t>((words[w] >> 24) & 0xFF);
    }
    return out;
}

// Big-endian 8-byte read (com.google.common.primitives.Longs.fromBytes).
inline std::uint64_t be_u64(const std::uint8_t* p) noexcept {
    std::uint64_t v = 0;
    for (int i = 0; i < 8; ++i) {
        v = (v << 8) | p[i];
    }
    return v;
}

}  // namespace rokkdoxx
