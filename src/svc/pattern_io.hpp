// The on-disk pattern format shared by rokktui and rokksearch:
//
//   # rokkdoxx pattern
//   edition bedrock            (optional: Bedrock Edition's floor; the default is Java)
//   seed <string>
//   y <int>
//   center <x> <z>
//   radius <int>
//   orientations <all|exact>
//   checkpoint <file>          (optional: a saved session -- the progress file to resume from)
//   stop_at_first yes          (optional: end the search at the first match)
//   size <w> <h>
//   <h lines of w chars: '#' bedrock, 'o' not-bedrock, '.' unknown>
//
// Rows run north to south (z increasing), characters west to east (x
// increasing), the same orientation as the TUI grid. center / radius stay
// strings here and are parsed by the front-end (radius -1 = whole world).
// A "session" is just a pattern file with a `checkpoint` line, written by the
// TUI's `s`; rokksearch --pattern and rokktui --resume both honour it.
#pragma once

#include <string>

#include "service_types.hpp"

namespace rokkdoxx::svc {

// Everything a pattern file holds. Defaults are what a missing line means.
struct PatternFile {
    Edition edition = Edition::java;
    std::string seed = "0";
    int y = -60;
    std::string center_x = "0";
    std::string center_z = "0";
    std::string radius = "5000";
    bool all_orientations = true;
    std::string checkpoint;  // empty = none (a plain pattern; a session names its progress file)
    bool stop_first = false;
    Pattern pattern;
};

// Lenient reader: unknown header lines are treated as the start of the grid,
// missing cells are unknown, sizes are clamped to 1..kMaxDim. Only fails if the
// file can't be opened. On failure `out` is untouched and `err` says why.
bool load_pattern_file(const std::string& path, PatternFile& out, std::string& err);
// Writes `pf` (overwriting `path`). Optional lines (edition, checkpoint,
// stop_at_first) are written only when set, so a plain Java pattern file stays
// byte-identical to the original format.
bool save_pattern_file(const std::string& path, const PatternFile& pf, std::string& err);

}  // namespace rokkdoxx::svc
