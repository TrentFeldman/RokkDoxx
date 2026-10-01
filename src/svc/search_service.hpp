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
#include <iosfwd>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "service_types.hpp"

namespace rokkdoxx::svc {

// --- TileScheduler ---------------------------------------------------------
// TileScheduler and ResultSink are not thread-safe: each is owned by one job's
// pump thread (ponytail: add a lock if a multi-threaded pump ever lands).

class TileScheduler {
public:
    // `tile_side` is a floor. It is doubled until the tile count is manageable,
    // so the full 30M x 30M world border does not create billions of tiles.
    //
    // Tiles are handed out in an outward square spiral from the tile at the
    // middle of the region (the world origin, for a whole-world search): where a
    // player's builds are most likely to be comes first.
    //
    // The tile grid is also cut into map_w x map_h map cells (clamped to the
    // tile grid), the coarse picture map() reports.
    TileScheduler(Region region, int tile_side, int map_w = 1, int map_h = 1);

    int tile_count() const { return n_; }
    int effective_tile_side() const { return tile_side_; }
    long long total_candidates() const { return region_.candidates(); }

    // Fill `out` with the next unfinished tile (and mark it in flight); false
    // when none remain.
    bool next(Tile& out);
    void mark_done(const Tile& tile);

    // Note a match at this origin, so its map cell shows as a hit. Origins
    // outside the region are ignored.
    void mark_hit(std::int64_t x, std::int64_t z);

    int map_w() const { return mw_; }
    int map_h() const { return mh_; }
    // One state per map cell, row-major, north-west first.
    std::vector<MapCell> map() const;

    int done_count() const { return done_count_; }
    long long candidates_done() const { return candidates_done_; }

    // Checkpoint I/O. `fingerprint` guards against loading a checkpoint that
    // was written for a different request (see request_fingerprint).
    // `out_matches`/`matches` carry the matches found before the checkpoint
    // was written, so a resumed run doesn't lose or need to re-find them for
    // tiles it's about to skip as already-done. The path versions are the
    // stream versions over a file; a failed read changes nothing.
    bool read_checkpoint(std::istream& in, std::uint64_t fingerprint, std::vector<Match>& out_matches);
    void write_checkpoint(std::ostream& out, std::uint64_t fingerprint,
                          const std::vector<Match>& matches) const;
    bool load_checkpoint(const std::string& path, std::uint64_t fingerprint,
                         std::vector<Match>& out_matches);
    void save_checkpoint(const std::string& path, std::uint64_t fingerprint,
                         const std::vector<Match>& matches) const;

private:
    Tile tile_at(int index) const;
    void finish(int index);          // the bookkeeping behind mark_done + checkpoint load
    int cell_of(int tile_index) const;

    Region region_;
    int tile_side_;
    int nx_, nz_, n_;
    int mw_, mh_;              // map cells across / down
    std::vector<char> done_;   // 0/1 per tile
    std::vector<char> busy_;   // 0/1 per tile: handed out by next(), not yet done
    std::vector<int> cell_done_, cell_busy_, cell_hit_;  // per map cell
    std::vector<int> order_;   // tile indices, innermost ring first
    std::size_t cursor_ = 0;   // next(): position in order_
    int done_count_ = 0;
    long long candidates_done_ = 0;
};

// Stable hash of the parts of a request that must match for a checkpoint to
// still apply (seed, region, plane, pattern, orientation flag, tile floor).
std::uint64_t request_fingerprint(const SearchRequest& req);

// --- ResultSink ----------------------------------------------------------

class ResultSink {
public:
    explicit ResultSink(std::uint32_t cap) : cap_(cap) {}

    void add(const std::vector<Match>& tile_matches);

    std::uint64_t count() const { return by_pos_.size(); }
    bool truncated() const { return truncated_; }

    // Deduplicated, sorted by (z, then x).
    std::vector<Match> snapshot() const;

private:
    // One 64-bit key per origin: x in the high half, z in the low half.
    static std::uint64_t key(int x, int z) {
        return (static_cast<std::uint64_t>(static_cast<std::uint32_t>(x)) << 32) |
               static_cast<std::uint32_t>(z);
    }
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

    // Pause / resume a running job. A pause takes effect once the tiles already
    // in flight have finished (state becomes `paused`); the job keeps its place.
    void pause(JobId id, bool on);

    // The job's progress as checkpoint text (empty until the job has saved once,
    // or if the request did not set keep_checkpoint). Refreshed every few seconds
    // while running, exactly when paused, and when the job ends.
    std::string checkpoint(JobId id) const;

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
