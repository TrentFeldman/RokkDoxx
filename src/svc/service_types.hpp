// The vocabulary of the search service: the plain data types every other svc/
// file speaks in, plus a few small pure helpers. No behaviour, no I/O, no
// threads.
//
// Responsibilities:
//   - describe a search job   (Pattern, Region, SearchRequest)
//   - describe its result     (Match, JobStatus, MapCell, JobId)
//   - describe a unit of work (Tile, WorkerConfig, SearchPlan) and the Worker
//     interface
//   - small helpers: seed_from_string, format_duration, to_string(JobState),
//     and the declarations of world_patch / worker_config / build_search_plan
//     (defined in workers.cpp)
// Not this file's job: running a search (search_service.*), the compute itself
// (workers.*, opencl_worker.*), or the front-end handle (client.*).
//
// Glossary (these words mean the same thing everywhere in the code):
//   pattern     the little grid the user drew: bedrock / not bedrock / unknown
//   plane / y   the one bedrock layer (-64..-59) the pattern lies on
//   candidate   one world (x, z) position the search tests; "Gcand/s" is
//               10^9 candidates per second. A region of W x H blocks has
//               W*H candidates.
//   anchor      the pattern cell the search lines up with each candidate
//               (SearchPlan). A Match's (x, z) is where the anchor cell sits
//               in the world -- NOT the pattern's top-left corner.
//   orientation one of the 8 D4 rotations/mirrors (kOrientations); bit g of
//               Match::orient_mask = orientation g matched.
//   variant     a group of orientations that give the identical cell set
//               after recentring (symmetric patterns have fewer than 8).
//   tile        a rectangle of candidates handed to a Worker in one call.
//   map cell    a coarse square of the region for the TUI's progress picture;
//               it groups whole tiles.
#pragma once

#include <array>
#include <charconv>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "gen/bedrock.hpp"

namespace rokkdoxx::svc {

using rokkdoxx::Edition;

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

// The user's drawing. i runs along world +x (east), j along world +z (south),
// matching how the TUI and pattern files lay it out.
struct Pattern {
    int w = 0, h = 0;
    std::vector<Cell> cells;  // row-major, size w*h (index = j*w + i)

    Cell at(int i, int j) const { return cells[static_cast<std::size_t>(j) * w + i]; }

    // Drop the wildcards; keep only the cells the search has to check.
    // Row-major order. This is what WorkerConfig::knowns carries to a worker.
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

// Largest pattern side; the search refuses bigger. The GPU sizes its plane
// buffers for the halo this allows (opencl_worker.cpp), and the pattern file
// loader / TUI clamp to it.
inline constexpr int kMaxDim = 32;

// Where the front-ends checkpoint a search when no file is named. Deleted when
// the search finishes; kept on cancel / kill so rerunning the same search resumes.
inline constexpr const char* kAutosavePath = "rokkdoxx-autosave.ckpt";

// A fully known w x h pattern copied from the world at (x0, z0). (workers.cpp)
// Used to plant a guaranteed match: tests, rokksearch --benchmark / --demo.
Pattern world_patch(std::int64_t seed, int y, int x0, int z0, int w, int h,
                    Edition edition = Edition::java);

// D4, the eight ways to lay a 2D shape onto a grid: 4 rotations, each with or
// without a mirror. A screenshot is rarely aligned to world north, so the
// search tries all eight unless the user asks for "exact".
//
// Each entry is a 2x2 integer matrix [[a b][c d]]. It maps a pattern offset
// (i, j) to a world offset (du, dv) = (a*i + b*j, c*i + d*j).
// Index 0 is the identity -- the round-trip test relies on that.
// The index is also the bit position in Match::orient_mask, and the order of
// the names in tui::orient_names(), so do not reorder.
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
// The coordinates are candidate positions, i.e. where the anchor cell may be;
// the rest of the pattern may reach up to kMaxDim blocks outside the region.
struct Region {
    std::int64_t x0 = 0, x1 = 0, z0 = 0, z1 = 0;

