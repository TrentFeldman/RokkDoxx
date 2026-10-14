// rokktui's screens: parameters, pattern grid, search/results, and a one-line
// text prompt. Each screen is a draw function (into a Frame) and a key handler.
//
// The search is reached only through svc::SearchClient (+ list_backends() to
// name the devices), so nothing here depends on how the search is computed.
//
// Responsibilities: App state beyond the Model, drawing, key handling, and
// driving a job (submit / poll / cancel / results).
// Not this file's job: the terminal (term.*), the diffing (frame.*), or the
// Model's pure logic (model.*).
#pragma once

#include <memory>
#include <string>
#include <vector>

#include "frame.hpp"
#include "model.hpp"
#include "svc/client.hpp"
#include "svc/workers.hpp"

namespace rokkdoxx::tui {

// Which screen is up. Transitions (all in screens.cpp's handle_* functions):
//   params --Enter--> grid --Enter--> result (start_search)
//   grid --Tab--> params      result --Enter/Esc/q, once stopped--> grid
//   params `L`, grid `S`, result `S`/`s` --> prompt --Enter/Esc--> back
//   quit: q / Esc on params, q on grid (never while a search runs)
enum class Screen { params, grid, result, prompt };

// All of rokktui's state: the Model being edited, UI cursors, the search
// client and the job being shown, and the map animation bookkeeping. One App
// lives in main() and is passed by reference to draw / handle_key /
// refresh_job each tick. Tests build Apps directly.
struct App {
    Model m;
    Screen screen = Screen::params;
    int field = 0;              // params: selected field
    int gx = 0, gy = 0;         // grid: cursor
    int grid_top = 0, grid_left = 0;  // grid: viewport scroll
    std::string status;

    // compute backend
    std::vector<svc::BackendInfo> backends;
    int backend_idx = -1;       // index into backends; -1 = "auto" (not listed)
    std::unique_ptr<svc::SearchClient> client;
    std::string backend_label;

    // the current / last job
    svc::JobId job = 0;
    bool job_running = false;
    svc::JobStatus jst;
    std::vector<svc::Match> matches;
    int result_scroll = 0;
    int result_rows = 10;       // list height at the last draw (for PgUp/PgDn)

    // The live search map: a grid of '#', one per area of the search region,
    // flashing as each area is searched. All animation is driven by `now_ms`
    // (set by the main loop), so nothing in here reads a clock.
    long long now_ms = 0;
    int term_cols = 80, term_rows = 24;   // the terminal at the last draw
    int map_side = 0;                     // cells per side asked of the service; 0 = no map
    svc::Region job_region;               // what the job searches (for the map caption)
    std::vector<svc::MapCell> map_prev;   // last polled map, to spot cells that just changed
    std::vector<long long> phase_since;   // per cell: now_ms when its phase last changed
    std::vector<long long> hit_since;     // per cell: now_ms when its first match was seen
    long long finished_at_ms = 0;         // now_ms when the job ended
    int map_toggle = -1;                  // after the job ends: -1 automatic, 0 hidden, 1 shown

    // pause / save
    bool want_pause = false;              // `p` was pressed: the job is pausing or paused
    std::string session_path;             // the last session saved or resumed (the save prompt's default)

    // text prompt (see begin_prompt in screens.cpp): what to show, what was
    // typed, where to go back to, and what to do with the text on Enter.
    std::string prompt_label, prompt_buf;
    Screen prompt_return = Screen::grid;
    void (*prompt_done)(App&) = nullptr;
};

// Create the client for `id` ("" / "auto", "cpu", "dedicated", "integrated",
// "opencl:N") and remember it. False + `err` if the backend can't be opened
// (the old client is kept). Replacing the client while a job runs would end
// that job; the backend field is only reachable on the params screen.
bool select_backend(App& app, const std::string& id, std::string& err);

// Draw the current screen into `fr`. Not const: it records the terminal size
// (term_cols/rows, used to size the next search's map) and the result list's
// page height.
void draw(App& app, Frame& fr);

// Handle one key. Returns false when the user asked to quit.
bool handle_key(App& app, int key);

// Continue a session saved with `s` on the search screen (a pattern file with a
// `checkpoint` line): load it and start searching from where it left off.
// False + `err` if it is not a session or its progress file is missing.
bool resume_session(App& app, const std::string& path, std::string& err);

// Poll the running job, if any; collects results once it ends.
void refresh_job(App& app);

// Cancel the running job, if any (used on quit).
void cancel_job(App& app);

}  // namespace rokkdoxx::tui
