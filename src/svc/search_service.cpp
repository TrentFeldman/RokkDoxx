// TileScheduler and SearchService (the per-job pump). Read the
// "life of one search" trace at the top of search_service.hpp first; the
// function that ties everything together is SearchService::run() at the
// bottom of this file.
#include "search_service.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <exception>
#include <fstream>
#include <sstream>
#include <thread>

namespace rokkdoxx::svc {

// ==========================================================================
// TileScheduler
// ==========================================================================

namespace {
// One round of an FNV-flavoured mixing step (same shape as boost::hash_combine).
std::uint64_t mix(std::uint64_t h, std::uint64_t v) {
    h ^= v + 0x9E3779B97F4A7C15ULL + (h << 6) + (h >> 2);
    return h;
}
}  // namespace

// Changing what goes into this hash invalidates every checkpoint users have
// saved (they silently restart from zero), so only ever add fields in a way
// that leaves old requests' hashes alone -- like the edition line below.
std::uint64_t request_fingerprint(const SearchRequest& req) {
    std::uint64_t h = 0xcbf29ce484222325ULL;  // FNV offset basis
    h = mix(h, static_cast<std::uint64_t>(req.seed));
    if (req.edition == Edition::bedrock) h = mix(h, 1);  // only then, so Java checkpoints stay valid
    h = mix(h, static_cast<std::uint64_t>(req.plane_y));
    h = mix(h, static_cast<std::uint64_t>(req.region.x0));
    h = mix(h, static_cast<std::uint64_t>(req.region.x1));
    h = mix(h, static_cast<std::uint64_t>(req.region.z0));
    h = mix(h, static_cast<std::uint64_t>(req.region.z1));
    h = mix(h, static_cast<std::uint64_t>(req.tile_side));
    h = mix(h, req.all_orientations ? 1u : 0u);
    h = mix(h, static_cast<std::uint64_t>(req.pattern.w));
    h = mix(h, static_cast<std::uint64_t>(req.pattern.h));
    for (Cell c : req.pattern.cells) h = mix(h, static_cast<std::uint64_t>(c));
    return h;
}

namespace {
// Cutting n tiles into `cells` map cells: cell m starts at tile split(m), so
// the cells differ in size by at most one tile. cell_of_tile() is its inverse.
// (Done independently per axis: x uses (nx_, mw_), z uses (nz_, mh_).)
// Example: n = 7 tiles, 3 cells -> starts 0, 2, 4 -> widths 2, 2, 3.
int split(int n, int cells, int m) {
    return static_cast<int>(static_cast<long long>(m) * n / cells);
}
// The largest m with split(m) <= t, in closed form: cell m owns tiles
// [m*n/cells, (m+1)*n/cells), so t is in cell floor(((t+1)*cells - 1) / n).
// tests/test_search.cpp checks it against a slow loop (slow_cell).
int cell_of_tile(int n, int cells, int t) {
    return static_cast<int>(((static_cast<long long>(t) + 1) * cells - 1) / n);
}
}  // namespace

// Called once per job by SearchService::run() (and per sweep by rokksearch's
// benchmark). The whole-world grid (~840k tiles once the side has doubled to
// 65536) is built and sorted here in tens of milliseconds.
TileScheduler::TileScheduler(Region region, int tile_side, int map_w, int map_h)
    : region_(region), tile_side_(tile_side < 1 ? 1 : tile_side) {
    constexpr long long kMaxTiles = 2'000'000;
    const long long spanx = region_.x1 - region_.x0 + 1;
    const long long spanz = region_.z1 - region_.z0 + 1;
    // Grow the tile until nx*nz fits under the cap (or the tile is absurdly
    // large). This keeps `done_` small even for a whole-world sweep.
    for (;;) {
        const long long gx = (spanx + tile_side_ - 1) / tile_side_;
        const long long gz = (spanz + tile_side_ - 1) / tile_side_;
        if (gx * gz <= kMaxTiles || tile_side_ >= (1 << 26)) {
            nx_ = static_cast<int>(gx < 1 ? 1 : gx);
            nz_ = static_cast<int>(gz < 1 ? 1 : gz);
            break;
        }
        tile_side_ *= 2;
    }
    n_ = nx_ * nz_;
    mw_ = std::clamp(map_w, 1, nx_);
    mh_ = std::clamp(map_h, 1, nz_);
    done_.assign(static_cast<std::size_t>(n_), 0);
    busy_.assign(static_cast<std::size_t>(n_), 0);
    const std::size_t cells = static_cast<std::size_t>(mw_) * static_cast<std::size_t>(mh_);
    cell_done_.assign(cells, 0);
    cell_busy_.assign(cells, 0);
    cell_hit_.assign(cells, 0);

    // Spiral order: by ring (Chebyshev distance from the centre tile), and round
    // each ring clockwise from its top-left corner.
    //
    // Each tile gets a sort key (ring << 25) | along, where `along` is its
    // position walking ring r clockwise (0 .. 8r-1) starting at the ring's
    // top-left corner. along < 8r, and r < 2^20 for any grid under kMaxTiles
    // (a 1 x 2M strip is the worst case), so along < 2^23 never spills into the
    // ring bits; sorting the keys yields ring 0, then ring 1 clockwise, etc.
    // Rings that are clipped by the grid edge simply have gaps in `along`.
    const int cx = std::clamp(static_cast<int>(((region_.x0 + region_.x1) / 2 - region_.x0) / tile_side_), 0, nx_ - 1);
    const int cz = std::clamp(static_cast<int>(((region_.z0 + region_.z1) / 2 - region_.z0) / tile_side_), 0, nz_ - 1);
    std::vector<std::pair<std::int64_t, int>> keyed(static_cast<std::size_t>(n_));
    for (int i = 0; i < n_; ++i) {
        const int dx = i % nx_ - cx, dz = i / nx_ - cz;
        const int r = std::max(std::abs(dx), std::abs(dz));
        const int along = r == 0 ? 0
                          : dz == -r ? dx + r                  // top row, left to right
                          : dx == r  ? 2 * r + (dz + r)        // right column, downward
                          : dz == r  ? 4 * r + (r - dx)        // bottom row, right to left
                                     : 6 * r + (r - dz);       // left column, upward
        keyed[static_cast<std::size_t>(i)] = {(static_cast<std::int64_t>(r) << 25) | along, i};
    }
    std::sort(keyed.begin(), keyed.end());
    order_.reserve(keyed.size());
    for (const auto& k : keyed) order_.push_back(k.second);
}

// Grid index -> world rectangle. Pure arithmetic; tiles are never stored.
Tile TileScheduler::tile_at(int index) const {
    const int tx = index % nx_;
    const int tz = index / nx_;
    Tile t;
    t.index = index;
    t.x0 = region_.x0 + static_cast<long long>(tx) * tile_side_;
    t.z0 = region_.z0 + static_cast<long long>(tz) * tile_side_;
    // The last column / row is clipped to the region edge.
    const std::int64_t xend = (tx == nx_ - 1) ? region_.x1 : t.x0 + tile_side_ - 1;
    const std::int64_t zend = (tz == nz_ - 1) ? region_.z1 : t.z0 + tile_side_ - 1;
    t.w = static_cast<int>(xend - t.x0 + 1);
    t.h = static_cast<int>(zend - t.z0 + 1);
    return t;
}

// Map cell of a tile: row cell from the tile's row, column cell from its
// column, combined row-major like JobStatus::map.
int TileScheduler::cell_of(int tile_index) const {
    return cell_of_tile(nz_, mh_, tile_index / nx_) * mw_ + cell_of_tile(nx_, mw_, tile_index % nx_);
}

// Called by the pump (SearchService::run) each time it has a free slot.
// The cursor only moves forward, so a tile skipped because it was already done
// is never revisited.
bool TileScheduler::next(Tile& out) {
    while (cursor_ < order_.size()) {
        const int index = order_[cursor_++];
        if (done_[static_cast<std::size_t>(index)]) continue;
        busy_[static_cast<std::size_t>(index)] = 1;
        ++cell_busy_[static_cast<std::size_t>(cell_of(index))];
        out = tile_at(index);
        return true;
    }
    return false;
}

// Count a tile as done exactly once. Called by the pump (a tile came back
// from the worker) and read_checkpoint (a tile a previous run finished), which
// is why it tolerates bad indices and repeats.
void TileScheduler::mark_done(int index) {
    if (index < 0 || index >= n_ || done_[static_cast<std::size_t>(index)]) return;
    const Tile t = tile_at(index);
    const std::size_t c = static_cast<std::size_t>(cell_of(index));
    done_[static_cast<std::size_t>(index)] = 1;
    ++done_count_;
    ++cell_done_[c];
    candidates_done_ += static_cast<long long>(t.w) * t.h;
    if (busy_[static_cast<std::size_t>(index)]) {
        busy_[static_cast<std::size_t>(index)] = 0;
        --cell_busy_[c];
    }
}

// Matches are attributed by coordinate, not by which tile reported them, so
// restored checkpoint matches light up the right cells too.
void TileScheduler::mark_hit(std::int64_t x, std::int64_t z) {
    if (x < region_.x0 || x > region_.x1 || z < region_.z0 || z > region_.z1) return;
    const int tx = static_cast<int>((x - region_.x0) / tile_side_);
    const int tz = static_cast<int>((z - region_.z0) / tile_side_);
    ++cell_hit_[static_cast<std::size_t>(cell_of(tz * nx_ + tx))];
}

// Derive each map cell's phase from its counters. Called by the pump after
// every tile (publish_map) when the request asked for a map.
std::vector<MapCell> TileScheduler::map() const {
    std::vector<MapCell> out(static_cast<std::size_t>(mw_) * static_cast<std::size_t>(mh_));
    for (int c = 0; c < mw_ * mh_; ++c) {
        const int mx = c % mw_, mz = c / mw_;
        // How many tiles this cell holds (its width in tiles x height in tiles).
        const int tiles = (split(nx_, mw_, mx + 1) - split(nx_, mw_, mx)) *
                          (split(nz_, mh_, mz + 1) - split(nz_, mh_, mz));
        const std::size_t i = static_cast<std::size_t>(c);
        out[i].phase = cell_busy_[i]            ? MapCell::searching
                       : cell_done_[i] == tiles ? MapCell::done
                       : cell_done_[i] > 0      ? MapCell::partial
                                                : MapCell::pending;
        out[i].hit = cell_hit_[i] > 0;
    }
    return out;
}

// Checkpoint file format:
//   rokkdoxx-checkpoint 2 <fingerprint>
//   done 0-15 17 40-1200 ...
//   matches <count>
//   <x> <z> <orient_mask>        (repeated <count> times)
// The done line is a run-length list of finished tile indices. The matches
// section carries the job's matches at save time, so a resumed run
// doesn't lose (or need to re-find) matches from tiles it's about to skip as
// already-done. Any other version is rejected.
// The tile indices only make sense for the same tile grid, which is why the
// fingerprint (request_fingerprint) must match before anything is applied.
bool TileScheduler::load_checkpoint(const std::string& path, std::uint64_t fingerprint,
                                    std::vector<Match>& out_matches) {
    std::ifstream f(path);
    return f && read_checkpoint(f, fingerprint, out_matches);
}

bool TileScheduler::read_checkpoint(std::istream& f, std::uint64_t fingerprint,
                                    std::vector<Match>& out_matches) {
    out_matches.clear();
    std::string tag;
    int version = 0;
    std::uint64_t fp = 0;
    f >> tag >> version >> fp;
    if (tag != "rokkdoxx-checkpoint" || version != 2 || fp != fingerprint)
        return false;

    // Nothing is applied until the whole file has parsed, and a line counts
    // only if its newline was written: a file cut off anywhere (killed
    // mid-write) is rejected whole, so a resume never skips tiles whose
    // matches were lost -- the job starts from zero instead.
    std::string line;
    auto next_line = [&] { return std::getline(f, line) && !f.eof(); };
    std::getline(f, line);  // rest of the header line
    if (!next_line()) return false;
    std::vector<std::pair<int, int>> runs;
    {
        std::istringstream ls(line);
        std::string kw;
        ls >> kw;
        if (kw != "done") return false;
        std::string tok;
        while (ls >> tok) {
            auto dash = tok.find('-');
            if (dash == std::string::npos) {
                runs.push_back({std::atoi(tok.c_str()), std::atoi(tok.c_str())});
            } else {
                runs.push_back({std::atoi(tok.substr(0, dash).c_str()), std::atoi(tok.substr(dash + 1).c_str())});
            }
        }
    }

    if (!next_line()) return false;
    std::istringstream ms(line);
    std::string kw;
    long long count = 0;
    if (!(ms >> kw >> count) || kw != "matches") return false;
    for (long long i = 0; i < count; ++i) {
        int x = 0, z = 0, mask = 0;
        std::istringstream ls;
        if (next_line()) ls.str(line);
        if (!(ls >> x >> z >> mask)) {
            out_matches.clear();
            return false;
        }
        out_matches.push_back({x, z, static_cast<std::uint8_t>(mask)});
    }
    for (const auto& [a, b] : runs)
        for (int i = std::max(a, 0); i <= b && i < n_; ++i) mark_done(i);
    return true;
}

// Writes the format described above. Runs of consecutive done tile indices
// collapse to "a-b"; indices are row-major, so a finished k x k square in the
// middle of the grid costs k runs.
void TileScheduler::write_checkpoint(std::ostream& f, std::uint64_t fingerprint,
                                     const std::vector<Match>& matches) const {
    f << "rokkdoxx-checkpoint 2 " << fingerprint << "\ndone";
    int i = 0;
    while (i < n_) {
        if (!done_[static_cast<std::size_t>(i)]) {
            ++i;
            continue;
        }
        int j = i;
        while (j + 1 < n_ && done_[static_cast<std::size_t>(j + 1)]) ++j;
        if (i == j) f << " " << i;
        else f << " " << i << "-" << j;
        i = j + 1;
    }
    f << "\nmatches " << matches.size() << "\n";
    for (const Match& m : matches)
        f << m.x << " " << m.z << " " << static_cast<int>(m.orient_mask) << "\n";
}

// ==========================================================================
// SearchService
// ==========================================================================

// One job owns its own worker thread. `status` and `results` are guarded by
// `mu`; `cancel` is a plain atomic the run loop checks between tiles.
//
// Locking: find() takes and releases SearchService::mu_, then the caller takes
// Job::mu. The job thread only ever takes Job::mu, never mu_, so a poll()
// can't deadlock against a running job.
struct SearchService::Job {
    SearchRequest req;
    std::atomic<bool> cancel{false};  // set by cancel() / ~SearchService
    std::atomic<bool> pause{false};   // set by pause(); the pump idles while true

