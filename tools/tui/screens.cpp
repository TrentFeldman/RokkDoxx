// rokktui's screens. Layout of this file:
//   colours + small helpers
//   parameters screen     draw_params / handle_params
//   pattern grid screen   draw_grid / handle_grid
//   search map            pick_map_side, track_map, cell_style, draw_map
//   search / results      start_search, draw_result / handle_result, saving
//   text prompt           draw_prompt / handle_prompt
//   public entry points   select_backend, resume_session, draw, handle_key,
//                         refresh_job, cancel_job (declared in screens.hpp)
// Every draw_* writes rows into a Frame; every handle_* takes one Key and
// returns false only to quit. Nothing here blocks: the search is polled by
// refresh_job once per tick.
#include "screens.hpp"

#include <algorithm>
#include <cstdio>
#include <exception>
#include <fstream>

namespace rokkdoxx::tui {

namespace {

// ANSI SGR colour codes (ESC [ ... m). kRst must follow every coloured run.
const char* kDim = "\x1b[2m";
const char* kRst = "\x1b[0m";
const char* kInv = "\x1b[7m";
const char* kBed = "\x1b[1;33m";
const char* kAir = "\x1b[36m";
const char* kHi = "\x1b[1;32m";

std::string hi(const std::string& s) { return std::string(kHi) + s + kRst; }
std::string dim(const std::string& s) { return std::string(kDim) + s + kRst; }

void status_line(const App& app, Frame& fr) {
    if (app.status.empty()) return;
    fr.line();
    fr.line("  " + app.status);
}

// ---------------------------------------------------------------------------
// splash
// ---------------------------------------------------------------------------

// The start-up banner, kSplashNeedCols wide (UTF-8 spelled as escapes, like
// the other non-ASCII strings here). The floor under the letters is real: Java
// seed 1911, x 0..78, y -60..-64.
const char* const kSplash[] = {
    "                     \xc2\xb7    \\     \\     |     /     /    \xc2\xb7",
    "                      \xe2\x96\x91\xe2\x96\x91\xe2\x96\x92\xe2\x96\x92\xe2\x96\x93\xe2\x96\x93\xe2\x96\x88\xe2\x96\x88\xe2\x96\x84\xe2\x96\x84\xe2\x96\x84\xe2\x96\x84\xe2\x96\x84\xe2\x96\x84\xe2\x96\x84\xe2\x96\x84\xe2\x96\x84\xe2\x96\x84\xe2\x96\x84\xe2\x96\x84\xe2\x96\x84\xe2\x96\x84\xe2\x96\x84\xe2\x96\x84\xe2\x96\x84\xe2\x96\x88\xe2\x96\x88\xe2\x96\x93\xe2\x96\x93\xe2\x96\x92\xe2\x96\x92\xe2\x96\x91\xe2\x96\x91",
    "                   \xce\x98\xe2\x95\x90\xe2\x95\xa4\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\xa7\xe2\x95\x90\xe2\x96\x93\xe2\x96\x93\xe2\x96\x80\xe2\x96\x80\xe2\x96\x80\xe2\x96\x91\xe2\x96\x91\xe2\x96\x91\xe2\x96\x91\xe2\x96\x91\xe2\x96\x91\xe2\x96\x91\xe2\x96\x91\xe2\x96\x91\xe2\x96\x91\xe2\x96\x91\xe2\x96\x91\xe2\x96\x91\xe2\x96\x80\xe2\x96\x80\xe2\x96\x80\xe2\x96\x93\xe2\x96\x93\xe2\x95\x90\xe2\x95\xa7\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\xa4\xe2\x95\x90\xce\x98",
    "\xce\xa9\xe2\x95\x9e\xe2\x95\x90\xe2\x95\xa4\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\xa4\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\xa4\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\xa4\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\xa4\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\xa4\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\xa4\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\xa4\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\xa4\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\xa4\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\xa4\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\xa4\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\xa4\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\xa4\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\xa4\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\xa1\xce\xa9",
    "      \xe2\x96\x88\xe2\x96\x88\xe2\x96\x88\xe2\x96\x88\xe2\x96\x88\xe2\x96\x88\xe2\x96\x84\xe2\x96\x84           \xe2\x96\x88\xe2\x96\x88\xe2\x96\x88      \xe2\x96\x88\xe2\x96\x88\xe2\x96\x88      \xe2\x96\x88\xe2\x96\x88\xe2\x96\x88\xe2\x96\x88\xe2\x96\x88\xe2\x96\x88\xe2\x96\x84\xe2\x96\x84",
    "     \xe2\x96\x84\xe2\x96\x88\xe2\x96\x88\xe2\x96\x80\xe2\x96\x80\xe2\x96\x80\xe2\x96\x88\xe2\x96\x88\xe2\x96\x88\xe2\x96\x80         \xe2\x96\x84\xe2\x96\x88\xe2\x96\x88\xe2\x96\x80     \xe2\x96\x84\xe2\x96\x88\xe2\x96\x88\xe2\x96\x80     \xe2\x96\x84\xe2\x96\x88\xe2\x96\x88\xe2\x96\x80\xe2\x96\x80\xe2\x96\x80\xe2\x96\x88\xe2\x96\x88\xe2\x96\x88\xe2\x96\x80",
    "     \xe2\x96\x88\xe2\x96\x88\xe2\x96\x88   \xe2\x96\x88\xe2\x96\x88\xe2\x96\x88          \xe2\x96\x88\xe2\x96\x88\xe2\x96\x88      \xe2\x96\x88\xe2\x96\x88\xe2\x96\x88      \xe2\x96\x88\xe2\x96\x88\xe2\x96\x88   \xe2\x96\x88\xe2\x96\x88\xe2\x96\x88",
    "    \xe2\x96\x88\xe2\x96\x88\xe2\x96\x88\xe2\x96\x88\xe2\x96\x88\xe2\x96\x88\xe2\x96\x88\xe2\x96\x88\xe2\x96\x80  \xe2\x96\x84\xe2\x96\x88\xe2\x96\x88\xe2\x96\x88\xe2\x96\x88\xe2\x96\x84  \xe2\x96\x88\xe2\x96\x88\xe2\x96\x88  \xe2\x96\x88\xe2\x96\x88\xe2\x96\x88 \xe2\x96\x88\xe2\x96\x88\xe2\x96\x88  \xe2\x96\x88\xe2\x96\x88\xe2\x96\x88 \xe2\x96\x88\xe2\x96\x88\xe2\x96\x88   \xe2\x96\x88\xe2\x96\x88\xe2\x96\x88  \xe2\x96\x84\xe2\x96\x88\xe2\x96\x88\xe2\x96\x88\xe2\x96\x88\xe2\x96\x84  \xe2\x96\x88\xe2\x96\x88\xe2\x96\x88  \xe2\x96\x88\xe2\x96\x88\xe2\x96\x88 \xe2\x96\x88\xe2\x96\x88\xe2\x96\x88  \xe2\x96\x88\xe2\x96\x88\xe2\x96\x88",
    "   \xe2\x96\x84\xe2\x96\x88\xe2\x96\x88\xe2\x96\x80\xe2\x96\x88\xe2\x96\x88\xe2\x96\x88\xe2\x96\x80  \xe2\x96\x84\xe2\x96\x88\xe2\x96\x88\xe2\x96\x80\xe2\x96\x80\xe2\x96\x88\xe2\x96\x88\xe2\x96\x88\xe2\x96\x80\xe2\x96\x84\xe2\x96\x88\xe2\x96\x88\xe2\x96\x88\xe2\x96\x84\xe2\x96\x88\xe2\x96\x80\xe2\x96\x80 \xe2\x96\x84\xe2\x96\x88\xe2\x96\x88\xe2\x96\x88\xe2\x96\x84\xe2\x96\x88\xe2\x96\x80\xe2\x96\x80 \xe2\x96\x84\xe2\x96\x88\xe2\x96\x88\xe2\x96\x80  \xe2\x96\x84\xe2\x96\x88\xe2\x96\x88\xe2\x96\x80\xe2\x96\x84\xe2\x96\x88\xe2\x96\x88\xe2\x96\x80\xe2\x96\x80\xe2\x96\x88\xe2\x96\x88\xe2\x96\x88\xe2\x96\x80 \xe2\x96\x88\xe2\x96\x88\xe2\x96\x88\xe2\x96\x84\xe2\x96\x84\xe2\x96\x88\xe2\x96\x80\xe2\x96\x80 \xe2\x96\x88\xe2\x96\x88\xe2\x96\x88\xe2\x96\x84\xe2\x96\x84\xe2\x96\x88\xe2\x96\x80\xe2\x96\x80",
    "   \xe2\x96\x88\xe2\x96\x88\xe2\x96\x88  \xe2\x96\x88\xe2\x96\x88\xe2\x96\x88  \xe2\x96\x88\xe2\x96\x88\xe2\x96\x88  \xe2\x96\x88\xe2\x96\x88\xe2\x96\x88 \xe2\x96\x88\xe2\x96\x88\xe2\x96\x88\xe2\x96\x88\xe2\x96\x88    \xe2\x96\x88\xe2\x96\x88\xe2\x96\x88\xe2\x96\x88\xe2\x96\x88    \xe2\x96\x88\xe2\x96\x88\xe2\x96\x88   \xe2\x96\x88\xe2\x96\x88\xe2\x96\x88 \xe2\x96\x88\xe2\x96\x88\xe2\x96\x88  \xe2\x96\x88\xe2\x96\x88\xe2\x96\x88   \xe2\x96\x88\xe2\x96\x88\xe2\x96\x88\xe2\x96\x88     \xe2\x96\x88\xe2\x96\x88\xe2\x96\x88\xe2\x96\x88",
    "  \xe2\x96\x88\xe2\x96\x88\xe2\x96\x88   \xe2\x96\x88\xe2\x96\x88\xe2\x96\x88 \xe2\x96\x88\xe2\x96\x88\xe2\x96\x88  \xe2\x96\x88\xe2\x96\x88\xe2\x96\x88 \xe2\x96\x88\xe2\x96\x88\xe2\x96\x88\xe2\x96\x80\xe2\x96\x88\xe2\x96\x88\xe2\x96\x84  \xe2\x96\x88\xe2\x96\x88\xe2\x96\x88\xe2\x96\x80\xe2\x96\x88\xe2\x96\x88\xe2\x96\x84  \xe2\x96\x88\xe2\x96\x88\xe2\x96\x88   \xe2\x96\x88\xe2\x96\x88\xe2\x96\x88 \xe2\x96\x88\xe2\x96\x88\xe2\x96\x88  \xe2\x96\x88\xe2\x96\x88\xe2\x96\x88   \xe2\x96\x88\xe2\x96\x88\xe2\x96\x88\xe2\x96\x88     \xe2\x96\x88\xe2\x96\x88\xe2\x96\x88\xe2\x96\x88",
    " \xe2\x96\x84\xe2\x96\x88\xe2\x96\x88\xe2\x96\x80  \xe2\x96\x84\xe2\x96\x88\xe2\x96\x88\xe2\x96\x80\xe2\x96\x84\xe2\x96\x88\xe2\x96\x88\xe2\x96\x88\xe2\x96\x84\xe2\x96\x84\xe2\x96\x88\xe2\x96\x88\xe2\x96\x80\xe2\x96\x84\xe2\x96\x88\xe2\x96\x88\xe2\x96\x80 \xe2\x96\x84\xe2\x96\x88\xe2\x96\x88\xe2\x96\x80\xe2\x96\x84\xe2\x96\x88\xe2\x96\x88\xe2\x96\x80 \xe2\x96\x84\xe2\x96\x88\xe2\x96\x88\xe2\x96\x80\xe2\x96\x84\xe2\x96\x88\xe2\x96\x88\xe2\x96\x88\xe2\x96\x84\xe2\x96\x84\xe2\x96\x84\xe2\x96\x88\xe2\x96\x88\xe2\x96\x80\xe2\x96\x84\xe2\x96\x88\xe2\x96\x88\xe2\x96\x88\xe2\x96\x84\xe2\x96\x84\xe2\x96\x88\xe2\x96\x88\xe2\x96\x80\xe2\x96\x84\xe2\x96\x84\xe2\x96\x88\xe2\x96\x80\xe2\x96\x80\xe2\x96\x88\xe2\x96\x88\xe2\x96\x88 \xe2\x96\x84\xe2\x96\x84\xe2\x96\x88\xe2\x96\x80\xe2\x96\x80\xe2\x96\x88\xe2\x96\x88\xe2\x96\x88",
    " \xe2\x96\x93\xe2\x96\x93\xe2\x96\x93   \xe2\x96\x93\xe2\x96\x93\xe2\x96\x93  \xe2\x96\x80\xe2\x96\x93\xe2\x96\x93\xe2\x96\x93\xe2\x96\x93\xe2\x96\x80  \xe2\x96\x93\xe2\x96\x93\xe2\x96\x93  \xe2\x96\x93\xe2\x96\x93\xe2\x96\x93 \xe2\x96\x93\xe2\x96\x93\xe2\x96\x93  \xe2\x96\x93\xe2\x96\x93\xe2\x96\x93 \xe2\x96\x93\xe2\x96\x93\xe2\x96\x93\xe2\x96\x93\xe2\x96\x93\xe2\x96\x93\xe2\x96\x80\xe2\x96\x80   \xe2\x96\x80\xe2\x96\x93\xe2\x96\x93\xe2\x96\x93\xe2\x96\x93\xe2\x96\x80  \xe2\x96\x93\xe2\x96\x93\xe2\x96\x93  \xe2\x96\x93\xe2\x96\x93\xe2\x96\x93 \xe2\x96\x93\xe2\x96\x93\xe2\x96\x93  \xe2\x96\x93\xe2\x96\x93\xe2\x96\x93",
    "     \xe2\x96\x88 \xe2\x96\x84   \xe2\x96\x88      \xe2\x96\x88\xe2\x96\x88  \xe2\x96\x88 \xe2\x96\x84      \xe2\x96\x84\xe2\x96\x88\xe2\x96\x88 \xe2\x96\x84 \xe2\x96\x88 \xe2\x96\x88  \xe2\x96\x84\xe2\x96\x88\xe2\x96\x84    \xe2\x96\x84\xe2\x96\x84\xe2\x96\x84 \xe2\x96\x88 \xe2\x96\x88\xe2\x96\x88    \xe2\x96\x80       \xe2\x96\x84  \xe2\x96\x88 \xe2\x96\x84 \xe2\x96\x80\xe2\x96\x88",
    " \xe2\x96\x88\xe2\x96\x88\xe2\x96\x88\xe2\x96\x84\xe2\x96\x88 \xe2\x96\x88\xe2\x96\x88\xe2\x96\x88\xe2\x96\x88\xe2\x96\x88 \xe2\x96\x88 \xe2\x96\x84 \xe2\x96\x84\xe2\x96\x88\xe2\x96\x88 \xe2\x96\x84\xe2\x96\x88 \xe2\x96\x88\xe2\x96\x88\xe2\x96\x84  \xe2\x96\x84\xe2\x96\x84\xe2\x96\x88\xe2\x96\x88\xe2\x96\x88\xe2\x96\x84\xe2\x96\x88 \xe2\x96\x88 \xe2\x96\x88\xe2\x96\x88 \xe2\x96\x88\xe2\x96\x88\xe2\x96\x88\xe2\x96\x84\xe2\x96\x84\xe2\x96\x84\xe2\x96\x88\xe2\x96\x88\xe2\x96\x88\xe2\x96\x80\xe2\x96\x84\xe2\x96\x88\xe2\x96\x84\xe2\x96\x88\xe2\x96\x88 \xe2\x96\x84 \xe2\x96\x84\xe2\x96\x88\xe2\x96\x84\xe2\x96\x88\xe2\x96\x88\xe2\x96\x84\xe2\x96\x88\xe2\x96\x88 \xe2\x96\x88 \xe2\x96\x88\xe2\x96\x88\xe2\x96\x84\xe2\x96\x88\xe2\x96\x84\xe2\x96\x84\xe2\x96\x88\xe2\x96\x88",
    "\xe2\x96\x80\xe2\x96\x80\xe2\x96\x80\xe2\x96\x80\xe2\x96\x80\xe2\x96\x80\xe2\x96\x80\xe2\x96\x80\xe2\x96\x80\xe2\x96\x80\xe2\x96\x80\xe2\x96\x80\xe2\x96\x80\xe2\x96\x80\xe2\x96\x80\xe2\x96\x80\xe2\x96\x80\xe2\x96\x80\xe2\x96\x80\xe2\x96\x80\xe2\x96\x80\xe2\x96\x80\xe2\x96\x80\xe2\x96\x80\xe2\x96\x80\xe2\x96\x80\xe2\x96\x80\xe2\x96\x80\xe2\x96\x80\xe2\x96\x80\xe2\x96\x80\xe2\x96\x80\xe2\x96\x80\xe2\x96\x80\xe2\x96\x80\xe2\x96\x80\xe2\x96\x80\xe2\x96\x80\xe2\x96\x80\xe2\x96\x80\xe2\x96\x80\xe2\x96\x80\xe2\x96\x80\xe2\x96\x80\xe2\x96\x80\xe2\x96\x80\xe2\x96\x80\xe2\x96\x80\xe2\x96\x80\xe2\x96\x80\xe2\x96\x80\xe2\x96\x80\xe2\x96\x80\xe2\x96\x80\xe2\x96\x80\xe2\x96\x80\xe2\x96\x80\xe2\x96\x80\xe2\x96\x80\xe2\x96\x80\xe2\x96\x80\xe2\x96\x80\xe2\x96\x80\xe2\x96\x80\xe2\x96\x80\xe2\x96\x80\xe2\x96\x80\xe2\x96\x80\xe2\x96\x80\xe2\x96\x80\xe2\x96\x80\xe2\x96\x80\xe2\x96\x80\xe2\x96\x80\xe2\x96\x80\xe2\x96\x80\xe2\x96\x80\xe2\x96\x80\xe2\x96\x80",
    "\xce\xa9\xe2\x95\x9e\xe2\x95\x90\xe2\x95\xa7\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\xa7\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\xa7\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\xa7\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\xa7\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\xa7\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\xa7\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\xa7\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\xa7\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\xa7\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\xa7\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\xa7\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\xa7\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\xa7\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\xa7\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\xa1\xce\xa9",
    "                    \xe2\x96\x91\xe2\x96\x92\xe2\x96\x93\xe2\x96\x88 GPU : Java + Bedrock : OpenCL \xe2\x96\x88\xe2\x96\x93\xe2\x96\x92\xe2\x96\x91",
    "",
    "                    \xe2\x96\x84\xe2\x96\x80\xe2\x96\x84\xe2\x96\x80  Got bedrock? Get locations.  \xe2\x96\x80\xe2\x96\x84\xe2\x96\x80\xe2\x96\x84",
    "          \xe2\x96\x91\xe2\x96\x92\xe2\x96\x93\xe2\x96\x88\xe2\x96\x93\xe2\x96\x92\xe2\x96\x91  Minecraft 26.2 bedrock floors, bit for bit  \xe2\x96\x91\xe2\x96\x92\xe2\x96\x93\xe2\x96\x88\xe2\x96\x93\xe2\x96\x92\xe2\x96\x91",
};
constexpr int kSplashRows = static_cast<int>(sizeof(kSplash) / sizeof(kSplash[0]));
static_assert(kSplashRows + 2 == kSplashNeedRows, "banner rows + blank + prompt");

// The splash animates off App::now_ms (nothing here reads a clock):
//   - a search beam sweeps the bedrock floor left to right, stops on a spot,
//     flashes it green and names it, then sweeps on; one spot per pass;
//   - the rails' teeth turn like gears (top right, bottom left), the end
//     glyphs swap Omega <-> Theta, and a highlight ripples out of the halo.
constexpr int kBeamMsPerCol = 40, kLockMs = 1500, kPassGapMs = 500;
constexpr int kPassMs = kSplashNeedCols * kBeamMsPerCol + kLockMs + kPassGapMs;
constexpr int kFloorTop = 13, kFloorRows = 3, kTopRail = 3, kBottomRail = 16, kCaptionRow = 18;
const char* kOmega = "\xce\xa9";
const char* kTheta = "\xce\x98";

// Where the beam is at time `t`: its column (past the right edge between
// passes) and, while it holds a spot, that spot's centre (else -1).
struct Beam {
    int col, lock;
};
Beam beam_at(long long t) {
    const long long pass = t / kPassMs;
    const int ms = static_cast<int>(t % kPassMs);
    const int lock = 6 + static_cast<int>((pass * 29) % (kSplashNeedCols - 12));  // spot +-2 stays on screen
    const int reach = lock * kBeamMsPerCol;
    if (ms < reach) return {ms / kBeamMsPerCol, -1};
    if (ms < reach + kLockMs) return {lock, lock};
    return {lock + (ms - reach - kLockMs) / kBeamMsPerCol, -1};
}

// A banner row split into its characters, one per column.
std::vector<std::string> glyphs(const std::string& s) {
    std::vector<std::string> g;
    for (unsigned char c : s) {
        if ((c & 0xC0) != 0x80 || g.empty()) g.emplace_back();
        g.back() += static_cast<char>(c);
    }
    return g;
}

// Join `g`, colouring column x with colour(x); a code is written only where the colour changes.
template <class F>
std::string paint(const std::vector<std::string>& g, F colour) {
    std::string out;
    const char* cur = nullptr;
    for (int x = 0; x < static_cast<int>(g.size()); ++x) {
        const char* c = colour(x);
        if (c != cur) out += kRst, out += c, cur = c;
        out += g[static_cast<std::size_t>(x)];
    }
    return out + kRst;
}

std::string splash_row(const App& app, int i) {
    const long long t = app.now_ms;
    std::vector<std::string> g = glyphs(kSplash[i]);
    const int n = static_cast<int>(g.size());
    if (i < kTopRail + 1 || i == kBottomRail) {  // halo + rails
        const bool swap = (t / 600) % 2 != 0;
        for (auto& c : g)
            if (c == kOmega || c == kTheta) c = ((c == kOmega) != swap) ? kOmega : kTheta;
        if (i == kTopRail || i == kBottomRail) {
            const int dir = i == kTopRail ? 1 : -1, phase = static_cast<int>((t / 150) % 5);
            const std::string tooth = g[3];  // the rows start "Omega, left cap, =, tooth"
            for (int x = 2; x < n - 2; ++x) g[static_cast<std::size_t>(x)] = (((x - 2 - dir * phase) % 5 + 5) % 5 == 1) ? tooth : "\xe2\x95\x90";
        }
        const int ripple = static_cast<int>((t / 80) % 24), centre = kSplashNeedCols / 2 - 1;
        return paint(g, [&](int x) {
            const int d = x < centre ? centre - x : x - centre;
            return (i < kTopRail && (d == ripple || d == ripple - 1)) ? "\x1b[1;97m" : kAir;
        });
    }
    if (i >= kFloorTop && i < kFloorTop + kFloorRows) {  // the floor under the beam
        const Beam b = beam_at(t);
        const bool flash = (t / 250) % 2 != 0;
        for (int x = 0; x < n; ++x)
            if (x == b.col && g[static_cast<std::size_t>(x)] == " ") g[static_cast<std::size_t>(x)] = "\xe2\x96\x91";
        return paint(g, [&](int x) {
            if (b.lock >= 0 && x >= b.lock - 2 && x <= b.lock + 2) return flash ? "\x1b[1;92m" : "\x1b[1;97;42m";
            if (x == b.col) return "\x1b[1;93m";
            if (x == b.col - 1 || x == b.col - 2) return "\x1b[33m";
            return kDim;
        });
    }
    if (i == kCaptionRow) {
        const Beam b = beam_at(t);
        char buf[96];
        if (b.lock >= 0)
            std::snprintf(buf, sizeof(buf), "pattern locked   seed 1911   x %d..%d   z 0   y -64..-60", b.lock - 2, b.lock + 2);
        else if (b.col < kSplashNeedCols)
            std::snprintf(buf, sizeof(buf), "scanning   x %d", b.col);
        else
            buf[0] = '\0';
        const std::string s = buf;
        const int lead = (kSplashNeedCols - static_cast<int>(s.size())) / 2;
        return std::string(static_cast<std::size_t>(lead), ' ') + (b.lock >= 0 ? hi(s) : dim(s));
    }
    // letters bedrock yellow, the info rows dim, the tagline bright
    return std::string(i < kFloorTop ? kBed : i == 19 ? kHi : kDim) + kSplash[i] + kRst;
}

void draw_splash(const App& app, Frame& fr) {
    const std::string pad(static_cast<std::size_t>((app.term_cols - kSplashNeedCols) / 2), ' ');
    for (int i = 0; i < kSplashRows; ++i) fr.line(pad + splash_row(app, i));
    fr.line();
    const long long ms = app.now_ms % 5000;  // a quick double blink every 5 s
    const bool off = (ms >= 4400 && ms < 4600) || ms >= 4800;
    fr.line(pad + std::string(33, ' ') + dim(off ? "             " : "press any key"));
}

// Any key goes on to the parameters.
bool handle_splash(App& app, int) {
    app.screen = Screen::params;
    return true;
}

// ---------------------------------------------------------------------------
// parameters
// ---------------------------------------------------------------------------

// The rows of the params screen, top to bottom; App::field indexes this.
// Adding a field: add it here, then to kFieldNames, field_value and
// handle_params' switch.
enum Field {
    F_EDITION, F_SEED, F_W, F_H, F_Y, F_CX, F_CZ, F_RADIUS, F_ORIENT, F_STOP, F_BACKEND, F_CKPT, F_COUNT
};

const char* const kFieldNames[F_COUNT] = {
    "edition", "seed", "width", "height", "Y layer", "center X", "center Z",
    "radius (-1=all)", "orientations", "stop at first", "backend", "checkpoint file"};

std::string backend_text(const App& app) {
    if (app.backend_idx < 0) return "auto  (" + app.backend_label + ")";
    const svc::BackendInfo& b = app.backends[static_cast<std::size_t>(app.backend_idx)];
    std::string s = b.id + "  " + b.label;
    if (b.units > 0)
        s += "  (" + std::to_string(b.units) +
             (!b.is_gpu ? " threads)" : b.integrated ? " CUs, integrated)" : " CUs)");
    return s;
}

std::string field_value(const App& app, int f) {
    const Model& m = app.m;
    switch (f) {
        case F_EDITION:
            return m.edition == svc::Edition::bedrock ? "Bedrock  (the same floor in every world)" : "Java";
        case F_SEED: return m.edition == svc::Edition::bedrock ? m.seed + "  (not used on Bedrock)" : m.seed;
        case F_W: return std::to_string(m.w);
        case F_H: return std::to_string(m.h);
        case F_Y: return std::to_string(m.y);
        case F_CX: return m.cx;
        case F_CZ: return m.cz;
        case F_RADIUS: return m.radius == "-1" ? "-1  (whole world, center ignored)" : m.radius;
        case F_ORIENT: return m.all_orient ? "all 8" : "exact";
        case F_STOP:
            return m.stop_first ? "yes  (end at the first match)" : "no  (scan the whole region)";
        case F_BACKEND: return backend_text(app);
        case F_CKPT: return m.checkpoint.empty() ? std::string("(auto) ") + svc::kAutosavePath : m.checkpoint;
    }
    return "";
}

void draw_params(const App& app, Frame& fr) {
    fr.line(hi("RokkDoxx  \xc2\xb7  bedrock pattern search"));
    fr.line();
    fr.line("Parameters   (Up/Down/Tab move, type to edit, Del clears, Left/Right to change)");
    fr.line();
    for (int f = 0; f < F_COUNT; ++f) {
        const bool sel = (f == app.field);
        char buf[64];
        std::snprintf(buf, sizeof(buf), "  %s %-16s ", sel ? "\xe2\x96\xb6" : " ", kFieldNames[f]);
        fr.put(buf);
        if (sel) fr.put(kInv);
        fr.put(" " + field_value(app, f) + " ");
        if (sel) fr.put(kRst);
        fr.line();
    }
    fr.line();
    {
        const int y = app.m.y;
        const double p = bedrock_probability(app.m.edition, y);
        char b[160];
        std::snprintf(b, sizeof(b), "  Y=%d  ->  P(bedrock) = %.2f   %s", y, p,
                      (p >= 1)     ? "(solid everywhere -- nothing to match)"
                      : (p <= 0)   ? "(air everywhere -- nothing to match)"
                      : (y == -60) ? "(recommended: most detail per cell)"
                                   : "");
        fr.line(dim(b));
    }
    {
        const double n = search_candidates(app.m);
        char b[160];
        if (n < 0)
            std::snprintf(b, sizeof(b), "  search area: radius must be -1 or a non-negative integer");
        else
            std::snprintf(b, sizeof(b), "  search area: %.4g candidate origins", n);
        fr.line(dim(b));
    }
    if (!app.m.checkpoint.empty())
        fr.line(dim("  checkpoint: resumes automatically if the file matches this exact search"));
    fr.line();
    fr.line("  " + hi("Enter") + " edit pattern      " + hi("L") + " load file      " + hi("q") +
            " quit");
    status_line(app, fr);
}

// Shared line editing for text fields and the prompt: Backspace deletes the
// last char, Delete clears, printable keys append (digits and a leading '-'
// only, for numeric fields). No cursor movement within the text.
void edit_text(std::string& s, int key, bool digits_only) {
    if (key == K_BACKSPACE) {
        if (!s.empty()) s.pop_back();
    } else if (key == K_DELETE) {
        s.clear();
    } else if (key >= 32 && key < 127) {
        const char c = static_cast<char>(key);
        if (digits_only) {
            if ((c >= '0' && c <= '9') || (c == '-' && s.empty())) s.push_back(c);
        } else if (s.size() < 240) {
            s.push_back(c);
        }
    }
}

// Switch to the one-line prompt. On Enter, handle_prompt returns to `ret` and
// calls `done(app)`, which reads app.prompt_buf (do_load_pattern,
// do_save_pattern, do_save_matches, do_save_session).
void begin_prompt(App& app, const std::string& label, Screen ret, void (*done)(App&)) {
    app.prompt_label = label;
    app.prompt_buf.clear();
    app.prompt_return = ret;
    app.prompt_done = done;
    app.screen = Screen::prompt;
}

void do_load_pattern(App& app) {
    if (app.prompt_buf.empty()) return;
    svc::PatternFile pf;
    std::string err;
    if (svc::load_pattern_file(app.prompt_buf, pf, err)) {
        file_to_model(pf, app.m);
        app.gx = std::min(app.gx, app.m.w - 1);
        app.gy = std::min(app.gy, app.m.h - 1);
        app.status = "loaded " + app.prompt_buf;
    } else {
        app.status = "load failed: " + err;
    }
}

void cycle_backend(App& app, int dir) {
    // Positions: -1 (auto), 0 .. n-1.
    const int n = static_cast<int>(app.backends.size());
    int idx = app.backend_idx + dir;
    if (idx < -1) idx = n - 1;
    if (idx >= n) idx = -1;
    const std::string id =
        idx < 0 ? "auto" : app.backends[static_cast<std::size_t>(idx)].id;
    std::string err;
    if (select_backend(app, id, err)) {
        app.backend_idx = idx;
        app.status = "backend: " + app.backend_label;
    } else {
        app.status = "backend " + id + " unavailable: " + err;
    }
}

// Keys on the params screen. Letters are typed into the free-text fields
// (seed, checkpoint), so `q` / `L` only act when one of those isn't selected.
bool handle_params(App& app, int k) {
    Model& m = app.m;
    if (k == 'q' && app.field != F_SEED && app.field != F_CKPT) return false;
    if (k == K_ESC) return false;
    if (k == K_TAB || k == K_DOWN) {
        app.field = (app.field + 1) % F_COUNT;
        return true;
    }
    if (k == K_UP) {
        app.field = (app.field + F_COUNT - 1) % F_COUNT;
        return true;
    }
    if (k == K_HOME || k == K_PGUP) {
        app.field = 0;
        return true;
    }
    if (k == K_END || k == K_PGDN) {
        app.field = F_COUNT - 1;
        return true;
    }
    if (k == K_ENTER) {
        app.screen = Screen::grid;
        app.status.clear();
        return true;
    }
    if (k == 'L' && app.field != F_SEED && app.field != F_CKPT) {
        begin_prompt(app, "Load pattern file:", Screen::params, do_load_pattern);
        return true;
    }
    const int step = (k == K_LEFT) ? -1 : (k == K_RIGHT) ? 1 : 0;
    switch (app.field) {
        case F_EDITION:
            if (step != 0 || k == ' ')
                m.edition = m.edition == svc::Edition::java ? svc::Edition::bedrock : svc::Edition::java;
            break;
        case F_SEED: edit_text(m.seed, k, false); break;
        case F_W: m.w = std::clamp(m.w + step, 1, kMaxDim); break;
        case F_H: m.h = std::clamp(m.h + step, 1, kMaxDim); break;
        case F_Y: m.y = std::clamp(m.y + step, -64, -59); break;
        case F_CX: edit_text(m.cx, k, true); break;
        case F_CZ: edit_text(m.cz, k, true); break;
        case F_RADIUS: edit_text(m.radius, k, true); break;
        case F_ORIENT:
            if (step != 0 || k == ' ') m.all_orient = !m.all_orient;
            break;
        case F_STOP:
            if (step != 0 || k == ' ') m.stop_first = !m.stop_first;
            break;
        case F_BACKEND:
            if (step != 0) cycle_backend(app, step);
            break;
        case F_CKPT: edit_text(m.checkpoint, k, false); break;
    }
    // The pattern may have shrunk: keep the grid cursor inside it.
    app.gx = std::min(app.gx, m.w - 1);
    app.gy = std::min(app.gy, m.h - 1);
    return true;
}

// ---------------------------------------------------------------------------
// pattern grid
// ---------------------------------------------------------------------------

constexpr int kGridChromeRows = 12;  // header, col index, summary, help, status
constexpr int kRowLabelCols = 6;

// Keep the cursor inside a viewport of vis_rows x vis_cols.
void scroll_to_cursor(App& app, int vis_rows, int vis_cols) {
    auto fit = [](int& start, int cur, int vis, int total) {
        if (cur < start) start = cur;
        if (cur >= start + vis) start = cur - vis + 1;
        start = std::clamp(start, 0, std::max(0, total - vis));
    };
    fit(app.grid_top, app.gy, vis_rows, app.m.h);
    fit(app.grid_left, app.gx, vis_cols, app.m.w);
}

// The pattern editor: header, column index, the visible part of the grid
// (it scrolls when the window is smaller than the pattern), a summary and the
// key help. Not const: it updates the scroll position.
void draw_grid(App& app, Frame& fr) {
    const Model& m = app.m;
    long long r = 0, cx = 0, cz = 0;
    parse_i64(m.radius, r);
    parse_i64(m.cx, cx);
    parse_i64(m.cz, cz);

    const int vis_rows = std::max(1, std::min(m.h, fr.rows_left() - kGridChromeRows));
    const int vis_cols = std::max(1, std::min(m.w, fr.cols() - 1 - kRowLabelCols));
    scroll_to_cursor(app, vis_rows, vis_cols);

    int known = 0, needbed = 0;
    for (int j = 0; j < m.h; ++j)
        for (int i = 0; i < m.w; ++i) {
            if (m.at(i, j) != svc::Cell::unknown) ++known;
            if (m.at(i, j) == svc::Cell::bedrock) ++needbed;
        }

    char hdr[256];
    const std::string world = m.edition == svc::Edition::bedrock ? "Bedrock" : "seed " + m.seed;
    std::snprintf(hdr, sizeof(hdr), "Pattern  %dx%d  Y=%d  %s   %s   center (%lld,%lld) r=%lld",
                  m.w, m.h, m.y, m.all_orient ? "all-orient" : "exact", world.c_str(), cx, cz, r);
    fr.line(hi(hdr));
    fr.line();

    fr.put(std::string(kRowLabelCols, ' '));
    for (int i = app.grid_left; i < app.grid_left + vis_cols; ++i) fr.put(dim(std::to_string(i % 10)));
    fr.line();
    for (int j = app.grid_top; j < app.grid_top + vis_rows; ++j) {
        char rb[16];
        std::snprintf(rb, sizeof(rb), "  %3d ", j);
        fr.put(dim(rb));
        for (int i = app.grid_left; i < app.grid_left + vis_cols; ++i) {
            const svc::Cell c = m.at(i, j);
            const char* col = c == svc::Cell::bedrock ? kBed : c == svc::Cell::not_bedrock ? kAir : kDim;
            const char* gl = c == svc::Cell::bedrock ? "#" : c == svc::Cell::not_bedrock ? "o" : ".";
            fr.put(col);
            if (i == app.gx && j == app.gy) fr.put(kInv);
            fr.put(gl);
            fr.put(kRst);
        }
        fr.line();
    }
    char sb[160];
    if (vis_rows < m.h || vis_cols < m.w) {
        // Uses the spacer row under the grid, so the chrome height is unchanged.
        std::snprintf(sb, sizeof(sb), "  view: rows %d-%d, cols %d-%d (window too small; it scrolls)",
                      app.grid_top, app.grid_top + vis_rows - 1, app.grid_left,
                      app.grid_left + vis_cols - 1);
        fr.line(dim(sb));
    } else {
        fr.line();
    }
    std::snprintf(sb, sizeof(sb), "  known cells: %d / %d      must be bedrock: %d", known, m.w * m.h,
                  needbed);
    fr.line(dim(sb));
    fr.line();
    fr.line("  " + hi("b") + " " + kBed + "#" + kRst + " bedrock   " + hi("e") + " " + kAir + "o" + kRst +
            " empty   " + hi(".") + " unknown   (cursor advances; " + hi("Backspace") + " steps back)");
    fr.line("  " + hi("space") + " cycle   " + hi("arrows/hjkl") + " move   " + hi("Home/End/PgUp/PgDn") +
            " jump");
    fr.line("  " + hi("P") + " fill from world at center   " + hi("C") + " clear   " + hi("S") +
            " save");
    fr.line("  " + hi("Enter") + " run search   " + hi("Tab") + " parameters   " + hi("q") + " quit");
    status_line(app, fr);
}

void do_save_pattern(App& app) {
    if (app.prompt_buf.empty()) return;
    std::string err;
    app.status = svc::save_pattern_file(app.prompt_buf, model_to_file(app.m), err)
                     ? ("saved pattern -> " + app.prompt_buf)
                     : ("save failed: " + err);
}

// ---------------------------------------------------------------------------
// search map
// ---------------------------------------------------------------------------
//
// While a search runs, the progress bar is a picture of the search region: a
// grid of '#', one per area, drawn as
//
//     ┌─────────────┐      grey      not searched yet
//     │ # # # # # # │      yellow    being searched now (pulses)
//     │ # # # # # # │      blue      partly searched
//     │ # # # # # # │      cyan      searched, nothing found (glows on arrival)
//     │ # # # # # # │      green     holds a match (flashes when found; pulses with
//     └─────────────┘                yellow while the area is still being searched)
//                          magenta   the whole map flashes when the search completes
//
// The scan spirals out from the middle of the region, so the map fills in from
// the centre. The service decides the areas (SearchRequest::map_w/h) and reports
// each one's state; this code only turns states into colour. Each '#' is
// followed by a space and terminal cells are ~twice as tall as wide, so the map
// looks square.

constexpr int kMapChromeRows = 14;  // every row of the search screen that is not a hash row
constexpr int kMaxMapSide = 40;     // bigger than this stops looking like one picture
constexpr int kMinMapSide = 4;

constexpr long long kFlashMs = 250;       // half a flash: on for this long, then off
constexpr long long kPopMs = 750;         // a cell that just finished blinks for this long
constexpr long long kHitFlashMs = 3000;   // a fresh hit flashes this long, then stays lit
constexpr long long kCelebrateMs = 3000;  // the whole map flashes this long on completion
constexpr long long kLongAgo = -1'000'000'000;

const char* kMapPending = "\x1b[2m";
const char* kMapSearchA = "\x1b[1;93m";   // searching: bright yellow <-> yellow
const char* kMapSearchB = "\x1b[33m";
const char* kMapPopA = "\x1b[1;96m";      // just searched: bright cyan <-> cyan, then cyan
const char* kMapPartial = "\x1b[94m";    // some of it searched: bright blue (turns cyan when finished)
const char* kMapDone = "\x1b[36m";
const char* kMapHitA = "\x1b[1;30;102m";  // hit: black on bright green <-> white on green,
const char* kMapHitB = "\x1b[1;97;42m";   //   then black on green for good
const char* kMapHit = "\x1b[1;30;42m";
const char* kMapParty = "\x1b[1;95m";     // completion: bright magenta

// Cells per side for a terminal of this size, or 0 if it is too small for a map.
int pick_map_side(int cols, int rows) {
    // Width: a box is 2 columns per cell + 3 (two borders and a space); the
    // frame keeps one column free.
    const int side = std::min({kMaxMapSide, rows - kMapChromeRows, (cols - 4) / 2});
    return side >= kMinMapSide ? side : 0;
}

// Remember when each cell last changed phase and when it first showed a match,
// so a fresh find or finish can flash from the moment it is first seen. The
// first map seen is the baseline: whatever a resumed checkpoint already covers
// is not "news". (A match in a cell that is being searched right now is news:
// a fast GPU can find one before the first poll.)
void track_map(App& app) {
    const std::vector<svc::MapCell>& map = app.jst.map;
    if (map.size() != app.map_prev.size()) {
        app.map_prev = map;
        app.phase_since.assign(map.size(), kLongAgo);
        app.hit_since.assign(map.size(), kLongAgo);
        for (std::size_t i = 0; i < map.size(); ++i)
            if (map[i].hit && map[i].phase == svc::MapCell::searching) app.hit_since[i] = app.now_ms;
        return;
    }
    for (std::size_t i = 0; i < map.size(); ++i) {
        if (map[i].phase != app.map_prev[i].phase) app.phase_since[i] = app.now_ms;
        if (map[i].hit && !app.map_prev[i].hit) app.hit_since[i] = app.now_ms;
        app.map_prev[i] = map[i];
    }
}

// Is the map on screen? While the job runs, always. Afterwards: through the
// completion flash, and for good if there are no matches to list in its place.
// `m` overrides that.
bool map_wanted(const App& app) {
    if (app.map_side <= 0) return false;
    if (app.job_running) return true;
    if (app.map_toggle >= 0) return app.map_toggle == 1;
    return app.matches.empty() || app.now_ms - app.finished_at_ms < kCelebrateMs;
}

struct MapView {
    int mw = 1, mh = 1;  // cells the service reports (1x1 until the job has started)
    int scale = 0;       // hashes per cell, each way; 0 = no map fits
    int w() const { return mw * scale; }
    int h() const { return mh * scale; }
};

// How big the map is drawn. A coarse map (few tiles in the region) is blown up
// to fill the space it was given. `rows` = the terminal's height.
MapView map_view(const App& app, int cols, int rows) {
    MapView v;
    if (app.map_side <= 0) return v;
    v.mw = std::max(1, app.jst.map_w);
    v.mh = std::max(1, app.jst.map_h);
    v.scale = std::max(1, std::min(app.map_side / v.mw, app.map_side / v.mh));
    // The window shrank since the search began: fall back to the plain bar.
    if (v.h() + kMapChromeRows > rows || 2 * v.w() + 3 > cols - 1) v.scale = 0;
    return v;
}

// How one cell looks right now. `on` is the global flash beat; the ages say how
// long ago the cell last changed phase / first showed a match.
const char* cell_style(svc::MapCell c, long long phase_age, long long hit_age, bool on,
                       bool celebrate) {
    if (c.hit && (celebrate || hit_age < kHitFlashMs)) {
        // A fresh find starts on the bright beat, so it flashes the moment it is seen.
        const bool bright = celebrate ? on : (hit_age / kFlashMs) % 2 == 0;
        return bright ? kMapHitA : kMapHitB;
    }
    switch (c.phase) {
        case svc::MapCell::pending: return c.hit ? kMapHit : kMapPending;
        case svc::MapCell::partial: return c.hit ? kMapHit : kMapPartial;
        case svc::MapCell::searching:
            if (c.hit) return on ? kMapSearchA : kMapHit;  // being searched, already holds a match
            return on ? kMapSearchA : kMapSearchB;
        case svc::MapCell::done:
            if (c.hit) return kMapHit;
            if (celebrate) return on ? kMapParty : kMapDone;
            return phase_age < kPopMs && (phase_age / kFlashMs) % 2 == 0 ? kMapPopA : kMapDone;
    }
    return kMapPending;
}

// The map, its legend, and a blank row: v.h() + 4 rows.
void draw_map(const App& app, Frame& fr, const MapView& v) {
    const svc::JobStatus& st = app.jst;
    const bool have = st.map_w > 0 && st.map.size() == static_cast<std::size_t>(st.map_w) * st.map_h;
    const bool on = (app.now_ms / kFlashMs) % 2 == 0;
    const bool celebrate =
        st.state == svc::JobState::done && app.now_ms - app.finished_at_ms < kCelebrateMs;

    const int box = 2 * v.w() + 3;
    const std::string indent(static_cast<std::size_t>(std::max(2, (fr.cols() - 1 - box) / 2)), ' ');
    std::string rule;
    for (int i = 0; i < 2 * v.w() + 1; ++i) rule += "\xe2\x94\x80";  // ─
    fr.line(indent + dim("\xe2\x94\x8c" + rule + "\xe2\x94\x90"));   // ┌ ┐

    for (int j = 0; j < v.h(); ++j) {
        std::string row = indent + dim("\xe2\x94\x82") + " ";        // │
        for (int i = 0; i < v.w(); ++i) {
            const std::size_t c = static_cast<std::size_t>(j / v.scale) * v.mw + i / v.scale;
            const char* style = kMapPending;
            if (have) {
                const long long ps = c < app.phase_since.size() ? app.phase_since[c] : kLongAgo;
                const long long hs = c < app.hit_since.size() ? app.hit_since[c] : kLongAgo;
                style = cell_style(st.map[c], app.now_ms - ps, app.now_ms - hs, on, celebrate);
            }
            row += std::string(style) + "#" + kRst + " ";
        }
        fr.line(row + dim("\xe2\x94\x82"));
    }
    fr.line(indent + dim("\xe2\x94\x94" + rule + "\xe2\x94\x98"));   // └ ┘

    // Centred under the map when it fits, else pushed left so it never clips.
    const std::size_t len = std::string("# not yet   # searching   # partial   # clear   # match").size();
    const std::size_t pad = std::max<std::size_t>(
        2, std::min(indent.size(), static_cast<std::size_t>(std::max(0, fr.cols() - 1)) - len));
    fr.line(std::string(pad, ' ') + kMapPending + "#" + kRst + " not yet   " + kMapSearchA + "#" +
            kRst + " searching   " + kMapPartial + "#" + kRst + " partial   " + kMapDone + "#" + kRst +
            " clear   " + kMapHit + "#" + kRst + " match");
    fr.line();
}

// "region x .. z ..   1 # ~ 2.3M blocks": where the map is, and how much each '#' covers.
std::string map_caption(const svc::Region& r, const MapView& v) {
    const std::string cw = format_blocks(static_cast<double>(r.x1 - r.x0 + 1) / v.w());
    const std::string ch = format_blocks(static_cast<double>(r.z1 - r.z0 + 1) / v.h());
    return "  region  x " + format_blocks(static_cast<double>(r.x0)) + " .. " +
           format_blocks(static_cast<double>(r.x1)) + "   z " +
           format_blocks(static_cast<double>(r.z0)) + " .. " +
           format_blocks(static_cast<double>(r.z1)) + "   1 # ~ " + cw +
           (cw == ch ? "" : " x " + ch) + " blocks";
}

// `resume_text`: progress from an earlier run of this same search (empty = start fresh).
// Called by Enter on the grid, `r` on the result screen, and resume_session.
// Resets everything the previous job left in App, builds the request from the
// Model, asks for a map sized to the terminal, and submits. Errors (bad model,
// backend failure) are shown on the result screen as a job error.
void start_search(App& app, const std::string& resume_text = "") {
    svc::SearchRequest req;
    std::string err;
    app.want_pause = false;
    app.matches.clear();
    app.result_scroll = 0;
    app.status.clear();
    app.screen = Screen::result;
    app.jst = {};
    app.map_side = 0;
    app.map_prev.clear();
    app.phase_since.clear();
    app.hit_since.clear();
    app.map_toggle = -1;
    app.finished_at_ms = kLongAgo;
    if (!build_request(app.m, req, err)) {
        app.jst.state = svc::JobState::error;
        app.jst.error = err;
        return;
    }
    app.map_side = pick_map_side(app.term_cols, app.term_rows);
    req.map_w = req.map_h = app.map_side;
    req.keep_checkpoint = true;  // so `s` can save, and `r` continue, at any time
    req.resume_text = resume_text;
    app.job_region = req.region;
    try {
        app.job = app.client->submit(req);
        app.job_running = true;
        app.jst = app.client->poll(app.job);
    } catch (const std::exception& e) {
        app.job_running = false;
        app.jst.state = svc::JobState::error;
        app.jst.error = e.what();
    }
}

// Set the cell under the cursor, then move right (wrapping to the next row;
// the last cell stays put).
void paint(App& app, svc::Cell c) {
    app.m.at(app.gx, app.gy) = c;
    if (app.gx + 1 < app.m.w) ++app.gx;
    else if (app.gy + 1 < app.m.h) app.gx = 0, ++app.gy;
}

// Keys on the pattern grid. Arrows/hjkl wrap around the edges.
bool handle_grid(App& app, int k) {
    Model& m = app.m;
    switch (k) {
        case 'q': return false;
        case K_TAB:
            app.screen = Screen::params;
            app.status.clear();
            return true;
        case K_UP: case 'k': app.gy = (app.gy + m.h - 1) % m.h; return true;
        case K_DOWN: case 'j': app.gy = (app.gy + 1) % m.h; return true;
        case K_LEFT: case 'h': app.gx = (app.gx + m.w - 1) % m.w; return true;
        case K_RIGHT: case 'l': app.gx = (app.gx + 1) % m.w; return true;
        case K_HOME: app.gx = 0; return true;
        case K_END: app.gx = m.w - 1; return true;
        case K_PGUP: app.gy = 0; return true;
        case K_PGDN: app.gy = m.h - 1; return true;
        case ' ': {
            svc::Cell& c = m.at(app.gx, app.gy);
            c = static_cast<svc::Cell>((static_cast<int>(c) + 1) % 3);
            return true;
        }
        // Paint and advance like typing, so a row is entered as e.g. "bbeeb".
        case 'b': case '#': case '1': paint(app, svc::Cell::bedrock); return true;
        case 'e': case 'o': case '0': paint(app, svc::Cell::not_bedrock); return true;
        case '.': case 'x': paint(app, svc::Cell::unknown); return true;
        case K_DELETE: m.at(app.gx, app.gy) = svc::Cell::unknown; return true;
        case K_BACKSPACE:  // step back one cell, wrapping to the previous row's end
            if (app.gx > 0) --app.gx;
            else if (app.gy > 0) app.gx = m.w - 1, --app.gy;
            return true;
        case 'C':
            m.clear();
            app.status = "cleared";
            return true;
        case 'P': {
            std::string err;
            app.status = fill_from_world(m, err)
                             ? "filled pattern from world at center (round-trip test)"
                             : err;
            return true;
        }
        case 'S':
            begin_prompt(app, "Save pattern to file:", Screen::grid, do_save_pattern);
            return true;
        case K_ENTER: start_search(app); return true;
    }
    return true;
}

// ---------------------------------------------------------------------------
// search / results
// ---------------------------------------------------------------------------

std::string progress_bar(double frac, int width) {
    frac = std::clamp(frac, 0.0, 1.0);
    const int fill = static_cast<int>(frac * width + 0.5);
    std::string s = "[";
    for (int i = 0; i < width; ++i) s += (i < fill) ? '#' : '-';
    s += "]";
    return s;
}

constexpr int kResultFooterRows = 5;  // rows kept free under the match list

// A stopped search can carry on: cancelled, or ended early at a match.
bool can_continue(const App& app) {
    return !app.job_running &&
           (app.jst.state == svc::JobState::cancelled ||
            (app.jst.state == svc::JobState::done && app.jst.stopped_early));
}

// The search screen: status lines, then either the live map or (once the job
// has ended and the map has had its moment) the scrolling match list, then
// the key help for the current state.
void draw_result(App& app, Frame& fr) {
    const svc::JobStatus& st = app.jst;
    // Sized from the whole terminal, so this comes before any row is drawn.
    const MapView mv = map_view(app, fr.cols(), fr.rows_left());
    const bool show_map = mv.scale > 0 && map_wanted(app);

    fr.line(hi("Search") + dim("   backend: " + app.backend_label));
    if (st.state == svc::JobState::error) {
        fr.line();
        fr.line(std::string(kAir) + "  error: " + st.error + kRst);
        fr.line();
        fr.line("  " + hi("Enter/Esc") + " back");
        return;
    }
    fr.line(show_map ? dim(map_caption(app.job_region, mv)) : "");

    char b[256];
    const char* label = st.stopped_early                                             ? "found, stopped"
                        : app.want_pause && st.state == svc::JobState::running ? "pausing"
                                                                              : to_string(st.state);
    if (show_map)  // the map is the progress bar
        std::snprintf(b, sizeof(b), "  %s  %5.1f%%   %.0f M/s", label, st.progress * 100.0,
                      st.rate / 1e6);
    else
        std::snprintf(b, sizeof(b), "  %s  %5.1f%%   %s   %.0f M/s", label, st.progress * 100.0,
                      progress_bar(st.progress, 32).c_str(), st.rate / 1e6);
    fr.line(b);
    std::string times = "  elapsed " + svc::format_duration(st.elapsed_s);
    if (st.state == svc::JobState::paused) times += "    (paused: p to resume)";
    else if (app.job_running) times += "    ETA " + (st.rate > 0 ? svc::format_duration(st.eta_s) : "--");
    fr.line(times);
    std::snprintf(b, sizeof(b), "  scanned %.4g / %.4g origins    matches: %llu%s",
                  static_cast<double>(st.candidates_done), static_cast<double>(st.candidates_total),
                  static_cast<unsigned long long>(st.matches), st.truncated ? "  (capped)" : "");
    fr.line(b);
    if (!app.m.checkpoint.empty()) fr.line(dim("  checkpoint: " + app.m.checkpoint));
    fr.line();
    if (show_map) draw_map(app, fr, mv);

    if (app.job_running) {
        const bool paused = app.want_pause || st.state == svc::JobState::paused;
        fr.line("  " + hi("p") + (paused ? " resume   " : " pause   ") + hi("c") + " cancel   " + hi("s") +
                " save session");
        status_line(app, fr);
        return;
    }

    const int total = static_cast<int>(app.matches.size());
    // With the map up there may be no room left for the list at all.
    const int room = std::max(0, fr.rows_left() - (total > 0 ? 1 : 0) - kResultFooterRows);
    if (total > 0 && room > 0) fr.line(dim("  anchor cell (~ pattern centre) of each match:"));
    const int rows = std::max(1, room);
    app.result_rows = rows;
    app.result_scroll = std::clamp(app.result_scroll, 0, std::max(0, total - rows));
    const int start = app.result_scroll;
    for (int k = start; k < std::min(total, start + room); ++k) {
        const svc::Match& mm = app.matches[static_cast<std::size_t>(k)];
        char lb[160];
        if (app.m.all_orient)
            std::snprintf(lb, sizeof(lb), "    x = %-11d  z = %-11d  %s", mm.x, mm.z,
                          orient_names(mm.orient_mask).c_str());
        else
            std::snprintf(lb, sizeof(lb), "    x = %-11d  z = %-11d", mm.x, mm.z);
        fr.line(lb);
    }
    if (room > 0 && total > rows) {
        char lb[96];
        std::snprintf(lb, sizeof(lb), "  [%d-%d of %d]  Up/Down/PgUp/PgDn/Home/End scroll", start + 1,
                      std::min(total, start + rows), total);
        fr.line(dim(lb));
    }
    fr.line();
    fr.line("  " + hi("S") + " save matches   " + hi("s") + " save session   " +
            (can_continue(app) ? hi("r") + " continue   " : "") +
            (mv.scale > 0 ? hi("m") + " map   " : "") + hi("Enter/Esc") + " back");
    status_line(app, fr);
}

void do_save_matches(App& app) {
    if (app.prompt_buf.empty()) return;
    std::ofstream f(app.prompt_buf, std::ios::binary | std::ios::trunc);
    if (!f) {
        app.status = "save failed: cannot open " + app.prompt_buf;
        return;
    }
    f << matches_header(app.m) << format_matches(app.matches);
    app.status = f ? "saved " + std::to_string(app.matches.size()) + " matches -> " + app.prompt_buf
                   : "save failed: write error";
}

// Save the search so it can be picked up later (even after quitting): the pattern
// file with a `checkpoint` line, plus the progress it names (<file>.ckpt). Running
// jobs are saved as of their last few seconds; a paused or stopped one exactly.
void do_save_session(App& app) {
    const std::string path = app.prompt_buf;
    if (path.empty()) return;
    const std::string text = app.client->checkpoint(app.job);
    if (text.empty()) {
        app.status = "nothing to save yet: wait for the first tile (a few seconds), or pause first";
        return;
    }
    const std::string ckpt = path + ".ckpt";
    {
        std::ofstream f(ckpt, std::ios::binary | std::ios::trunc);
        f << text;
        if (!f) {
            app.status = "save failed: cannot write " + ckpt;
            return;
        }
    }
    svc::PatternFile pf = model_to_file(app.m);
    pf.checkpoint = ckpt;
    pf.stop_first = app.m.stop_first;
    std::string err;
    if (!svc::save_pattern_file(path, pf, err)) {
        app.status = "save failed: " + err;
        return;
    }
    app.session_path = path;
    app.status = "saved -> " + path + " + " + ckpt + "   (continue later: rokktui --resume " + path + ")";
}

// Keys on the search screen. While a job runs only p / s / c act; once it
// has stopped, the list scrolls and S / s / r / m / Enter work.
bool handle_result(App& app, int k) {
    if (k == 'p' && app.job_running) {
        app.want_pause = !app.want_pause;
        app.client->pause(app.job, app.want_pause);
        return true;
    }
    if (k == 's' && app.job != 0 && app.jst.state != svc::JobState::error) {
        begin_prompt(app, "Save session as (continue later with: rokktui --resume FILE):", Screen::result,
                     do_save_session);
        app.prompt_buf = app.session_path.empty() ? "session.txt" : app.session_path;
        return true;
    }
    if (k == 'r' && can_continue(app)) {
        start_search(app, app.client->checkpoint(app.job));
        return true;
    }
    if (k == 'c' && app.job_running) {
        cancel_job(app);
        app.status = "cancelling...";
        return true;
    }
    // While a search runs, p / s / c (above) are the only keys that do anything: a stray
    // Enter, Esc or q must not be able to throw away hours of work. (Ctrl+C is the
    // terminal's own kill and never reaches here.)
    if (app.job_running) return true;
    if (k == K_ENTER || k == K_ESC || k == 'q') {
        app.screen = Screen::grid;
        app.status.clear();
        return true;
    }
    const int page = app.result_rows;
    switch (k) {
        case K_UP: --app.result_scroll; break;
        case K_DOWN: ++app.result_scroll; break;
        case K_PGUP: app.result_scroll -= page; break;
        case K_PGDN: app.result_scroll += page; break;
        case K_HOME: app.result_scroll = 0; break;
        case K_END: app.result_scroll = static_cast<int>(app.matches.size()); break;
        case 'm': app.map_toggle = map_wanted(app) ? 0 : 1; break;
        case 'S':
            if (app.jst.state != svc::JobState::error)
                begin_prompt(app, "Save matches to file:", Screen::result, do_save_matches);
            break;
    }
    // draw_result clamps result_scroll to the list.
    app.result_scroll = std::max(0, app.result_scroll);
    return true;
}

// ---------------------------------------------------------------------------
// text prompt
// ---------------------------------------------------------------------------

void draw_prompt(const App& app, Frame& fr) {
    fr.line(hi(app.prompt_label));
    fr.line();
    fr.line("  " + std::string(kInv) + " " + app.prompt_buf + " " + kRst);
    fr.line();
    fr.line(dim("  Enter to confirm, Esc to cancel, Del to clear"));
}

bool handle_prompt(App& app, int k) {
    if (k == K_ESC) {
        app.screen = app.prompt_return;
        return true;
    }
    if (k == K_ENTER) {
        app.screen = app.prompt_return;
        if (app.prompt_done) app.prompt_done(app);
        return true;
    }
    edit_text(app.prompt_buf, k, false);
    return true;
}

}  // namespace

// Called at start-up (rokktui.cpp) and by cycle_backend on the params screen.
bool select_backend(App& app, const std::string& id, std::string& err) {
    try {
        auto c = svc::make_client(id);
        app.backend_label = c->backend_name();
        app.client = std::move(c);
        return true;
    } catch (const std::exception& e) {
        err = e.what();
        return false;
    }
}

bool resume_session(App& app, const std::string& path, std::string& err) {
    svc::PatternFile pf;
    if (!svc::load_pattern_file(path, pf, err)) return false;
    if (pf.checkpoint.empty()) {
        err = path + " is a plain pattern, not a saved session (no checkpoint line)";
        return false;
    }
    if (!std::ifstream(pf.checkpoint)) {
        err = "the progress file " + pf.checkpoint + " (named by " + path + ") is missing";
        return false;
    }
    file_to_model(pf, app.m);
    app.session_path = path;
    start_search(app);
    return true;
}

// Called once per tick by rokktui.cpp's redraw(), right after frame.begin().
void draw(App& app, Frame& fr) {
    app.term_cols = fr.cols();
    app.term_rows = fr.rows_left();  // nothing is drawn yet: that is the whole height
    switch (app.screen) {
        case Screen::params: draw_params(app, fr); break;
        case Screen::grid: draw_grid(app, fr); break;
        case Screen::result: draw_result(app, fr); break;
        case Screen::prompt: draw_prompt(app, fr); break;
        case Screen::splash: draw_splash(app, fr); break;
    }
}

bool handle_key(App& app, int key) {
    switch (app.screen) {
        case Screen::params: return handle_params(app, key);
        case Screen::grid: return handle_grid(app, key);
        case Screen::result: return handle_result(app, key);
        case Screen::prompt: return handle_prompt(app, key);
        case Screen::splash: return handle_splash(app, key);
    }
    return true;
}

// Called once per tick by the main loop (and by tests in a loop). poll() only
// copies a status, so this never blocks on the search. When the job reaches a
// final state the results are fetched once and job_running drops to false,
// which is what unlocks the result screen's other keys.
void refresh_job(App& app) {
    if (!app.job_running) return;
    try {
        app.jst = app.client->poll(app.job);
        track_map(app);
        if (app.jst.state == svc::JobState::done || app.jst.state == svc::JobState::cancelled ||
            app.jst.state == svc::JobState::error) {
            app.matches = app.client->results(app.job);
            app.job_running = false;
            app.want_pause = false;
            app.finished_at_ms = app.now_ms;
            if (app.jst.state == svc::JobState::cancelled) app.status = "cancelled";
        }
    } catch (const std::exception& e) {
        app.job_running = false;
        app.jst.state = svc::JobState::error;
        app.jst.error = e.what();
    }
}

void cancel_job(App& app) {
    if (!app.job_running || !app.client) return;
    try {
        app.client->cancel(app.job);
    } catch (...) {
    }
}

}  // namespace rokkdoxx::tui
