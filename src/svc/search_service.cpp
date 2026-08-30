#include "search_service.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
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

TileScheduler::TileScheduler(Region region, int tile_side, int max_tiles)
    : region_(region), tile_side_(tile_side < 1 ? 1 : tile_side) {
    const long long spanx = region_.x1 - region_.x0 + 1;
    const long long spanz = region_.z1 - region_.z0 + 1;
    // Grow the tile until nx*nz fits under the cap (or the tile is absurdly
    // large). This keeps `done_` small even for a whole-world sweep.
    for (;;) {
        const long long gx = (spanx + tile_side_ - 1) / tile_side_;
        const long long gz = (spanz + tile_side_ - 1) / tile_side_;
        if (gx * gz <= max_tiles || tile_side_ >= (1 << 26)) {
            nx_ = static_cast<int>(gx < 1 ? 1 : gx);
            nz_ = static_cast<int>(gz < 1 ? 1 : gz);
            break;
        }
        tile_side_ *= 2;
    }
    n_ = nx_ * nz_;
    done_.assign(static_cast<std::size_t>(n_), 0);
}

Tile TileScheduler::tile_at(int index) const {
    const int tx = index % nx_;
    const int tz = index / nx_;
    Tile t;
    t.index = index;
    t.x0 = region_.x0 + static_cast<long long>(tx) * tile_side_;
    t.z0 = region_.z0 + static_cast<long long>(tz) * tile_side_;
    // The last column / row is clipped to the region edge.
    const long xend = (tx == nx_ - 1) ? region_.x1 : t.x0 + tile_side_ - 1;
    const long zend = (tz == nz_ - 1) ? region_.z1 : t.z0 + tile_side_ - 1;
    t.w = static_cast<int>(xend - t.x0 + 1);
    t.h = static_cast<int>(zend - t.z0 + 1);
    return t;
}

bool TileScheduler::next(Tile& out) {
    std::lock_guard<std::mutex> lk(mu_);
    while (cursor_ < n_ && done_[static_cast<std::size_t>(cursor_)]) ++cursor_;
    if (cursor_ >= n_) return false;
    out = tile_at(cursor_);
    ++cursor_;
    return true;
}

void TileScheduler::mark_done(const Tile& tile) {
    std::lock_guard<std::mutex> lk(mu_);
    const int index = tile.index;
    if (index >= 0 && index < n_ && !done_[static_cast<std::size_t>(index)]) {
        done_[static_cast<std::size_t>(index)] = 1;
        ++done_count_;
        candidates_done_ += static_cast<long long>(tile.w) * tile.h;
    }
}

int TileScheduler::done_count() const {
    std::lock_guard<std::mutex> lk(mu_);
    return done_count_;
}

long long TileScheduler::candidates_done() const {
    std::lock_guard<std::mutex> lk(mu_);
    return candidates_done_;
}

// Checkpoint file format (one line of header, one line of data):
//   rokkdoxx-checkpoint 1 <fingerprint>
//   done 0-15 17 40-1200 ...
// The data line is a run-length list of finished tile indices.
bool TileScheduler::load_checkpoint(const std::string& path, std::uint64_t fingerprint) {
    std::ifstream f(path);
    if (!f) return false;
    std::string tag;
    int version = 0;
    std::uint64_t fp = 0;
    f >> tag >> version >> fp;
    if (tag != "rokkdoxx-checkpoint" || version != 1 || fp != fingerprint) return false;
    std::string kw;
    f >> kw;  // "done"
    if (kw != "done") return false;
    std::lock_guard<std::mutex> lk(mu_);
    std::string tok;
    while (f >> tok) {
        auto dash = tok.find('-');
        int a, b;
        if (dash == std::string::npos) {
            a = b = std::atoi(tok.c_str());
        } else {
            a = std::atoi(tok.substr(0, dash).c_str());
            b = std::atoi(tok.substr(dash + 1).c_str());
        }
        for (int i = a; i <= b && i < n_; ++i)
            if (i >= 0 && !done_[static_cast<std::size_t>(i)]) {
                done_[static_cast<std::size_t>(i)] = 1;
                ++done_count_;
                const Tile t = tile_at(i);
                candidates_done_ += static_cast<long long>(t.w) * t.h;
            }
    }
    return true;
}

void TileScheduler::save_checkpoint(const std::string& path, std::uint64_t fingerprint) const {
    std::lock_guard<std::mutex> lk(mu_);
    std::ofstream f(path, std::ios::trunc);
    if (!f) return;
    f << "rokkdoxx-checkpoint 1 " << fingerprint << "\ndone";
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
    f << "\n";
}

// ==========================================================================
// ResultSink
// ==========================================================================

void ResultSink::add(const std::vector<Match>& tile_matches) {
    std::lock_guard<std::mutex> lk(mu_);
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

std::uint64_t ResultSink::count() const {
    std::lock_guard<std::mutex> lk(mu_);
    return by_pos_.size();
}

bool ResultSink::truncated() const {
    std::lock_guard<std::mutex> lk(mu_);
    return truncated_;
}

std::vector<Match> ResultSink::snapshot() const {
    std::lock_guard<std::mutex> lk(mu_);
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

    mutable std::mutex mu;
    JobStatus status;
    std::vector<Match> results;

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

        TileScheduler sched(req.region, tile_side);
        const std::uint64_t fp = request_fingerprint(req);
        if (!req.checkpoint_path.empty()) sched.load_checkpoint(req.checkpoint_path, fp);

        ResultSink sink(req.match_cap);
        worker->configure(cfg);

        {
            std::lock_guard<std::mutex> jl(job->mu);
            job->status.state = JobState::running;
        }

        // The pump: one tile at a time until the region is covered or the job
        // is cancelled. Status is refreshed after every tile so a poller sees
        // live progress.
        auto last_ckpt = clock::now();
        Tile tile;
        while (!job->cancel.load() && sched.next(tile)) {
            std::vector<Match> m = worker->run_tile(tile);
            sink.add(m);
            sched.mark_done(tile);

            const auto now = clock::now();
            const double elapsed = std::chrono::duration<double>(now - t0).count();
            const long long done = sched.candidates_done();
            {
                std::lock_guard<std::mutex> jl(job->mu);
                job->status.candidates_done = done;
                job->status.progress =
                    job->status.candidates_total > 0
                        ? static_cast<double>(done) / static_cast<double>(job->status.candidates_total)
                        : 1.0;
                job->status.matches = sink.count();
                job->status.elapsed_s = elapsed;
                job->status.rate = elapsed > 0 ? static_cast<double>(done) / elapsed : 0.0;
                job->status.truncated = sink.truncated() || worker->truncated();
            }

            if (!req.checkpoint_path.empty() &&
                std::chrono::duration<double>(now - last_ckpt).count() > 5.0) {
                sched.save_checkpoint(req.checkpoint_path, fp);
                last_ckpt = now;
            }
        }

        if (!req.checkpoint_path.empty()) sched.save_checkpoint(req.checkpoint_path, fp);

        std::vector<Match> final_matches = sink.snapshot();
        std::lock_guard<std::mutex> jl(job->mu);
        job->results = std::move(final_matches);
        job->status.matches = job->results.size();
        job->status.elapsed_s = std::chrono::duration<double>(clock::now() - t0).count();
        job->status.state = job->cancel.load() ? JobState::cancelled : JobState::done;
        if (job->status.state == JobState::done) job->status.progress = 1.0;
    } catch (const std::exception& e) {
        set_err(e.what());
    } catch (...) {
        set_err("unknown error");
    }
}

}  // namespace rokkdoxx::svc
