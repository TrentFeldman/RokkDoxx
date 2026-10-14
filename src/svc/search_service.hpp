// The orchestrator -- "the CPU as the organizer" between a front-end and the
// compute device. Everything in this file runs on the CPU and coordinates work;
// none of it does the bedrock math.
//
// Responsibilities:
//   - TileScheduler: cut the search region into tiles, hand them out, and
//     remember which are finished (so a job can resume from a checkpoint file).
//   - SearchService: the job registry + the per-job loop that pumps tiles
//     through a Worker, updates JobStatus, and honours cancellation.
// Not this file's job: the compute (workers.*, opencl_worker.*) or the
// front-end handle (client.*).
//
// ---------------------------------------------------------------------------
// The life of one search, top to bottom (start here when tracing a bug):
//
//   front-end thread                         job thread (one per job)
//   ----------------                         ------------------------
//   rokksearch main() / tui::start_search()
//     make_client(backend)          client.cpp: resolve_backend +
//                                   make_worker_factory (workers.cpp)
//     client->submit(req)  ------>  SearchService::submit: store the Job,
//                                   spawn std::thread -> SearchService::run()
//                                     validate req (errors -> JobState::error)
//                                     worker_config() + factory_() -> Worker
//                                     TileScheduler (+ checkpoint restore)
//                                     worker->configure(cfg)
//                                       -> build_search_plan()   (workers.cpp)
//                                       -> GPU: upload tables, compile
//                                          specialized match_plane
//                                     pump loop:
//                                       sched.next(tile)
//                                       worker->begin_tile(tile)
//                                         CPU: CpuWorker::run_tile, N threads
//                                         GPU: enqueue_search -> fill_plane*
//                                              + match_plane  (search_tile.cl)
//                                       worker->end_tile() -> matches
//                                       on_tile_done: matches, scheduler,
//                                         JobStatus (under Job::mu), checkpoint
//                                     final status + results
//     loop: client->poll(id)  <---  copy of JobStatus (under Job::mu)
//     client->results(id)     <---  copy of the sorted matches
//
//   pause / cancel just flip atomics in the Job; the pump reads them between
//   tiles. Nothing ever interrupts a tile in the middle.
// ---------------------------------------------------------------------------
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
// TileScheduler is not thread-safe: it is owned by one job's
// pump thread (ponytail: add a lock if a multi-threaded pump ever lands).

// Owns the tile grid of one job: which tile comes next (spiral order), which
// are in flight / done, the per-map-cell counters behind JobStatus::map, and
// the checkpoint format. Tile indices are row-major over the grid
// (index = tz * nx + tx); the spiral is only the order next() hands them out.
// Also used directly (without a SearchService) by rokksearch's benchmark.
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

    // Fill `out` with the next unfinished tile (and mark it in flight); false
    // when none remain. Tiles already done (restored from a checkpoint) are
    // skipped. Each tile is returned at most once per scheduler.
    bool next(Tile& out);
    // The tile's results have been collected: count it as done (idempotent).
    void mark_done(int index);

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
    // tiles it's about to skip as already-done. load_checkpoint is
    // read_checkpoint over a file; a failed read changes nothing.
    // (The format is documented above read_checkpoint in search_service.cpp.)
    bool read_checkpoint(std::istream& in, std::uint64_t fingerprint, std::vector<Match>& out_matches);
    void write_checkpoint(std::ostream& out, std::uint64_t fingerprint,
                          const std::vector<Match>& matches) const;
    bool load_checkpoint(const std::string& path, std::uint64_t fingerprint,
                         std::vector<Match>& out_matches);

private:
    Tile tile_at(int index) const;   // grid index -> world rectangle
    int cell_of(int tile_index) const;  // grid index -> map cell index

    Region region_;
    int tile_side_;
    int nx_, nz_, n_;          // tiles across / down / total
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
// still apply (edition, seed, region, plane, pattern, orientation flag, tile
// floor). Not part of it: match_cap, map size, stop_at_first_match, backend --
// changing those still resumes. Note the tile floor is the REQUESTED one; a
// resume on a backend with a different preferred_tile_side gets a different
// tile grid, and a "done" list from another grid would be misread, so keep the
// backend kind the same when resuming.
std::uint64_t request_fingerprint(const SearchRequest& req);

// --- SearchService -----------------------------------------------------

// Runs search jobs, each on its own thread, and answers questions about them.
// All public methods are thread-safe and return quickly (they never wait for a
// tile). Owned by InProcessClient (client.cpp); tests also use it directly.
//
// Jobs are never removed from the registry while the service lives, so a
// JobId stays valid (poll/results keep answering) after the job ends.
class SearchService {
public:
    // `factory` is called once per job, on the job's thread, to make its Worker.
    explicit SearchService(WorkerFactory factory);
    // Cancels every job and joins every job thread (each finishes its
    // in-flight tiles first, so this can take up to a tile's time).
    ~SearchService();

    SearchService(const SearchService&) = delete;
    SearchService& operator=(const SearchService&) = delete;

    // Start a job on a new thread; returns at once. The request is copied.
    JobId submit(const SearchRequest& req);
    // The job's latest status (an unknown id gives state=error, "no such job").
    JobStatus poll(JobId id) const;
    // The job's matches, sorted by (z, x). Empty until the job has ended
    // (done or cancelled); poll() first to know when.
    std::vector<Match> results(JobId id) const;
    // Ask the job to stop: no new tiles are started, in-flight ones finish,
    // then the state becomes `cancelled` (results keep what was found).
    void cancel(JobId id);

    // Pause / resume a running job. A pause takes effect once the tiles already
    // in flight have finished (state becomes `paused`); the job keeps its place.
    void pause(JobId id, bool on);

    // The job's progress as checkpoint text (empty until the job has saved once,
    // or if the request did not set keep_checkpoint). Refreshed every few seconds
    // while running, exactly when paused, and when the job ends.
    std::string checkpoint(JobId id) const;

private:
    struct Job;                 // defined in search_service.cpp
    void run(Job* job) noexcept;  // the body of a job's thread
    Job* find(JobId id) const;

    WorkerFactory factory_;
    mutable std::mutex mu_;     // guards next_id_ and jobs_ (the map, not the jobs)
    JobId next_id_ = 1;
    std::unordered_map<JobId, std::unique_ptr<Job>> jobs_;
};

}  // namespace rokkdoxx::svc