    // The (2*radius + 1)-block square around (cx, cz).
    static Region centered(std::int64_t cx, std::int64_t cz, std::int64_t radius) {
        return Region{cx - radius, cx + radius, cz - radius, cz + radius};
    }
    // Every block inside the default Overworld world border (+/-29,999,984).
    // Front-ends map "radius -1" to this.
    static constexpr std::int64_t kWorldBorder = 29'999'984;
    static Region world() {
        return Region{-kWorldBorder, kWorldBorder - 1, -kWorldBorder, kWorldBorder - 1};
    }
    long long candidates() const {
        return static_cast<long long>(x1 - x0 + 1) * static_cast<long long>(z1 - z0 + 1);
    }
    bool valid() const { return x1 >= x0 && z1 >= z0; }
};

// --- request / result -----------------------------------------------------

// Everything a front-end says about one search. Built by rokksearch's main(),
// tui::build_request(), or a test, then handed to SearchClient::submit(). The
// service copies it, so the caller may reuse or destroy it right away.
// Validation (plane range, pattern size, empty pattern, Bedrock Edition's
// coordinate limit) happens inside the job, and failures surface as
// JobState::error -- submit() itself never rejects a request.
struct SearchRequest {
    Edition edition = Edition::java;
    std::int64_t seed = 0;  // Java only: Bedrock Edition's floor is the same in every world
    int plane_y = -60;  // which bedrock layer the pattern is on, -64..-59
    Pattern pattern;
    Region region{};
    bool all_orientations = true;
    std::uint32_t match_cap = 1u << 20;  // stop collecting after this many hits
    int tile_side = 4096;                // scheduler floor (a worker may raise it)
    std::string checkpoint_path;         // empty = no checkpointing

    // Progress kept in memory, so a front-end can save a search (or continue a
    // stopped one) without having named a file in advance. keep_checkpoint makes
    // the job maintain its checkpoint text (SearchClient::checkpoint); resume_text
    // starts a job from such text. Both use the checkpoint file's format, and
    // resume_text beats checkpoint_path when it is valid for this request.
    bool keep_checkpoint = false;
    std::string resume_text;

    // Stop as soon as a tile yields a match (the tiles already in flight finish).
    // Tiles are scanned outward from the middle of the region, so with this the
    // search ends as soon as it reaches the nearest match.
    bool stop_at_first_match = false;

    // A front-end that wants a picture of the search asks for a map_w x map_h
    // overview (0 = none; clamped to the tile grid) in JobStatus::map. The
    // scan order does not change: tiles still spiral out from the middle, so a
    // map cell can be partly done (MapCell::partial) while others start.
    int map_w = 0, map_h = 0;
};

// A place where the pattern occurs: (x, z) is the world position of the
// pattern's anchor cell (SearchPlan::anchor_i/j inside the pattern; roughly
// its centre), not of its top-left corner. orient_mask has bit g set for every
// orientation g (index into kOrientations) that matched at this origin.
// Written to stdout / saved files as "x z mask".
struct Match {
    int x, z;
    std::uint8_t orient_mask;
    bool operator==(const Match&) const = default;
};

// Lifecycle of a job:
//   pending (submitted; validating, opening the device, compiling kernels)
//   -> running <-> paused
//   -> one of done / cancelled / error
// done, cancelled and error are final; poll() keeps returning the last status
// after that.
enum class JobState { pending, running, paused, done, cancelled, error };

// One square of JobStatus::map: how far the search of that area has got, and
// whether it has produced a match. The two are independent -- an area can hold
// a match while it is still being searched.
struct MapCell {
    // partial: some of its tiles are done, none is being searched right now.
    enum Phase : std::uint8_t { pending, partial, searching, done };
    Phase phase = pending;
    bool hit = false;
    bool operator==(const MapCell&) const = default;
};

inline const char* to_string(JobState s) {
    switch (s) {
        case JobState::pending: return "pending";
        case JobState::running: return "running";
        case JobState::paused: return "paused";
        case JobState::done: return "done";
        case JobState::cancelled: return "cancelled";
        case JobState::error: return "error";
    }
    return "?";
}

// A snapshot of a running job. The service updates it after every tile; a
// front-end polls it for the progress display.
// Returned BY VALUE from poll(), so a front-end can read it without locks.
struct JobStatus {
    JobState state = JobState::pending;
    double progress = 0.0;  // 0..1
    long long candidates_total = 0;
    long long candidates_done = 0;  // includes tiles restored from a checkpoint
    std::uint64_t matches = 0;
    double elapsed_s = 0.0;  // this run only, paused time excluded
    double rate = 0.0;   // candidate origins / second, this run (not counting resumed tiles)
    double eta_s = 0.0;  // seconds left at `rate`; 0 until the first tile lands
    bool truncated = false;  // a match cap was hit (job-wide or a GPU tile's): some matches were dropped
    bool stopped_early = false;  // done, but cut short by stop_at_first_match
    std::string error;           // set when state == error

