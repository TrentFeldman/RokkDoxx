// rokktui's editable state and the pure logic around it -- no terminal, so
// tests/test_tui.cpp can exercise it directly.
//
// Responsibilities: the Model, converting it to/from a pattern file and a
// SearchRequest, filling it from the world, and formatting matches.
// Not this file's job: drawing or input (screens.*), the terminal (term.*).
#pragma once

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

#include "svc/pattern_io.hpp"
#include "svc/service_types.hpp"

namespace rokkdoxx::tui {

namespace svc = rokkdoxx::svc;

constexpr int kMaxDim = 32;

struct Model {
    std::string seed = "0";
    int w = 8, h = 8;
    int y = -60;
    std::string cx = "0", cz = "0", radius = "5000";
    bool all_orient = true;
    std::string checkpoint;  // empty = no checkpointing

    // Fixed kMaxDim x kMaxDim storage, so shrinking and re-growing the pattern
    // keeps what was painted. Only the top-left w x h is the pattern.
    std::vector<svc::Cell> grid =
        std::vector<svc::Cell>(static_cast<std::size_t>(kMaxDim) * kMaxDim, svc::Cell::unknown);

    svc::Cell& at(int i, int j) { return grid[static_cast<std::size_t>(j) * kMaxDim + i]; }
    svc::Cell at(int i, int j) const { return grid[static_cast<std::size_t>(j) * kMaxDim + i]; }
    void clear() { std::fill(grid.begin(), grid.end(), svc::Cell::unknown); }
};

// Whole-string integer parse; false on junk or overflow.
bool parse_i64(const std::string& s, long long& out);

svc::PatternFile model_to_file(const Model& m);
void file_to_model(const svc::PatternFile& pf, Model& m);

// The search the model describes. False + `err` if it can't run as-is.
bool build_request(const Model& m, svc::SearchRequest& req, std::string& err);

// Paint the w x h pattern from the real world at (center X, center Z) -- a
// round-trip test: searching for it must find that spot.
bool fill_from_world(Model& m, std::string& err);

// Chance a block at this Y of the bedrock floor is bedrock.
double bedrock_probability(int y);

// Candidate origins the search area covers; -1 if the radius isn't a valid
// integer.
double search_candidates(const Model& m);

// One "x z mask" line per match -- byte-identical to rokksearch's stdout.
std::string format_matches(const std::vector<svc::Match>& matches);

// A header comment for a saved matches file.
std::string matches_header(const Model& m);

// "id r90 m+r180" etc. for the bits set in an orientation mask.
std::string orient_names(std::uint8_t mask);

}  // namespace rokkdoxx::tui
