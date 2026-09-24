// term.hpp on Windows 10+: the console API for input and sizing, with VT
// processing switched on so the shared ANSI drawing code works unchanged.
//
// Untested on a real Windows box at time of writing (see README).
#include "term.hpp"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <cstdlib>

#ifndef ENABLE_VIRTUAL_TERMINAL_PROCESSING
#  define ENABLE_VIRTUAL_TERMINAL_PROCESSING 0x0004
#endif

namespace rokkdoxx::tui {

namespace {

HANDLE g_in = INVALID_HANDLE_VALUE;
HANDLE g_out = INVALID_HANDLE_VALUE;
DWORD g_orig_in = 0, g_orig_out = 0;
UINT g_orig_cp = 0;
bool g_raw = false;

// Ctrl+C / close: put the console back, then let the default handler end the
// process (returning FALSE).
BOOL WINAPI on_ctrl(DWORD) {
    leave();
    return FALSE;
}

int map_key(const KEY_EVENT_RECORD& k) {
    switch (k.wVirtualKeyCode) {
        case VK_UP: return K_UP;
        case VK_DOWN: return K_DOWN;
        case VK_LEFT: return K_LEFT;
        case VK_RIGHT: return K_RIGHT;
        case VK_RETURN: return K_ENTER;
        case VK_BACK: return K_BACKSPACE;
        case VK_ESCAPE: return K_ESC;
        case VK_TAB: return K_TAB;
        case VK_HOME: return K_HOME;
        case VK_END: return K_END;
        case VK_DELETE: return K_DELETE;
        case VK_PRIOR: return K_PGUP;
        case VK_NEXT: return K_PGDN;
    }
    const wchar_t ch = k.uChar.UnicodeChar;
    if (ch >= 32 && ch < 127) return static_cast<int>(ch);
    return K_NONE;
}

}  // namespace

bool is_interactive() {
    DWORD mode;
    return GetConsoleMode(GetStdHandle(STD_INPUT_HANDLE), &mode) &&
           GetConsoleMode(GetStdHandle(STD_OUTPUT_HANDLE), &mode);
}

bool enter() {
    if (g_raw) return true;
    g_in = GetStdHandle(STD_INPUT_HANDLE);
    g_out = GetStdHandle(STD_OUTPUT_HANDLE);
    if (!GetConsoleMode(g_in, &g_orig_in) || !GetConsoleMode(g_out, &g_orig_out)) return false;
    if (!SetConsoleMode(g_out, g_orig_out | ENABLE_VIRTUAL_TERMINAL_PROCESSING))
        return false;  // pre-Windows-10 console: no VT support
    // No line buffering / echo; keep ENABLE_PROCESSED_INPUT so Ctrl+C still
    // reaches on_ctrl; ENABLE_WINDOW_INPUT reports resizes.
    if (!SetConsoleMode(g_in, ENABLE_PROCESSED_INPUT | ENABLE_WINDOW_INPUT)) {
        SetConsoleMode(g_out, g_orig_out);
        return false;
    }
    g_orig_cp = GetConsoleOutputCP();
    SetConsoleOutputCP(CP_UTF8);
    g_raw = true;

    static bool hooks = false;
    if (!hooks) {
        hooks = true;
        std::atexit(leave);
        SetConsoleCtrlHandler(on_ctrl, TRUE);
    }
    write_out("\x1b[?1049h\x1b[?25l");
    return true;
}

void leave() {
    if (!g_raw) return;
    write_out("\x1b[0m\x1b[?25h\x1b[?1049l");
    SetConsoleMode(g_in, g_orig_in);
    SetConsoleMode(g_out, g_orig_out);
    if (g_orig_cp) SetConsoleOutputCP(g_orig_cp);
    g_raw = false;
}

TermSize size() {
    CONSOLE_SCREEN_BUFFER_INFO info;
    if (GetConsoleScreenBufferInfo(GetStdHandle(STD_OUTPUT_HANDLE), &info)) {
        const int cols = info.srWindow.Right - info.srWindow.Left + 1;
        const int rows = info.srWindow.Bottom - info.srWindow.Top + 1;
        if (cols > 0 && rows > 0) return {cols, rows};
    }
    return {};
}

int read_key(int timeout_ms) {
    const ULONGLONG deadline = GetTickCount64() + static_cast<ULONGLONG>(timeout_ms);
    for (;;) {
        const ULONGLONG now = GetTickCount64();
        const DWORD wait = now >= deadline ? 0 : static_cast<DWORD>(deadline - now);
        if (WaitForSingleObject(g_in, wait) != WAIT_OBJECT_0) return K_NONE;

        INPUT_RECORD rec;
        DWORD n = 0;
        if (!ReadConsoleInputW(g_in, &rec, 1, &n) || n == 0) return K_NONE;
        if (rec.EventType == WINDOW_BUFFER_SIZE_EVENT) return K_RESIZE;
        if (rec.EventType == KEY_EVENT && rec.Event.KeyEvent.bKeyDown) {
            const int k = map_key(rec.Event.KeyEvent);
            if (k != K_NONE) return k;
        }
        // mouse/focus/menu events, key-ups, bare modifiers: keep waiting
    }
}

void write_out(std::string_view bytes) {
    HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    while (!bytes.empty()) {
        DWORD n = 0;
        if (!WriteFile(out, bytes.data(), static_cast<DWORD>(bytes.size()), &n, nullptr) || n == 0)
            return;
        bytes.remove_prefix(n);
    }
}

}  // namespace rokkdoxx::tui