    // The search region as a map_w x map_h grid, row-major, north-west (x0, z0)
    // first. Empty unless the request asked for one; the size can be smaller
    // than asked (see SearchRequest::map_w) and is only known once the job runs.
    int map_w = 0, map_h = 0;
    std::vector<MapCell> map;
};

// Handle for a submitted job, unique per SearchService (starts at 1; 0 means
// "no job" in the TUI).
using JobId = std::uint64_t;

// "1h 02m 03s" / "4m 05s" / "7s" -- for ETA and elapsed displays.
inline std::string format_duration(double seconds) {
    const long long t = seconds > 0 ? static_cast<long long>(seconds + 0.5) : 0;
    char b[48];
    if (t >= 3600)
        std::snprintf(b, sizeof b, "%lldh %02lldm %02llds", t / 3600, t / 60 % 60, t % 60);
    else if (t >= 60)
        std::snprintf(b, sizeof b, "%lldm %02llds", t / 60, t % 60);
    else
        std::snprintf(b, sizeof b, "%llds", t);
    return b;
}

// --- worker (compute tier) ----------------------------------------------

// A rectangular block of candidate origins to test. World block coordinates.
// Made by TileScheduler::tile_at(); `index` is its position in the scheduler's
// row-major tile grid (what the checkpoint "done" list records).
struct Tile {
    int index = 0;
    std::int64_t x0 = 0, z0 = 0;
    int w = 0, h = 0;  // edge tiles are clipped to the region, so may be smaller
};

// Everything a worker needs that stays constant for the whole job. The service
// derives these from the SearchRequest once, up front (see search_service.cpp).
struct WorkerConfig {
    Edition edition = Edition::java;
    std::uint64_t derived_lo = 0;  // per-seed generator state, low 64 bits (Java)
    std::uint64_t derived_hi = 0;  // ... high 64 bits
    int plane_y = -60;
    std::uint32_t threshold = 0;   // bedrock iff bits24_at(...) < threshold
    std::vector<KnownCell> knowns;
    bool all_orientations = true;
    std::uint32_t match_cap = 1u << 20;
};

// A WorkerConfig with the per-seed part filled in; callers add the pattern. (workers.cpp)
// Called by SearchService::run() and by rokksearch's benchmark/demo, which
// drive a Worker directly.
WorkerConfig worker_config(std::int64_t seed, int plane_y, Edition edition = Edition::java);

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
//
// Reading the tables: for variant v and cell c, the block to test for a
// candidate at world (x, z) is (x + off_x[v*n_cells + c], z + off_z[v*n_cells + c]),
// and it must be bedrock iff want[c] == 1. Cell 0's offset is (0, 0) in every
// variant. If every cell of variant v passes, the candidate matches with
// orientations variant_mask[v].
struct SearchPlan {
    int n_cells = 0;     // includes the anchor at index 0
    int n_variants = 0;  // 1..8 (fewer than 8 for symmetric patterns)
    int anchor_i = 0, anchor_j = 0;
    std::vector<std::uint8_t> want;         // [n_cells]  1 = bedrock
    std::vector<std::int32_t> off_x;        // [n_variants * n_cells]  world dx from the anchor
    std::vector<std::int32_t> off_z;        // [n_variants * n_cells]
    std::vector<std::uint8_t> variant_mask; // [n_variants]  OR of (1u << g)
};

// `all_orientations == false` -> a single variant (identity only). `knowns` must not be empty.
// `threshold` (from BedrockGenerator::threshold) only decides which cell type
// is rarer. Called by CpuWorker::configure, OpenclWorker::configure, the
// brute-force reference in tests, and rokksearch --demo (to print the anchor).
SearchPlan build_search_plan(std::vector<KnownCell> knowns, std::uint32_t threshold,
                             bool all_orientations);

// The compute tier. The orchestrator configures it once, then feeds it tiles.
// Implementations: CpuWorker (workers.*) and OpenclWorker (opencl_worker.*).
//
// Call sequence (SearchService::run, rokksearch's bmark::sweep):
//   configure(cfg)                  once per job (may be called again to reset)
//   begin_tile(t1) [, begin_tile(t2)]   at most 2 outstanding
//   end_tile() -> matches of t1     strictly FIFO
//   begin_tile(t3) ... end_tile() ...
// A worker is used from one thread at a time (the job's pump thread); it may
// start its own threads internally (CpuWorker does).
class Worker {
public:
    virtual ~Worker() = default;
    // Per-job setup: build the SearchPlan, upload tables (GPU), reset the
    // match-cap / truncation state.
    virtual void configure(const WorkerConfig& cfg) = 0;

