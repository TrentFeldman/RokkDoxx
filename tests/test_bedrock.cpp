// Unit + known-answer tests for the Minecraft 26.2 bedrock-floor generator
// (Java Edition; Bedrock Edition's is test_bedrock_edition).
//
// RNG vectors are Java-generated (from Xevion/seedcrack-portal's
// TestVectorGenerator.java, MC 1.21.4 -- identical code path to 26.2).
// Bedrock fingerprints come from the independent Python reference
// (tests/reference/bedrock_ref.py), itself validated against the same Java
// vectors. The broad C++/Python cross-check is a separate ctest.
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <random>
#include <string>
#include <vector>

#include "check.hpp"
#include "gen/bedrock.hpp"
#include "gen/bedrock_core.h"

namespace {

void expect_u64(std::uint64_t got, std::uint64_t want, const std::string& what) {
    if (got != want) {
        std::printf("  FAIL: %s: got 0x%016llx want 0x%016llx\n", what.c_str(),
                    (unsigned long long)got, (unsigned long long)want);
        ++g_fail;
    }
}

void expect_i64(std::int64_t got, std::int64_t want, const std::string& what) {
    if (got != want) {
        std::printf("  FAIL: %s: got %lld want %lld\n", what.c_str(), (long long)got,
                    (long long)want);
        ++g_fail;
    }
}

// Java nextFloat(): next(24) * 0x1.0p-24f. Exact: the shifted value is < 2^24.
float next_float(rk_u64* lo, rk_u64* hi) {
    return static_cast<float>(rk_xoro_next(lo, hi) >> 40) * 5.9604645e-8f;
}

// ---------------------------------------------------------------------------

void test_seed_upgrade() {
    std::printf("test_seed_upgrade\n");
    // (seed, expected mixed lo, expected mixed hi)  -- Java-generated
    struct C {
        std::int64_t seed, lo, hi;
    };
    const std::vector<C> cases = {
        {0, 3847398142028685078LL, 7192185014346937746LL},
        {1, 5272463233947570727LL, 1927618558350093866LL},
        {42, 6720814022939733433LL, -2851323883594622011LL},
        {12345, 733019005196230046LL, -3494074583369400597LL},
        {-1, -110783831392733308LL, 2932223646667407290LL},
        {INT64_MAX, -5345562080669513825LL, -3799270749775305465LL},
        {INT64_MIN, -6382634648412944878LL, 5448932524140013571LL},
    };
    for (const auto& c : cases) {
        rk_u64 lo, hi;
        rk_xoro_seed(static_cast<std::uint64_t>(c.seed), &lo, &hi);
        expect_i64(static_cast<std::int64_t>(lo), c.lo, "upgrade lo seed " + std::to_string(c.seed));
        expect_i64(static_cast<std::int64_t>(hi), c.hi, "upgrade hi seed " + std::to_string(c.seed));
    }
}

void test_next_long() {
    std::printf("test_next_long\n");
    struct C {
        std::int64_t seed, n1, n2, n3;
    };
    const std::vector<C> cases = {
        {0, 3038984756725240190LL, -3694039286755638414LL, 4633751808701151732LL},
        {1, -1033667707219518978LL, 6451672561743293322LL, -1821890263888393630LL},
        {42, -4695948378737616609LL, 7341713790291473579LL, -7542733514721318211LL},
        {12345, -8118485274630516485LL, 8241557746459281790LL, 4143755034716878659LL},
        {-1, -8676505878415342125LL, -868585888688873692LL, -6331679347063163302LL},
    };
    for (const auto& c : cases) {
        rk_u64 lo, hi;
        rk_xoro_seed(static_cast<std::uint64_t>(c.seed), &lo, &hi);
        expect_i64(static_cast<std::int64_t>(rk_xoro_next(&lo, &hi)), c.n1, "nextLong1 seed " + std::to_string(c.seed));
        expect_i64(static_cast<std::int64_t>(rk_xoro_next(&lo, &hi)), c.n2, "nextLong2 seed " + std::to_string(c.seed));
        expect_i64(static_cast<std::int64_t>(rk_xoro_next(&lo, &hi)), c.n3, "nextLong3 seed " + std::to_string(c.seed));
    }
}

void test_next_float() {
    std::printf("test_next_float\n");
    struct C {
        std::int64_t seed;
        float f1, f2, f3;
    };
    const std::vector<C> cases = {
        {0, 0.164743662f, 0.799745679f, 0.251196146f},
        {1, 0.943964720f, 0.349745870f, 0.901235104f},
        {42, 0.745432079f, 0.397995055f, 0.591107547f},
        {12345, 0.559895992f, 0.446775734f, 0.224633396f},
        {-1, 0.529645622f, 0.952913821f, 0.656758964f},
    };
    for (const auto& c : cases) {
        rk_u64 lo, hi;
        rk_xoro_seed(static_cast<std::uint64_t>(c.seed), &lo, &hi);
        check(std::fabs(next_float(&lo, &hi) - c.f1) < 1e-6f, "nextFloat1 seed " + std::to_string(c.seed));
        check(std::fabs(next_float(&lo, &hi) - c.f2) < 1e-6f, "nextFloat2 seed " + std::to_string(c.seed));
        check(std::fabs(next_float(&lo, &hi) - c.f3) < 1e-6f, "nextFloat3 seed " + std::to_string(c.seed));
    }
}

void test_block_pos_seed() {
    std::printf("test_block_pos_seed\n");
    // (x, y, z) -> Mth.getSeed, from the Python reference (widths exercised:
    // negative coords, 32-bit overflow of the x multiply, INT32 extremes).
    struct C {
        int x, y, z;
        std::int64_t want;
    };
    const std::vector<C> cases = {
        {0, -60, 0, 2324589LL},
        {1, -61, 2, -101893194541405LL},
        {-1, -64, -1, 52541653973741LL},
        {1000000, -59, -1000000, -93255336010769LL},
        {-2000000000, -63, 2000000000, -115508855401418LL},
        {2147483647, -60, -2147483648, 5223223232237LL},
    };
    for (const auto& c : cases) {
        expect_i64(static_cast<std::int64_t>(rk_block_pos_seed(c.x, c.y, c.z)), c.want,
                   "getSeed(" + std::to_string(c.x) + "," + std::to_string(c.y) + "," +
                       std::to_string(c.z) + ")");
    }
}

void test_floor_factory() {
    std::printf("test_floor_factory\n");
    // Derived positional-factory seeds for "minecraft:bedrock_floor", seed 0.
    rokkdoxx::BedrockGenerator g(0);
    expect_u64(g.derived_lo(), 0xba3e925a8761b872ULL, "floor factory lo seed 0");
    expect_u64(g.derived_hi(), 0xbf06e276cffa4db6ULL, "floor factory hi seed 0");
}

void test_gradient_shape() {
    std::printf("test_gradient_shape\n");
    rokkdoxx::BedrockGenerator g(0);
    // y == -64 always bedrock; y >= -59 never bedrock.
    for (int x = -20; x < 20; ++x) {
        for (int z = -20; z < 20; ++z) {
            check(g.is_bedrock_floor(x, -64, z), "y=-64 solid");
            check(!g.is_bedrock_floor(x, -59, z), "y=-59 empty");
            check(!g.is_bedrock_floor(x, -58, z), "y=-58 empty");
        }
    }
    // Statistical: measured fraction close to the theoretical gradient.
    const int n = 400;
    const double expected[5] = {1.0, 0.8, 0.6, 0.4, 0.2};
    for (int i = 0; i < 5; ++i) {
        const int y = -64 + i;
        long hit = 0;
        for (int x = 0; x < n; ++x)
            for (int z = 0; z < n; ++z) hit += g.is_bedrock_floor(x, y, z) ? 1 : 0;
        const double frac = static_cast<double>(hit) / (n * n);
        check(std::fabs(frac - expected[i]) < 0.02,
              "gradient y=" + std::to_string(y) + " frac=" + std::to_string(frac));
    }
}

// FNV-1a over a bedrock bitfield; matches tests/reference/bedrock_ref.py's fnv().
std::uint64_t fnv_grid(std::int64_t seed, int y, int x0, int z0, int n) {
    rokkdoxx::BedrockGenerator g(seed);
    std::uint64_t h = 0xcbf29ce484222325ULL;
    for (int dz = 0; dz < n; ++dz) {
        for (int dx = 0; dx < n; ++dx) {
            const std::uint64_t b = g.is_bedrock_floor(x0 + dx, y, z0 + dz) ? 1u : 0u;
            h = (h ^ b) * 0x100000001b3ULL;
        }
    }
    return h;
}

void test_grid_fingerprints() {
    std::printf("test_grid_fingerprints\n");
    expect_u64(fnv_grid(1, -60, 0, 0, 128), 0x21ee57840a62b465ULL, "fnv seed 1 y=-60");
    expect_u64(fnv_grid(0, -62, -48, -48, 96), 0x22b2792f67a79145ULL, "fnv seed 0 y=-62 neg origin");
}

void test_counts_reference() {
    std::printf("test_counts_reference\n");
    // seed 0, 64x64 at origin -- exact counts from the Python reference.
    const long want[5] = {4096, 3248, 2463, 1645, 784};
    rokkdoxx::BedrockGenerator g(0);
    for (int i = 0; i < 5; ++i) {
        const int y = -64 + i;
        long hit = 0;
        for (int x = 0; x < 64; ++x)
            for (int z = 0; z < 64; ++z) hit += g.is_bedrock_floor(x, y, z) ? 1 : 0;
        expect_i64(hit, want[i], "count seed 0 y=" + std::to_string(y));
    }
}

// Bedrock Edition, read back from Bedrock Dedicated Server 1.26.52.3: each
// digit is a column's top bedrock layer + 63, rows z = z0.., columns x = x0..
// The worlds had different seeds (comments); the floor does not use them. The
// last three sit past 2^24, where odd rows and columns repeat a neighbour.
void test_bedrock_edition() {
    std::printf("test_bedrock_edition\n");
    struct C {
        int x0, z0;
        const char* tops[4];
    };
    const C cases[] = {
        {0, 0, {"0032322011212010", "3003033030203031", "1230201003320302", "0111010231220321"}},  // 12345
        {-64, -61, {"1203131301113301", "3111333020301131", "0202230332232232", "1133100201120312"}},  // 999
        {1234560, -7654320, {"3113002133130113", "2232031323030122", "3012102012330001", "0032303131221231"}},  // 777
        {5000013, 123460, {"3100003003013012", "1030212202302332", "2203212003112311", "3011120033120222"}},  // 8819392414030687460
        {16777208, -16777220, {"3021220111311102", "3021220111311102", "2103113200133333", "2321000022033312"}},  // 5
        {-29999104, 29998912, {"1111112111322202", "1111112111322202", "1101110111033300", "3303330222300031"}},  // -4242
    };
    const rokkdoxx::BedrockGenerator a(0, rokkdoxx::Edition::bedrock), b(-77, rokkdoxx::Edition::bedrock);
    for (const C& c : cases)
        for (int j = 0; j < 4; ++j)
            for (int i = 0; i < 16; ++i)
                for (int y = -64; y <= -59; ++y) {
                    const bool want = y <= -63 + (c.tops[j][i] - '0');
                    const int x = c.x0 + i, z = c.z0 + j;
                    check(a.is_bedrock_floor(x, y, z) == want && b.is_bedrock_floor(x, y, z) == want,
                          "bedrock edition " + std::to_string(x) + " " + std::to_string(y) + " " + std::to_string(z));
                }

    // rk_be_chunk_rows walks MT19937 with cursors; std::mt19937 is the textbook one.
    std::mt19937 pick(1);
    for (int t = 0; t < 50; ++t) {
        const int cx = static_cast<int>(pick()) >> 6, cz = static_cast<int>(pick()) >> 6;
        std::mt19937 mt(static_cast<std::uint32_t>(cx) * 341872712u + static_cast<std::uint32_t>(cz) * 132899541u);
        std::uint32_t rows[16];
        rk_be_chunk_rows(cx, cz, 2, rows);  // y = -61: bedrock iff draw % 4 >= 2
        bool ok = true;
        for (int k = 0; k < 256; ++k) ok = ok && ((rows[k & 15] >> (k >> 4)) & 1u) == (mt() % 4 >= 2);
        check(ok, "chunk " + std::to_string(cx) + "," + std::to_string(cz) + " vs std::mt19937");
    }
}

}  // namespace

int main() {
    test_seed_upgrade();
    test_next_long();
    test_next_float();
    test_block_pos_seed();
    test_floor_factory();
    test_gradient_shape();
    test_grid_fingerprints();
    test_counts_reference();
    test_bedrock_edition();
    return report();
}
