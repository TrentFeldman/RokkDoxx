// A double-buffered screen: each draw builds the full list of lines, and
// flush() emits only the lines that changed since the last flush. That is what
// stops the flicker the old clear-and-redraw-everything loop had.
//
// Lines may contain ANSI SGR colour codes; they are clipped to the terminal
// width by *visible* characters (escape codes and UTF-8 continuation bytes
// don't count), so a long line never wraps and shifts the rows below it.
//
// Responsibilities: line buffering, width/height clipping, the diff.
// Not this file's job: talking to the terminal (term.hpp) or what is drawn.
#pragma once

#include <string>
#include <vector>

#include "term.hpp"

namespace rokkdoxx::tui {

// Clip `s` to at most `cols` visible characters; appends a colour reset if it
// had to cut.
std::string clip_visible(const std::string& s, int cols);

class Frame {
public:
    // Start a new frame for a terminal of this size. A size change forces the
    // next flush to repaint everything.
    void begin(TermSize size);

    // Append text to the current line / finish the current line.
    void put(const std::string& s) { partial_ += s; }
    void line(const std::string& s = "");

    // Rows still free below what has been drawn so far.
    int rows_left() const;
    int cols() const { return size_.cols; }

    // Force the next flush to repaint the whole screen.
    void invalidate() { full_ = true; }

    // The bytes to write to bring the terminal from the previous frame to this
    // one. Empty if nothing changed.
    std::string flush();

private:
    std::vector<std::string> cur_, prev_;
    std::string partial_;
    TermSize size_{}, prev_size_{};
    bool full_ = true;
};

}  // namespace rokkdoxx::tui
