// rokktui -- interactive front-end for RokkDoxx.
//
//   * enter a world seed (numeric, or a text seed hashed the way Minecraft does)
//   * choose the pattern size (width x height), the Y layer, and orientations
//   * pick the compute backend and, optionally, a checkpoint file to resume from
//   * paint the bedrock pattern on a grid (bedrock / not-bedrock / unknown)
//   * run the search and watch progress; list every match
//
// The search itself lives in librokksvc (SearchService + a compute Worker),
// run in-process. The TUI is split across tools/tui/:
//   term.hpp + term_posix.cpp / term_win.cpp   raw terminal, per OS
//   frame.*                                     diffed screen output
//   model.*                                     editable state + pure logic
//   screens.*                                   drawing + key handling
// This file only parses arguments and runs the loop.
#include <cstdio>
#include <string>

#include "tui/frame.hpp"
#include "tui/screens.hpp"
#include "tui/term.hpp"

namespace tui = rokkdoxx::tui;
namespace svc = rokkdoxx::svc;

int main(int argc, char** argv) {
    tui::App app;
    std::string backend, load_path;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "-h" || a == "--help") {
            std::puts(
                "rokktui -- interactive bedrock pattern search\n"
                "  --load FILE        preload a pattern file\n"
                "  --backend ID       cpu | opencl:N | auto (can also be changed on screen)\n"
                "  --checkpoint FILE  resumable progress file (can also be set on screen)\n"
                "Requires an interactive terminal. Controls are shown on screen.");
            return 0;
        }
        if (a == "--load" && i + 1 < argc) load_path = argv[++i];
        else if (a == "--backend" && i + 1 < argc) backend = argv[++i];
        else if (a == "--checkpoint" && i + 1 < argc) app.m.checkpoint = argv[++i];
        else {
            std::fprintf(stderr, "unknown argument: %s (try --help)\n", a.c_str());
            return 1;
        }
    }

    if (!load_path.empty()) {
        svc::PatternFile pf;
        std::string err;
        if (!svc::load_pattern_file(load_path, pf, err)) {
            std::fprintf(stderr, "load failed: %s\n", err.c_str());
            return 1;
        }
        tui::file_to_model(pf, app.m);
    }

    app.backends = svc::list_backends();
    {
        std::string err;
        if (!tui::select_backend(app, backend, err)) {
            std::fprintf(stderr, "backend error: %s\n", err.c_str());
            return 1;
        }
        for (std::size_t i = 0; i < app.backends.size(); ++i)
            if (app.backends[i].id == backend) app.backend_idx = static_cast<int>(i);
    }

    if (!tui::is_interactive()) {
        std::fprintf(stderr, "rokktui needs an interactive terminal.\n");
        return 1;
    }
    if (!tui::enter()) {
        std::fprintf(stderr, "rokktui: could not put the terminal in raw mode.\n");
        return 1;
    }

    // Redraw every tick: the Frame diff makes an unchanged screen cost nothing,
    // and re-reading the size each time catches resizes on every platform.
    tui::Frame frame;
    auto redraw = [&] {
        frame.begin(tui::size());
        tui::draw(app, frame);
        tui::write_out(frame.flush());
    };
    redraw();
    for (;;) {
        const int k = tui::read_key(100);
        tui::refresh_job(app);
        if (k == tui::K_RESIZE) frame.invalidate();
        else if (k != tui::K_NONE && !tui::handle_key(app, k)) break;
        redraw();
    }
    tui::cancel_job(app);
    tui::leave();
    return 0;
}
