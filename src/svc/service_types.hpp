// The vocabulary of the search service: the plain data types every other svc/
// file speaks in, plus two tiny pure helpers. No behaviour, no I/O, no threads.
//
// Responsibilities:
//   - describe a search job   (Pattern, Region, SearchRequest)
//   - describe its result     (Match, JobStatus, JobId)
//   - describe a unit of work (Tile, WorkerConfig) and the Worker interface
//   - parse a world seed string (seed_from_string)
// Not this file's job: running a search (search_service.*), the compute itself
// (workers.*, opencl_worker.*), or the front-end handle (client.*).
#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace rokkdoxx::svc {

// --- pattern -----------------------------------------------------------------

// One square of the picture the user drew. "unknown" is a wildcard: the search
// does not care what the world has there.
enum class Cell : std::uint8_t { unknown = 0, bedrock = 1, not_bedrock = 2 };

// A pattern square that actually constrains the search, flattened out of the
// grid: its offset from the pattern's top-left origin, and what it must be.
struct KnownCell {
    int i, j;            // offset from the pattern origin
    std::uint8_t want;   // 1 = bedrock, 0 = not bedrock
};

struct Pattern {
    int w = 0, h = 0;
    std::vector<Cell> cells;  // row-major, size w*h (index = j*w + i)

    Cell at(int i, int j) const { return cells[static_cast<std::size_t>(j) * w + i]; }

    // Drop the wildcards; keep only the cells the search has to check.
    std::vector<KnownCell> knowns() const {
        std::vector<KnownCell> out;
        for (int j = 0; j < h; ++j)
            for (int i = 0; i < w; ++i) {
                Cell c = at(i, j);
                if (c == Cell::bedrock) out.push_back({i, j, 1});
                else if (c == Cell::not_bedrock) out.push_back({i, j, 0});
            }
        return out;
    }
};

// D4, the eight ways to lay a 2D shape onto a grid: 4 rotations, each with or
// without a mirror. A screenshot is rarely aligned to world north, so the
// search tries all eight unless the user asks for "exact".
//
// Each entry is a 2x2 integer matrix [[a b][c d]]. It maps a pattern offset
// (i, j) to a world offset (du, dv) = (a*i + b*j, c*i + d*j).
// Index 0 is the identity -- the round-trip test relies on that.
struct Transform {
    int a, b, c, d;
};
inline constexpr std::array<Transform, 8> kOrientations = {{
    {1, 0, 0, 1},    // 0: identity
    {0, -1, 1, 0},   // 1: rotate 90
    {-1, 0, 0, -1},  // 2: rotate 180
    {0, 1, -1, 0},   // 3: rotate 270
    {-1, 0, 0, 1},   // 4: mirror
    {0, 1, 1, 0},    // 5: mirror + rotate 90
    {1, 0, 0, -1},   // 6: mirror + rotate 180
    {0, -1, -1, 0},  // 7: mirror + rotate 270
}};

// --- region ----------------------------------------------------------------

// The square of world to search, as inclusive block-coordinate ranges. 64-bit
// so a full 30M x 30M sweep does not overflow the span arithmetic (and so it
// stays 64-bit on Win64, where `long` is only 32 bits).
struct Region {
    std::int64_t x0 = 0, x1 = 0, z0 = 0, z1 = 0;

    static Region centered(std::int64_t cx, std::int64_t cz, std::int64_t radius) {
        return Region{cx - radius, cx + radius, cz - radius, cz + radius};
    }
    long long candidates() const {
        return static_cast<long long>(x1 - x0 + 1) * static_cast<long long>(z1 - z0 + 1);
    }
    bool valid() const { return x1 >= x0 && z1 >= z0; }
};

// --- request / result -----------------------------------------------------

struct SearchRequest {
    std::int64_t seed = 0;
    int plane_y = -60;  // which bedrock layer the pattern is on, -64..-59
    Pattern pattern;
    Region region{};
    bool all_orientations = true;
    std::uint32_t match_cap = 1u << 20;  // stop collecting after this many hits
    int tile_side = 4096;                // scheduler floor (a worker may raise it)
    std::string checkpoint_path;         // empty = no checkpointing
};

// A place where the pattern occurs. orient_mask has bit g set for every
// orientation g (index into kOrientations) that matched at this origin.
struct Match {
    int x, z;
    std::uint8_t orient_mask;
};

enum class JobState { pending, running, done, cancelled, error };

inline const char* to_string(JobState s) {
    switch (s) {
        case JobState::pending: return "pending";
        case JobState::running: return "running";
        case JobState::done: return "done";
        case JobState::cancelled: return "cancelled";
        case JobState::error: return "error";
    }
    return "?";
}

