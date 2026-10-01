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

#include "gen/bedrock.hpp"

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

std::uint64_t request_fingerprint(const SearchRequest& req) {
    std::uint64_t h = 0xcbf29ce484222325ULL;  // FNV offset basis
    h = mix(h, static_cast<std::uint64_t>(req.seed));
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
int split(int n, int cells, int m) {
    return static_cast<int>(static_cast<long long>(m) * n / cells);
}
int cell_of_tile(int n, int cells, int t) {
    return static_cast<int>(((static_cast<long long>(t) + 1) * cells - 1) / n);
}
}  // namespace

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

int TileScheduler::cell_of(int tile_index) const {
    return cell_of_tile(nz_, mh_, tile_index / nx_) * mw_ + cell_of_tile(nx_, mw_, tile_index % nx_);
}

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

void TileScheduler::mark_done(const Tile& tile) { finish(tile.index); }

void TileScheduler::finish(int index) {
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

void TileScheduler::mark_hit(std::int64_t x, std::int64_t z) {
    if (x < region_.x0 || x > region_.x1 || z < region_.z0 || z > region_.z1) return;
    const int tx = static_cast<int>((x - region_.x0) / tile_side_);
    const int tz = static_cast<int>((z - region_.z0) / tile_side_);
    ++cell_hit_[static_cast<std::size_t>(cell_of(tz * nx_ + tx))];
}

std::vector<MapCell> TileScheduler::map() const {
    std::vector<MapCell> out(static_cast<std::size_t>(mw_) * static_cast<std::size_t>(mh_));
    for (int c = 0; c < mw_ * mh_; ++c) {
        const int mx = c % mw_, mz = c / mw_;
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
// section carries whatever ResultSink held at save time, so a resumed run
// doesn't lose (or need to re-find) matches from tiles it's about to skip as
// already-done. Any other version is rejected.
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

    std::string line;
    std::getline(f, line);              // rest of the header line
    if (!std::getline(f, line)) return false;
    {
        std::istringstream ls(line);
        std::string kw;
        ls >> kw;
        if (kw != "done") return false;
        std::string tok;
        while (ls >> tok) {
            auto dash = tok.find('-');
            int a, b;
            if (dash == std::string::npos) {
                a = b = std::atoi(tok.c_str());
            } else {
                a = std::atoi(tok.substr(0, dash).c_str());
                b = std::atoi(tok.substr(dash + 1).c_str());
            }
            for (int i = std::max(a, 0); i <= b && i < n_; ++i) finish(i);
        }
    }

    if (std::getline(f, line)) {
        std::istringstream ms(line);
        std::string kw;
        long long count = 0;
        ms >> kw >> count;
        if (kw == "matches" && count > 0) {
            for (long long i = 0; i < count && std::getline(f, line); ++i) {
                std::istringstream ls(line);
                int x = 0, z = 0, mask = 0;
                if (ls >> x >> z >> mask)
                    out_matches.push_back({x, z, static_cast<std::uint8_t>(mask)});
            }
        }
    }
    return true;
}

void TileScheduler::save_checkpoint(const std::string& path, std::uint64_t fingerprint,
                                    const std::vector<Match>& matches) const {
    std::ofstream f(path, std::ios::trunc);
    if (f) write_checkpoint(f, fingerprint, matches);
}

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
// ResultSink
// ==========================================================================

void ResultSink::add(const std::vector<Match>& tile_matches) {
    for (const Match& m : tile_matches) {
        // Same origin from another tile/orientation: merge the masks.
        auto [it, inserted] = by_pos_.try_emplace(key(m.x, m.z), m.orient_mask);
        if (!inserted) {
            it->second |= m.orient_mask;
        } else if (by_pos_.size() > cap_) {
            by_pos_.erase(it);
            truncated_ = true;
        }
    }
}

std::vector<Match> ResultSink::snapshot() const {
    std::vector<Match> out;
    out.reserve(by_pos_.size());
    for (const auto& [k, mask] : by_pos_) {
        const int x = static_cast<int>(static_cast<std::uint32_t>(k >> 32));
        const int z = static_cast<int>(static_cast<std::uint32_t>(k));
        out.push_back({x, z, mask});
    }
    std::sort(out.begin(), out.end(), [](const Match& a, const Match& b) {
        return a.z != b.z ? a.z < b.z : a.x < b.x;
    });
    return out;
}

// ==========================================================================
// SearchService
// ==========================================================================

// One job owns its own worker thread. `status` and `results` are guarded by
// `mu`; `cancel` is a plain atomic the run loop checks between tiles.
struct SearchService::Job {
    JobId id = 0;
    SearchRequest req;
    std::atomic<bool> cancel{false};
    std::atomic<bool> pause{false};

    mutable std::mutex mu;
    JobStatus status;
    std::vector<Match> results;
    std::string ckpt_text;  // see SearchRequest::keep_checkpoint

    std::thread th;
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

std::string SearchService::backend_name() const {
    auto w = factory_();
    return w ? w->name() : "none";
}

JobId SearchService::submit(const SearchRequest& req) {
    auto job = std::make_unique<Job>();
    std::lock_guard<std::mutex> lk(mu_);
    const JobId id = next_id_++;
    job->id = id;
    job->req = req;
    job->status.state = JobState::pending;
    job->status.candidates_total = req.region.candidates();
    Job* raw = job.get();
    jobs_.emplace(id, std::move(job));
    raw->th = std::thread([this, raw] { run(raw); });
    return id;
}

JobStatus SearchService::poll(JobId id) const {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = jobs_.find(id);
    if (it == jobs_.end()) {
        JobStatus s;
        s.state = JobState::error;
        s.error = "no such job";
        return s;
    }
    std::lock_guard<std::mutex> jl(it->second->mu);
    return it->second->status;
}

std::vector<Match> SearchService::results(JobId id) const {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = jobs_.find(id);
    if (it == jobs_.end()) return {};
    std::lock_guard<std::mutex> jl(it->second->mu);
    return it->second->results;
}

void SearchService::cancel(JobId id) {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = jobs_.find(id);
    if (it != jobs_.end()) it->second->cancel.store(true);
}

void SearchService::pause(JobId id, bool on) {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = jobs_.find(id);
    if (it != jobs_.end()) it->second->pause.store(on);
}

std::string SearchService::checkpoint(JobId id) const {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = jobs_.find(id);
    if (it == jobs_.end()) return {};
    std::lock_guard<std::mutex> jl(it->second->mu);
    return it->second->ckpt_text;
}

// The body of one job's worker thread. noexcept: any failure is recorded in the
// job's status, never thrown out of the thread.
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
        if (req.pattern.knowns().empty()) return set_err("pattern has no known cells");

        // Turn the seed + plane into the handful of constants the worker needs.
        BedrockGenerator gen(req.seed);
        WorkerConfig cfg;
        cfg.derived_lo = gen.derived_lo();
        cfg.derived_hi = gen.derived_hi();
        cfg.plane_y = req.plane_y;
        cfg.threshold = gen.threshold(req.plane_y);
        cfg.knowns = req.pattern.knowns();
        cfg.all_orientations = req.all_orientations;
        cfg.match_cap = req.match_cap;

        auto worker = factory_();
        if (!worker) return set_err("no compute backend available");

        // A worker may insist on bigger tiles than the request asked for.
        int tile_side = req.tile_side;
        if (const int pref = worker->preferred_tile_side()) tile_side = std::max(tile_side, pref);

        TileScheduler sched(req.region, tile_side, req.map_w, req.map_h);
        const std::uint64_t fp = request_fingerprint(req);
        ResultSink sink(req.match_cap);
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
                sink.add(restored);
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
        // Progress goes to the checkpoint file and/or the in-memory copy. The
        // first tile saves at once (last_ckpt starts in the past), then every 5 s.
        auto last_ckpt = clock::now() - std::chrono::seconds(10);
        auto save_progress = [&](const std::vector<Match>& matches) {
            if (req.checkpoint_path.empty() && !req.keep_checkpoint) return;
            std::ostringstream text;
            sched.write_checkpoint(text, fp, matches);
            if (!req.checkpoint_path.empty()) {
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
        // lags dispatch order by up to one tile. That
        // lag is fine: TileScheduler::mark_done/ResultSink::add are already
        // idempotent/order-independent, and progress stays monotonic.
        bool stop = false;  // stop_at_first_match: no new tiles once one has a match
        auto on_tile_done = [&](const Tile& done, const std::vector<Match>& m) {
            if (req.stop_at_first_match && !m.empty()) stop = true;
            sink.add(m);
            for (const Match& mm : m) sched.mark_hit(mm.x, mm.z);
            sched.mark_done(done);

            const auto now = clock::now();
            const double elapsed = std::chrono::duration<double>(now - t0).count() - paused_total;
            const long long cdone = sched.candidates_done();
            {
                std::lock_guard<std::mutex> jl(job->mu);
                job->status.candidates_done = cdone;
                job->status.progress =
                    job->status.candidates_total > 0
                        ? static_cast<double>(cdone) / static_cast<double>(job->status.candidates_total)
                        : 1.0;
                job->status.matches = sink.count();
                job->status.elapsed_s = elapsed;
                job->status.rate = elapsed > 0 ? static_cast<double>(cdone - resumed) / elapsed : 0.0;
                job->status.eta_s =
                    job->status.rate > 0
                        ? static_cast<double>(job->status.candidates_total - cdone) / job->status.rate
                        : 0.0;
                job->status.truncated = sink.truncated() || worker->truncated();
            }
            publish_map();

            if (std::chrono::duration<double>(now - last_ckpt).count() > 5.0) save_progress(sink.snapshot());
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
            save_progress(sink.snapshot());
            while (job->pause.load() && !job->cancel.load())
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            paused_total += std::chrono::duration<double>(clock::now() - since).count();
            std::lock_guard<std::mutex> jl(job->mu);
            job->status.state = JobState::running;
        };

        constexpr int kDepth = 2;
        std::deque<Tile> inflight;
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
            std::vector<Match> m = worker->end_tile();
            on_tile_done(inflight.front(), m);
            inflight.pop_front();
        }

        std::vector<Match> final_matches = sink.snapshot();
        save_progress(final_matches);

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
