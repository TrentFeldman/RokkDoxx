// The terminal seam for rokktui: raw keyboard input, the screen size, and
// writing bytes. Everything above this file speaks ANSI/VT escape codes, which
// both platforms understand (Windows 10+ once VT processing is switched on), so
// only input and mode switching differ per OS.
//
// Responsibilities: the Key vocabulary + the tiny terminal interface.
// Implementations: term_posix.cpp (termios) and term_win.cpp (console API) --
// the build picks exactly one. They are the only platform-specific code in the
// TUI.
#pragma once

#include <string_view>

namespace rokkdoxx::tui {

// Printable ASCII comes back as itself (32..126); everything else is one of
// these negative codes.
enum Key : int {
    K_NONE = -1,  // timed out, nothing pressed
    K_UP = -2,
    K_DOWN = -3,
    K_LEFT = -4,
    K_RIGHT = -5,
    K_ENTER = -6,
    K_BACKSPACE = -7,
    K_ESC = -8,
    K_TAB = -9,
    K_HOME = -10,
    K_END = -11,
    K_DELETE = -12,
    K_PGUP = -13,
    K_PGDN = -14,
    K_RESIZE = -15,  // the window changed size; redraw everything
};

struct TermSize {
    int cols = 80, rows = 24;
    bool operator==(const TermSize&) const = default;
};

// True when both stdin and stdout are an interactive terminal/console.
bool is_interactive();

// Raw input, alternate screen, hidden cursor. Installs hooks (atexit, signals /
// console ctrl handler) that undo it if the process dies. Returns false (and
// changes nothing) if the terminal can't be put in that mode.
bool enter();

// Undo enter(). Safe to call more than once.
void leave();

TermSize size();

// Wait up to `timeout_ms` for a key. Returns K_NONE on timeout.
int read_key(int timeout_ms);

// Write bytes straight to the terminal (no CRT newline translation).
void write_out(std::string_view bytes);

}  // namespace rokkdoxx::tui