// A snapshot of a running job. The service updates it after every tile; a
// front-end polls it for the progress bar.
struct JobStatus {
    JobState state = JobState::pending;
    double progress = 0.0;  // 0..1
    long long candidates_total = 0;
    long long candidates_done = 0;
    std::uint64_t matches = 0;
    double elapsed_s = 0.0;
    double rate = 0.0;  // candidate origins / second
    bool truncated = false;
    std::string error;
};

using JobId = std::uint64_t;

// --- worker (compute tier) ----------------------------------------------

// A rectangular block of candidate origins to test. World block coordinates.
struct Tile {
    int index = 0;
    std::int64_t x0 = 0, z0 = 0;
    int w = 0, h = 0;
};

// Everything a worker needs that stays constant for the whole job. The service
// derives these from the SearchRequest once, up front (see search_service.cpp).
struct WorkerConfig {
    std::uint64_t derived_lo = 0;  // per-seed generator state, low 64 bits
    std::uint64_t derived_hi = 0;  // ... high 64 bits
    int plane_y = -60;
    std::uint32_t threshold = 0;   // bedrock iff bits24_at(...) < threshold
    std::vector<KnownCell> knowns;
    bool all_orientations = true;
    std::uint32_t match_cap = 1u << 20;
};

// --- search plan (shared prep for both workers) ---------------------------
//
// Turns the user's known cells into the exact per-candidate test both workers
// run, so the CPU and the GPU cannot disagree. Built once per job in
// build_search_plan() (workers.cpp).
//
// The key move ("shared anchor"): cell 0 is a rare pattern cell, and every
// other cell is stored *relative to it*. D4 rotations/mirrors fix the origin,
// so cell 0 lands at the candidate coordinate for all 8 orientations -- one
// bedrock test there rejects every orientation at once. A match is therefore
// reported at the world position of that anchor cell (anchor_i / anchor_j give
// its offset inside the pattern, if a caller needs to translate back).
//
// Orientations whose transformed cell set is identical are collapsed into one
// "variant"; variant_mask[v] carries the original orientation bits so the
// output is unchanged.
struct SearchPlan {
    int n_cells = 0;     // includes the anchor at index 0
    int n_variants = 0;  // 1..8 (fewer than 8 for symmetric patterns)
    int anchor_i = 0, anchor_j = 0;
    std::vector<std::uint8_t> want;         // [n_cells]  1 = bedrock
    std::vector<std::int32_t> off_x;        // [n_variants * n_cells]  world dx from the anchor
    std::vector<std::int32_t> off_z;        // [n_variants * n_cells]
    std::vector<std::uint8_t> variant_mask; // [n_variants]  OR of (1u << g)
};

// `all_orientations == false` -> a single variant (identity only).
SearchPlan build_search_plan(std::vector<KnownCell> knowns, std::uint32_t threshold,
                             bool all_orientations);

// The compute tier. The orchestrator configures it once, then feeds it tiles.
// Implementations: CpuWorker (workers.*) and OpenclWorker (opencl_worker.*).
class Worker {
public:
    virtual ~Worker() = default;
    virtual std::string name() const = 0;
    virtual void configure(const WorkerConfig& cfg) = 0;

    // Matches whose origin lies inside `tile`. May stop early once match_cap is
    // reached (then truncated() returns true).
    virtual std::vector<Match> run_tile(const Tile& tile) = 0;
    virtual bool truncated() const { return false; }

    // Tile side this backend prefers (0 = let the caller decide). A GPU wants
    // big tiles so the fixed per-dispatch cost is amortised; the CPU wants
    // smaller tiles so progress and cancellation stay responsive.
    virtual int preferred_tile_side() const { return 0; }
};

using WorkerFactory = std::function<std::unique_ptr<Worker>()>;

// --- seed parsing ------------------------------------------------------

// Turn a seed the user typed into the 64-bit number Minecraft would use.
// All-digits (with an optional leading '-') parses as an integer; anything
// else is hashed exactly the way Java's String.hashCode does, then
// sign-extended to 64 bits -- which is what the vanilla client does with a
// non-numeric seed.
inline std::int64_t seed_from_string(std::string_view s) {
    if (!s.empty()) {
        std::size_t i = (s[0] == '-') ? 1 : 0;
        bool numeric = i < s.size();
        for (; i < s.size(); ++i)
            if (s[i] < '0' || s[i] > '9') {
                numeric = false;
                break;
            }
        if (numeric) {
            try {
                return static_cast<std::int64_t>(std::stoll(std::string(s)));
            } catch (...) {
            }
        }
    }
    std::int32_t h = 0;
    for (unsigned char c : s) h = static_cast<std::int32_t>(31u * static_cast<std::uint32_t>(h) + c);
    return static_cast<std::int64_t>(h);
}

}  // namespace rokkdoxx::svc
