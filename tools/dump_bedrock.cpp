// dump_bedrock <seed> <x0> <z0> <width> <height> [y|all]
//
// Prints the Overworld bedrock floor as an ASCII grid: rows are z (north->south,
// increasing), columns are x (west->east, increasing), 'X' = bedrock, '.' = not.
// With `all` (or no y), prints every layer y = -64 .. -59.
//
// Used by the differential test against the independent Python reference.
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "gen/bedrock.hpp"

namespace {

void dump_layer(const rokkdoxx::BedrockGenerator& gen, int y, long x0, long z0, long w, long h) {
    std::printf("# y=%d\n", y);
    long bedrock = 0;
    for (long dz = 0; dz < h; ++dz) {
        std::string row;
        row.reserve(static_cast<std::size_t>(w));
        for (long dx = 0; dx < w; ++dx) {
            const bool b = gen.is_bedrock_floor(static_cast<int>(x0 + dx), y,
                                                static_cast<int>(z0 + dz));
            row.push_back(b ? 'X' : '.');
            bedrock += b ? 1 : 0;
        }
        std::printf("%s\n", row.c_str());
    }
    std::fprintf(stderr, "y=%d: %ld / %ld bedrock (%.4f)\n", y, bedrock, w * h,
                 static_cast<double>(bedrock) / static_cast<double>(w * h));
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 6 || argc > 7) {
        std::fprintf(stderr,
                     "usage: %s <seed> <x0> <z0> <width> <height> [y|all]\n", argv[0]);
        return 2;
    }

    const std::int64_t seed = static_cast<std::int64_t>(std::strtoll(argv[1], nullptr, 10));
    const long x0 = std::strtol(argv[2], nullptr, 10);
    const long z0 = std::strtol(argv[3], nullptr, 10);
    const long w = std::strtol(argv[4], nullptr, 10);
    const long h = std::strtol(argv[5], nullptr, 10);

    if (w <= 0 || h <= 0) {
        std::fprintf(stderr, "width and height must be positive\n");
        return 2;
    }

    const rokkdoxx::BedrockGenerator gen(seed);

    std::printf("# seed=%lld x0=%ld z0=%ld w=%ld h=%ld\n",
                static_cast<long long>(seed), x0, z0, w, h);

    if (argc == 7 && std::strcmp(argv[6], "all") != 0) {
        dump_layer(gen, static_cast<int>(std::strtol(argv[6], nullptr, 10)), x0, z0, w, h);
    } else {
        for (int y = rokkdoxx::BedrockGenerator::kFloorMinY;
             y < rokkdoxx::BedrockGenerator::kFloorMaxY; ++y) {
            dump_layer(gen, y, x0, z0, w, h);
        }
    }
    return 0;
}