    mutable std::mutex mu;
    JobStatus status;            // what poll() returns (copied out)
    std::vector<Match> results;  // filled once, when the job ends
    std::string ckpt_text;  // see SearchRequest::keep_checkpoint

    std::thread th;  // runs SearchService::run(this); joined in ~SearchService
};

SearchService::SearchService(WorkerFactory factory) : factory_(std::move(factory)) {}

SearchService::~SearchService() {
    // Tell every job to stop, then wait for the threads.
    std::vector<Job*> live;
    {
        std::lock_guard<std::mutex> lk(mu_);
        for (auto& [id, j] : jobs_) {
            j->cancel.store(true);
            live.push_back(j.get());
        }
    }
    for (Job* j : live)
        if (j->th.joinable()) j->th.join();
}

// The thread is started while mu_ is held, which is fine: run() never takes
// mu_. Jobs are heap-allocated (unique_ptr) so `raw` stays valid while the
// map rehashes.
JobId SearchService::submit(const SearchRequest& req) {
    auto job = std::make_unique<Job>();
    std::lock_guard<std::mutex> lk(mu_);
    const JobId id = next_id_++;
    job->req = req;
    job->status.state = JobState::pending;
    job->status.candidates_total = req.region.candidates();
    Job* raw = job.get();
    jobs_.emplace(id, std::move(job));
    raw->th = std::thread([this, raw] { run(raw); });
    return id;
}

// Jobs are never erased, so the pointer stays valid after mu_ is released.
SearchService::Job* SearchService::find(JobId id) const {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = jobs_.find(id);
    return it == jobs_.end() ? nullptr : it->second.get();
}

JobStatus SearchService::poll(JobId id) const {
    const Job* j = find(id);
    if (!j) {
        JobStatus s;
        s.state = JobState::error;
        s.error = "no such job";
        return s;
    }
    std::lock_guard<std::mutex> jl(j->mu);
    return j->status;
}

std::vector<Match> SearchService::results(JobId id) const {
    const Job* j = find(id);
    if (!j) return {};
    std::lock_guard<std::mutex> jl(j->mu);
    return j->results;
}

void SearchService::cancel(JobId id) {
    if (Job* j = find(id)) j->cancel.store(true);
}

void SearchService::pause(JobId id, bool on) {
    if (Job* j = find(id)) j->pause.store(on);
}

std::string SearchService::checkpoint(JobId id) const {
    const Job* j = find(id);
    if (!j) return {};
    std::lock_guard<std::mutex> jl(j->mu);
    return j->ckpt_text;
}

// The body of one job's worker thread. noexcept: any failure is recorded in the
// job's status, never thrown out of the thread.
//
// Outline:
//   1. validate the request              (errors -> set_err, return)
//   2. build WorkerConfig, make + configure the Worker
//   3. build the TileScheduler, restore a checkpoint (text first, then file)
//   4. the pump: keep up to kDepth tiles in flight, drain the oldest, update
//      status / map / checkpoint after each; honour pause and cancel between
//      tiles
//   5. final checkpoint, results, terminal state
// Exceptions from anywhere (e.g. an OpenCL error while compiling or
// dispatching) land in the catch blocks at the end and become JobState::error.
void SearchService::run(Job* job) noexcept {
    using clock = std::chrono::steady_clock;
    const auto t0 = clock::now();

    auto set_err = [&](const std::string& msg) {
        std::lock_guard<std::mutex> jl(job->mu);
        job->status.state = JobState::error;
        job->status.error = msg;
    };

    try {
        const SearchRequest& req = job->req;
        if (!req.region.valid()) return set_err("empty region");
        if (req.plane_y < -64 || req.plane_y > -59)
            return set_err("plane y must be in [-64, -59]");
        if (req.pattern.w > kMaxDim || req.pattern.h > kMaxDim)
            return set_err("pattern is bigger than " + std::to_string(kMaxDim) + "x" + std::to_string(kMaxDim));
        if (req.pattern.knowns().empty()) return set_err("pattern has no known cells");
        // Past 2^25 the float rounding in Bedrock Edition's floor (rk_be_coord)
        // moves a column more than one block, which the GPU path does not allow for.
        if (req.edition == Edition::bedrock &&
            std::max({-req.region.x0, req.region.x1, -req.region.z0, req.region.z1}) > (1 << 25) - kMaxDim)
            return set_err("Bedrock Edition searches must stay within +/-33,554,400");

        // Turn the seed + plane into the handful of constants the worker needs.
        WorkerConfig cfg = worker_config(req.seed, req.plane_y, req.edition);
        cfg.knowns = req.pattern.knowns();
        cfg.all_orientations = req.all_orientations;
        cfg.match_cap = req.match_cap;

        // Making the worker opens the device; for OpenCL this compiles the
        // generic kernels (OpenclWorker's constructor), on this job's thread.
        auto worker = factory_();
        if (!worker) return set_err("no compute backend available");

        // A worker may insist on bigger tiles than the request asked for.
        // (The GPU's 16384.) The fingerprint must cover the side actually used:
        // the done list indexes this tile grid, so a GPU checkpoint must not
        // resume on the CPU or vice versa. Mixed in only when raised, so
        // CPU checkpoints stay valid.
        int tile_side = req.tile_side;
        if (const int pref = worker->preferred_tile_side()) tile_side = std::max(tile_side, pref);

        TileScheduler sched(req.region, tile_side, req.map_w, req.map_h);
        std::uint64_t fp = request_fingerprint(req);
        if (tile_side != req.tile_side) fp = mix(fp, static_cast<std::uint64_t>(tile_side));
        // The first req.match_cap matches, in arrival order. No dedup: every
        // origin is in exactly one tile, and the worker ORs its orientations.
        std::vector<Match> found;
        bool capped = false;
        auto keep = [&](const std::vector<Match>& m) {
            for (const Match& x : m) {
                if (found.size() < req.match_cap) found.push_back(x);
                else capped = true;
            }
        };
        auto sorted = [&] {  // by (z, then x)
            std::vector<Match> out = found;
            std::sort(out.begin(), out.end(), [](const Match& a, const Match& b) {
                return a.z != b.z ? a.z < b.z : a.x < b.x;
            });
            return out;
        };
        // Resume: in-memory text (TUI's `r` / --resume) wins over the file.
        // A checkpoint for a different request fails the fingerprint check
        // and is ignored, so the job just starts from zero.
        {
            std::vector<Match> restored;
            bool resumed_from = false;
            if (!req.resume_text.empty()) {
                std::istringstream in(req.resume_text);
                resumed_from = sched.read_checkpoint(in, fp, restored);
            }
            if (!resumed_from && !req.checkpoint_path.empty())
                resumed_from = sched.load_checkpoint(req.checkpoint_path, fp, restored);
            if (resumed_from) {
                keep(restored);
                for (const Match& m : restored) sched.mark_hit(m.x, m.z);
            }
        }

        // Copy the scheduler's progress map into the job status (if a front-end
        // asked for one) so poll() can show which areas are being searched.
        const bool want_map = req.map_w > 0 && req.map_h > 0;
        auto publish_map = [&] {
            if (!want_map) return;
            std::vector<MapCell> cells = sched.map();
            std::lock_guard<std::mutex> jl(job->mu);
            job->status.map_w = sched.map_w();
            job->status.map_h = sched.map_h();
            job->status.map = std::move(cells);
        };
        // Tiles a checkpoint already covered; the rate (and so the ETA) only
        // counts work done in this run.
        const long long resumed = sched.candidates_done();

        // Builds the SearchPlan; on the GPU also uploads the tables and
        // compiles the pattern-specialized match kernel (~200 ms first time).
        worker->configure(cfg);

        publish_map();
        {
            std::lock_guard<std::mutex> jl(job->mu);
            job->status.state = JobState::running;
        }

        // The pump: tiles are handed to the worker until the region is
        // covered or the job is cancelled. Status is refreshed after every
        // completed tile so a poller sees live progress. Up to two tiles are
        // in flight, so OpenclWorker's dispatch for tile N overlaps its
        // read-back for tile N-1 (other workers run each tile in begin_tile()).
        // Progress goes to the in-memory copy every 5 s and to the checkpoint
        // file every ~1 min of candidates (rate x 60 s). Both save on the first
        // tile (last_ckpt starts in the past, next_disk at 0).
        auto last_ckpt = clock::now() - std::chrono::seconds(10);
        long long next_disk = 0;
        auto save_progress = [&](const std::vector<Match>& matches, bool to_disk) {
            to_disk = to_disk && !req.checkpoint_path.empty();
            if (!to_disk && !req.keep_checkpoint) return;
            std::ostringstream text;
            sched.write_checkpoint(text, fp, matches);
            if (to_disk) {
                std::ofstream f(req.checkpoint_path, std::ios::trunc);
                if (f) f << text.str();
            }
            if (req.keep_checkpoint) {
                std::lock_guard<std::mutex> jl(job->mu);
                job->ckpt_text = text.str();
            }
            last_ckpt = clock::now();
        };
        double paused_total = 0.0;  // seconds spent paused: not counted in elapsed / rate

        // Bookkeeping for one *completed* tile -- called at drain time, which
        // lags dispatch order by up to one tile. That lag is fine: neither
        // TileScheduler::mark_done nor keep() cares about order, and
        // progress stays monotonic.
        bool stop = false;  // stop_at_first_match: no new tiles once one has a match
        // Only matches from THIS run can trigger stop_at_first_match: restored
        // ones were kept before the loop, so a resumed job carries on.
        auto on_tile_done = [&](const Tile& done, const std::vector<Match>& m) {
            if (req.stop_at_first_match && !m.empty()) stop = true;
            keep(m);
            for (const Match& mm : m) sched.mark_hit(mm.x, mm.z);
            sched.mark_done(done.index);

            const auto now = clock::now();
            const double elapsed = std::chrono::duration<double>(now - t0).count() - paused_total;
            const long long cdone = sched.candidates_done();
            const double rate = elapsed > 0 ? static_cast<double>(cdone - resumed) / elapsed : 0.0;
            {
                std::lock_guard<std::mutex> jl(job->mu);
                job->status.candidates_done = cdone;
                job->status.progress =
                    job->status.candidates_total > 0
                        ? static_cast<double>(cdone) / static_cast<double>(job->status.candidates_total)
                        : 1.0;
                job->status.matches = found.size();
                job->status.elapsed_s = elapsed;
                job->status.rate = rate;
                job->status.eta_s =
                    job->status.rate > 0
                        ? static_cast<double>(job->status.candidates_total - cdone) / job->status.rate
                        : 0.0;
                job->status.truncated = capped || worker->truncated();
            }
            publish_map();

            const bool disk_due = !req.checkpoint_path.empty() && cdone >= next_disk;
            if (disk_due) next_disk = cdone + static_cast<long long>(rate * 60.0);
            if (disk_due || (req.keep_checkpoint && std::chrono::duration<double>(now - last_ckpt).count() > 5.0))
                save_progress(sorted(), disk_due);
        };

        // Idle until resumed (or cancelled). Entering the pause is the moment to
        // save: nothing is in flight, so the saved progress is exact.
        auto wait_while_paused = [&] {
            const auto since = clock::now();
            {
                std::lock_guard<std::mutex> jl(job->mu);
                job->status.elapsed_s = std::chrono::duration<double>(since - t0).count() - paused_total;
                job->status.state = JobState::paused;
            }
            save_progress(sorted(), true);
            while (job->pause.load() && !job->cancel.load())
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            paused_total += std::chrono::duration<double>(clock::now() - since).count();
            std::lock_guard<std::mutex> jl(job->mu);
            job->status.state = JobState::running;
        };

        // kDepth must not exceed OpenclWorker's kSlots (2): begin_tile() reuses
        // slots round-robin, so a third outstanding tile would overwrite the
        // buffers of one not yet read back.
        constexpr int kDepth = 2;
        std::deque<Tile> inflight;  // begun, not yet drained; front = oldest
        Tile t;
        bool more = true;
        while (true) {
            // Admit new tiles up to the pipeline depth. Cancellation and pausing
            // only stop *new* admissions -- anything already begun always gets
            // drained below, so the loop never exits with GPU work still
            // outstanding.
            const auto may_admit = [&] { return more && !stop && !job->cancel.load(); };
            while (may_admit() && !job->pause.load() && static_cast<int>(inflight.size()) < kDepth) {
                if (!sched.next(t)) {
                    more = false;
                    break;
                }
                publish_map();  // the tile is now "searching" (a CPU worker computes it inside begin_tile)
                worker->begin_tile(t);
                inflight.push_back(t);
            }
            if (inflight.empty()) {
                if (may_admit() && job->pause.load()) {  // drained: now actually pause
                    wait_while_paused();
                    continue;
                }
                break;
            }
            // Blocks until the oldest in-flight tile is finished (FIFO pairing
            // with begin_tile, see Worker).
            std::vector<Match> m = worker->end_tile();
            on_tile_done(inflight.front(), m);
            inflight.pop_front();
        }

        // Out of the loop: the region is covered, or cancel / stop_at_first
        // stopped admissions and everything in flight has drained.

        std::vector<Match> final_matches = sorted();
        const bool autosave_done = !job->cancel.load() && req.checkpoint_path == kAutosavePath;
        save_progress(final_matches, !autosave_done);
        if (autosave_done) std::remove(kAutosavePath);  // a finished search has nothing to resume

        std::lock_guard<std::mutex> jl(job->mu);
        job->results = std::move(final_matches);
        job->status.matches = job->results.size();
        job->status.elapsed_s = std::chrono::duration<double>(clock::now() - t0).count() - paused_total;
        job->status.state = job->cancel.load() ? JobState::cancelled : JobState::done;
        job->status.eta_s = 0.0;
        const bool early = stop && sched.done_count() < sched.tile_count();  // a match on the last tile cut nothing short
        job->status.stopped_early = early && job->status.state == JobState::done;
        if (job->status.state == JobState::done && !early) job->status.progress = 1.0;
    } catch (const std::exception& e) {
        set_err(e.what());
    } catch (...) {
        set_err("unknown error");
    }
}

}  // namespace rokkdoxx::svc
