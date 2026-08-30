// Unit + known-answer tests for the Minecraft 26.2 bedrock-floor generator.
//
// RNG vectors are Java-generated (from Xevion/seedcrack-portal's
// TestVectorGenerator.java, MC 1.21.4 -- identical code path to 26.2).
// Bedrock fingerprints come from the independent Python reference
// (tests/reference/bedrock_ref.py), itself validated against the same Java
// vectors. The broad C++/Python cross-check is a separate ctest.
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "gen/bedrock.hpp"
#include "gen/md5.hpp"
#include "gen/positional_random.hpp"
#include "gen/xoroshiro128pp.hpp"

namespace {

int g_failures = 0;

void check(bool ok, const std::string& what) {
    if (!ok) {
        std::printf("  FAIL: %s\n", what.c_str());
        ++g_failures;
    }
}

void expect_u64(std::uint64_t got, std::uint64_t want, const std::string& what) {
    if (got != want) {
        std::printf("  FAIL: %s: got 0x%016llx want 0x%016llx\n", what.c_str(),
                    (unsigned long long)got, (unsigned long long)want);
        ++g_failures;
    }
}

void expect_i64(std::int64_t got, std::int64_t want, const std::string& what) {
    if (got != want) {
        std::printf("  FAIL: %s: got %lld want %lld\n", what.c_str(), (long long)got,
                    (long long)want);
        ++g_failures;
    }
}

std::string hex16(const std::array<std::uint8_t, 16>& d) {
    static const char* k = "0123456789abcdef";
    std::string s;
    for (std::uint8_t b : d) {
        s.push_back(k[b >> 4]);
        s.push_back(k[b & 0xF]);
    }
    return s;
}

// ---------------------------------------------------------------------------

void test_md5() {
    std::printf("test_md5\n");
    check(hex16(rokkdoxx::md5("")) == "d41d8cd98f00b204e9800998ecf8427e", "md5 empty");
    check(hex16(rokkdoxx::md5("abc")) == "900150983cd24fb0d6963f7d28e17f72", "md5 abc");
    check(hex16(rokkdoxx::md5("The quick brown fox jumps over the lazy dog")) ==
              "9e107d9d372bb6826bd81d3542a419d6",
          "md5 fox");
    check(hex16(rokkdoxx::md5("minecraft:bedrock_floor")) ==
              "bbf7928b7bf1d285c4dc7cf90e1b3b94",
          "md5 minecraft:bedrock_floor");
}

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
        auto x = rokkdoxx::Xoroshiro128PP::from_seed(static_cast<std::uint64_t>(c.seed));
        expect_i64(static_cast<std::int64_t>(x.lo), c.lo, "upgrade lo seed " + std::to_string(c.seed));
        expect_i64(static_cast<std::int64_t>(x.hi), c.hi, "upgrade hi seed " + std::to_string(c.seed));
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
        auto x = rokkdoxx::Xoroshiro128PP::from_seed(static_cast<std::uint64_t>(c.seed));
        expect_i64(static_cast<std::int64_t>(x.next()), c.n1, "nextLong1 seed " + std::to_string(c.seed));
        expect_i64(static_cast<std::int64_t>(x.next()), c.n2, "nextLong2 seed " + std::to_string(c.seed));
        expect_i64(static_cast<std::int64_t>(x.next()), c.n3, "nextLong3 seed " + std::to_string(c.seed));
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
        auto x = rokkdoxx::Xoroshiro128PP::from_seed(static_cast<std::uint64_t>(c.seed));
        check(std::fabs(x.next_float() - c.f1) < 1e-6f, "nextFloat1 seed " + std::to_string(c.seed));
        check(std::fabs(x.next_float() - c.f2) < 1e-6f, "nextFloat2 seed " + std::to_string(c.seed));
        check(std::fabs(x.next_float() - c.f3) < 1e-6f, "nextFloat3 seed " + std::to_string(c.seed));
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
        expect_i64(rokkdoxx::block_pos_seed(c.x, c.y, c.z), c.want,
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

}  // namespace

int main() {
    test_md5();
    test_seed_upgrade();
    test_next_long();
    test_next_float();
    test_block_pos_seed();
    test_floor_factory();
    test_gradient_shape();
    test_grid_fingerprints();
    test_counts_reference();

    if (g_failures == 0) {
        std::printf("\nALL PASS\n");
        return 0;
    }
    std::printf("\n%d FAILURE(S)\n", g_failures);
    return 1;
}
