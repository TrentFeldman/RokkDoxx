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
//
// Where this sits:
//   bedrock_core.h   the raw math, shared by the CPU and the OpenCL kernel
//   bedrock.hpp/cpp  (this) a friendly C++ wrapper: per-seed setup + one-block
//                    queries. The library target is `rokkdoxx_gen`.
// Who uses BedrockGenerator:
//   - svc/workers.cpp: worker_config() (per-seed constants for a job) and
//     world_patch() (copy a pattern out of the world)
//   - tools/dump_bedrock.cpp, tools/rokksearch.cpp (--demo), tools/tui/model.cpp
//     (fill_from_world, bedrock_probability)
//   - every test, as the ground truth the search results are checked against
// The hot search loops do NOT call is_bedrock_floor() per block: the workers
// take derived_lo/hi + threshold() once and call the bedrock_core.h functions
// directly, which avoids this class's per-call branches.
#pragma once

#include <cstdint>

namespace rokkdoxx {

// Which game's floor to reproduce. The two editions share nothing but the
// layer range: Java is a per-seed hash per block, Bedrock Edition is a
// seed-independent Mersenne Twister per chunk (bedrock_core.h).
enum class Edition : std::uint8_t { java, bedrock };

// One world's bedrock floor. Cheap to construct (a handful of RNG steps), and
// immutable afterwards, so it is safe to share between threads.
//
//   rokkdoxx::BedrockGenerator gen(seed);          // Java, this seed
//   bool b = gen.is_bedrock_floor(x, -60, z);
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
    // Any y is accepted: below the floor is solid, above it is never bedrock.
    // Bedrock Edition regenerates the whole 16x16 chunk on every call (slow but
    // simple); bulk callers should use rk_be_chunk_rows() instead, as
    // CpuWorker does.
    bool is_bedrock_floor(int x, int y, int z) const noexcept;

    // Per-seed state the OpenCL search kernel needs: the forked positional
    // factory seeds for "minecraft:bedrock_floor". Everything else in the
    // kernel is arithmetic on (x, y, z) and these two constants.
    // Java only; 0 for Bedrock Edition. Copied into WorkerConfig by
    // svc::worker_config().
    std::uint64_t derived_lo() const noexcept { return lo_; }
    std::uint64_t derived_hi() const noexcept { return hi_; }

    // Integer cutoff for a plane: `rk_bits24_at(...) < threshold(y)` == bedrock.
    // Same value the kernel is handed. y in [-64, -59]; clamped outside.
    // Bedrock Edition: P(bedrock) * 2^24 at that y, for the search plan.
    // (build_search_plan uses it to decide whether bedrock or air is the rarer
    // cell type, and rokksearch --demo / the TUI turn it into a probability.)
    std::uint32_t threshold(int y) const noexcept;

private:
    Edition edition_;
    std::uint64_t lo_ = 0, hi_ = 0;  // the forked positional factory (bedrock.cpp)
};

}  // namespace rokkdoxx
