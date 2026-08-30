#include "pattern_io.hpp"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <sstream>

namespace rokkdoxx::svc {

namespace {
constexpr int kMaxDim = 32;
}

bool load_pattern_file(const std::string& path, PatternFile& out, std::string& err) {
    std::ifstream f(path);
    if (!f) {
        err = "cannot open " + path;
        return false;
    }
    PatternFile pf;
    std::vector<std::string> rows;
    int declared_w = 0, declared_h = 0;
    // Grid rows use '#' for bedrock, so '#' is only a comment marker in the
    // header (before the "size" line).
    bool in_grid = false;
    std::string line;
    while (std::getline(f, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;
        if (in_grid) {
            rows.push_back(line);
            continue;
        }
        if (line[0] == '#') continue;
        std::istringstream ls(line);
        std::string kw;
        ls >> kw;
        if (kw == "seed") {
            ls >> std::ws;
            std::getline(ls, pf.seed);
        } else if (kw == "y") {
            ls >> pf.y;
        } else if (kw == "center") {
            ls >> pf.center_x >> pf.center_z;
        } else if (kw == "radius") {
            ls >> pf.radius;
        } else if (kw == "orientations") {
            std::string v;
            ls >> v;
            pf.all_orientations = (v != "exact");
        } else if (kw == "size") {
            ls >> declared_w >> declared_h;
            in_grid = true;
        } else {
            // an unrecognised header line before "size" -- treat as a grid row
            // (tolerates size-less files)
            rows.push_back(line);
            in_grid = true;
        }
    }

    int w = declared_w;
    int h = declared_h ? declared_h : static_cast<int>(rows.size());
    if (w <= 0)
        for (const auto& r : rows) w = std::max(w, static_cast<int>(r.size()));
    w = std::clamp(w, 1, kMaxDim);
    h = std::clamp(h, 1, kMaxDim);

    pf.pattern.w = w;
    pf.pattern.h = h;
    pf.pattern.cells.assign(static_cast<std::size_t>(w) * h, Cell::unknown);
    for (int j = 0; j < h && j < static_cast<int>(rows.size()); ++j) {
        const std::string& r = rows[static_cast<std::size_t>(j)];
        for (int i = 0; i < w && i < static_cast<int>(r.size()); ++i) {
            Cell c = r[i] == '#' ? Cell::bedrock : r[i] == 'o' ? Cell::not_bedrock : Cell::unknown;
            pf.pattern.cells[static_cast<std::size_t>(j) * w + i] = c;
        }
    }
    out = std::move(pf);
    return true;
}

bool save_pattern_file(const std::string& path, const PatternFile& pf, std::string& err) {
    std::ofstream f(path, std::ios::trunc);
    if (!f) {
        err = "cannot open " + path;
        return false;
    }
    f << "# rokkdoxx pattern\n";
    f << "seed " << pf.seed << "\n";
    f << "y " << pf.y << "\n";
    f << "center " << pf.center_x << " " << pf.center_z << "\n";
    f << "radius " << pf.radius << "\n";
    f << "orientations " << (pf.all_orientations ? "all" : "exact") << "\n";
    f << "size " << pf.pattern.w << " " << pf.pattern.h << "\n";
    for (int j = 0; j < pf.pattern.h; ++j) {
        for (int i = 0; i < pf.pattern.w; ++i) {
            Cell c = pf.pattern.at(i, j);
            f << (c == Cell::bedrock ? '#' : c == Cell::not_bedrock ? 'o' : '.');
        }
        f << "\n";
    }
    return static_cast<bool>(f);
}

}  // namespace rokkdoxx::svc
