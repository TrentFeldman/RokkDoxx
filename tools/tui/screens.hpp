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

enum class Screen { params, grid, result, prompt };

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

    // text prompt
    std::string prompt_label, prompt_buf;
    Screen prompt_return = Screen::grid;
    void (*prompt_done)(App&) = nullptr;
};

// Create the client for `id` ("" / "auto", "cpu", "opencl:N") and remember
// it. False + `err` if the backend can't be opened (the old client is kept).
bool select_backend(App& app, const std::string& id, std::string& err);

void draw(App& app, Frame& fr);

// Handle one key. Returns false when the user asked to quit.
bool handle_key(App& app, int key);

// Poll the running job, if any; collects results once it ends.
void refresh_job(App& app);

// Cancel the running job, if any (used on quit).
void cancel_job(App& app);

}  // namespace rokkdoxx::tui