    // Pipelined dispatch -- what the pump calls. begin_tile() enqueues `tile`;
    // end_tile() blocks for and returns the matches of the OLDEST
    // not-yet-collected begin_tile() call (strict FIFO pairing): those whose
    // anchor position (see SearchPlan) lies inside that tile. May stop early
    // once match_cap is reached (then truncated() returns true).
    virtual void begin_tile(const Tile& tile) = 0;
    virtual std::vector<Match> end_tile() = 0;
    virtual bool truncated() const { return false; }

    // Tile side this backend prefers (0 = let the caller decide). A GPU wants
    // big tiles so the fixed per-dispatch cost is amortised; the CPU wants
    // smaller tiles so progress and cancellation stay responsive.
    virtual int preferred_tile_side() const { return 0; }
};

// Makes a fresh Worker for one job. SearchService calls it at the start of
// every job (on the job's thread), so each job owns its worker -- and a GPU
// worker compiles its kernels there, not at client creation.
// Built by make_worker_factory() (workers.cpp).
using WorkerFactory = std::function<std::unique_ptr<Worker>()>;

// --- seed parsing ------------------------------------------------------

// Turn a seed the user typed into the 64-bit number Minecraft would use.
// All-digits (with an optional leading '-') parses as an integer; anything
// else is hashed exactly the way Java's String.hashCode does, then
// sign-extended to 64 bits -- which is what the vanilla client does with a
// non-numeric seed.
// (A number too big for 64 bits fails from_chars and so is hashed as text.
// The unsigned multiply makes the 32-bit wrap well-defined, as in Java.)
inline std::int64_t seed_from_string(std::string_view s) {
    std::int64_t v = 0;
    const auto [end, ec] = std::from_chars(s.data(), s.data() + s.size(), v);
    if (ec == std::errc{} && end == s.data() + s.size()) return v;
    std::int32_t h = 0;
    for (unsigned char c : s) h = static_cast<std::int32_t>(31u * static_cast<std::uint32_t>(h) + c);
    return static_cast<std::int64_t>(h);
}

}  // namespace rokkdoxx::svc
