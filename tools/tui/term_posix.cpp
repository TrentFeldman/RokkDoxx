// term.hpp on Linux/macOS: termios raw mode, poll() for timed reads, and a
// small VT escape-sequence decoder.
#include "term.hpp"

#include <poll.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

#include <cerrno>
#include <csignal>
#include <cstdlib>

namespace rokkdoxx::tui {

namespace {

termios g_orig{};   // terminal settings before enter(), restored by leave()
bool g_raw = false;  // between enter() and leave()

// SIGINT/SIGTERM/SIGHUP: restore the terminal, then die of the same signal
// (default action) so the parent shell sees the usual exit status.
void on_fatal_signal(int sig) {
    leave();
    std::signal(sig, SIG_DFL);
    std::raise(sig);
}

// Read one byte, waiting at most `timeout_ms`. -1 on timeout / error.
int read_byte(int timeout_ms) {
    pollfd p{STDIN_FILENO, POLLIN, 0};
    if (poll(&p, 1, timeout_ms) <= 0) return -1;
    unsigned char c;
    if (read(STDIN_FILENO, &c, 1) != 1) return -1;
    return c;
}

// The rest of a sequence arrives in the same burst as its ESC; a lone ESC
// press is followed by nothing.
constexpr int kSeqTimeoutMs = 30;

// The final byte of "ESC [ A" / "ESC O A" style arrow and Home/End keys.
int key_for_final(int c) {
    switch (c) {
        case 'A': return K_UP;
        case 'B': return K_DOWN;
        case 'C': return K_RIGHT;
        case 'D': return K_LEFT;
        case 'H': return K_HOME;
        case 'F': return K_END;
    }
    return K_NONE;
}

// "ESC [ <n> ~" style keys (the number before any ';' modifier).
int key_for_tilde(int n) {
    switch (n) {
        case 1: case 7: return K_HOME;
        case 4: case 8: return K_END;
        case 3: return K_DELETE;
        case 5: return K_PGUP;
        case 6: return K_PGDN;
    }
    return K_NONE;
}

// Called after an ESC byte: read the rest of the sequence (if any arrives
// within kSeqTimeoutMs) and map it to a Key. Unknown sequences become K_ESC.
int decode_escape() {
    const int s0 = read_byte(kSeqTimeoutMs);
    if (s0 < 0) return K_ESC;
    if (s0 == 'O') {  // SS3: ESC O A (application cursor mode)
        const int f = read_byte(kSeqTimeoutMs);
        const int k = key_for_final(f);
        return k == K_NONE ? K_ESC : k;
    }
    if (s0 != '[') return K_ESC;

    // CSI: parameter bytes (digits, ';') then one final byte in 0x40..0x7E.
    int first_num = 0;
    bool in_first = true;
    for (int n = 0; n < 16; ++n) {
        const int c = read_byte(kSeqTimeoutMs);
        if (c < 0) return K_ESC;
        if (c >= '0' && c <= '9') {
            if (in_first) first_num = first_num * 10 + (c - '0');
            continue;
        }
        if (c == ';') {
            in_first = false;
            continue;
        }
        if (c == '~') {
            const int k = key_for_tilde(first_num);
            return k == K_NONE ? K_ESC : k;
        }
        const int k = key_for_final(c);
        return k == K_NONE ? K_ESC : k;
    }
    return K_ESC;
}

}  // namespace

bool is_interactive() { return isatty(STDIN_FILENO) && isatty(STDOUT_FILENO); }

bool enter() {
    if (g_raw) return true;
    if (tcgetattr(STDIN_FILENO, &g_orig) != 0) return false;
    termios r = g_orig;
    r.c_lflag &= ~(ECHO | ICANON | IEXTEN);  // ISIG stays on: Ctrl+C still quits
    r.c_iflag &= ~(IXON | ICRNL | BRKINT | INPCK | ISTRIP);
    r.c_oflag &= ~OPOST;
    r.c_cflag |= CS8;
    r.c_cc[VMIN] = 1;  // reads only happen after poll() says a byte is there
    r.c_cc[VTIME] = 0;
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &r) != 0) return false;
    g_raw = true;

    static bool hooks = false;
    if (!hooks) {
        hooks = true;
        std::atexit(leave);
        std::signal(SIGINT, on_fatal_signal);
        std::signal(SIGTERM, on_fatal_signal);
        std::signal(SIGHUP, on_fatal_signal);
        std::signal(SIGPIPE, SIG_IGN);
    }
    write_out("\x1b[?1049h\x1b[?25l");
    return true;
}

void leave() {
    if (!g_raw) return;
    write_out("\x1b[0m\x1b[?25h\x1b[?1049l");
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &g_orig);
    g_raw = false;
}

// Asks the kernel each call (cheap), so a resize is picked up on the next draw.
TermSize size() {
    winsize ws{};
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0 && ws.ws_row > 0)
        return {ws.ws_col, ws.ws_row};
    return {};
}

int read_key(int timeout_ms) {
    const int c = read_byte(timeout_ms);
    if (c < 0) return K_NONE;
    if (c == '\r' || c == '\n') return K_ENTER;
    if (c == '\t') return K_TAB;
    if (c == 127 || c == 8) return K_BACKSPACE;
    if (c == 27) return decode_escape();
    if (c >= 32 && c < 127) return c;
    return K_NONE;  // other control bytes / non-ASCII: ignored
}

void write_out(std::string_view bytes) {
    while (!bytes.empty()) {
        const ssize_t n = write(STDOUT_FILENO, bytes.data(), bytes.size());
        if (n < 0) {
            if (errno == EINTR) continue;
            return;
        }
        bytes.remove_prefix(static_cast<std::size_t>(n));
    }
}

}  // namespace rokkdoxx::tui
