// The orchestrator -- "the CPU as the organizer" between a front-end and the
// compute device. Everything in this file runs on the CPU and coordinates work;
// none of it does the bedrock math.
//
// Responsibilities:
//   - TileScheduler: cut the search region into tiles, hand them out, and
//     remember which are finished (so a job can resume from a checkpoint file).
//   - ResultSink: gather per-tile matches, deduplicate by (x, z), OR together
//     the orientation masks.
//   - SearchService: the job registry + the per-job loop that pumps tiles
//     through a Worker, updates JobStatus, and honours cancellation.
// Not this file's job: the compute (workers.*, opencl_worker.*) or the
// front-end handle (client.*).
#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "service_types.hpp"

namespace rokkdoxx::svc {

// --- TileScheduler ---------------------------------------------------------

class TileScheduler {
public:
    // `tile_side` is a floor. It is doubled until the tile count is manageable,
    // so the full 30M x 30M world border does not create billions of tiles.
    TileScheduler(Region region, int tile_side, int max_tiles = 2'000'000);

    int tile_count() const { return n_; }
    int effective_tile_side() const { return tile_side_; }
    long long total_candidates() const { return region_.candidates(); }

    // Fill `out` with the next unfinished tile; false when none remain.
    bool next(Tile& out);
    void mark_done(const Tile& tile);

    int done_count() const;
    long long candidates_done() const;

    // Checkpoint I/O. `fingerprint` guards against loading a checkpoint that
    // was written for a different request (see request_fingerprint).
    bool load_checkpoint(const std::string& path, std::uint64_t fingerprint);
    void save_checkpoint(const std::string& path, std::uint64_t fingerprint) const;

private:
    Tile tile_at(int index) const;

    Region region_;
    int tile_side_;
    int nx_, nz_, n_;
    std::vector<char> done_;  // 0/1 per tile
    int cursor_ = 0;
    int done_count_ = 0;
    long long candidates_done_ = 0;
    mutable std::mutex mu_;
};

// Stable hash of the parts of a request that must match for a checkpoint to
// still apply (seed, region, plane, pattern, orientation flag, tile floor).
std::uint64_t request_fingerprint(const SearchRequest& req);

// --- ResultSink ----------------------------------------------------------

class ResultSink {
public:
    explicit ResultSink(std::uint32_t cap) : cap_(cap) {}

    void add(const std::vector<Match>& tile_matches);

    std::uint64_t count() const;
    bool truncated() const;

    // Deduplicated, sorted by (z, then x).
    std::vector<Match> snapshot() const;

private:
    // One 64-bit key per origin: x in the high half, z in the low half.
    static std::uint64_t key(int x, int z) {
        return (static_cast<std::uint64_t>(static_cast<std::uint32_t>(x)) << 32) |
               static_cast<std::uint32_t>(z);
    }
    mutable std::mutex mu_;
    std::unordered_map<std::uint64_t, std::uint8_t> by_pos_;  // key -> OR of orient masks
    std::uint32_t cap_;
    bool truncated_ = false;
};

// --- SearchService -----------------------------------------------------

class SearchService {
public:
    explicit SearchService(WorkerFactory factory);
    ~SearchService();

    SearchService(const SearchService&) = delete;
    SearchService& operator=(const SearchService&) = delete;

    JobId submit(const SearchRequest& req);
    JobStatus poll(JobId id) const;
    std::vector<Match> results(JobId id) const;
    void cancel(JobId id);

    // Label of a freshly created worker (for UIs). Cheap.
    std::string backend_name() const;

private:
    struct Job;
    void run(Job* job) noexcept;

    WorkerFactory factory_;
    mutable std::mutex mu_;
    JobId next_id_ = 1;
    std::unordered_map<JobId, std::unique_ptr<Job>> jobs_;
};

}  // namespace rokkdoxx::svc
