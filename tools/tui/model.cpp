#include "model.hpp"

#include <algorithm>
#include <cstdio>

#include "gen/bedrock.hpp"

namespace rokkdoxx::tui {

bool parse_i64(const std::string& s, long long& out) {
    try {
        std::size_t pos = 0;
        out = std::stoll(s, &pos);
        return pos == s.size();
    } catch (...) {
        return false;
    }
}

svc::PatternFile model_to_file(const Model& m) {
    svc::PatternFile pf;
    pf.seed = m.seed;
    pf.y = m.y;
    pf.center_x = m.cx;
    pf.center_z = m.cz;
    pf.radius = m.radius;
    pf.all_orientations = m.all_orient;
    pf.pattern.w = m.w;
    pf.pattern.h = m.h;
    pf.pattern.cells.assign(static_cast<std::size_t>(m.w) * m.h, svc::Cell::unknown);
    for (int j = 0; j < m.h; ++j)
        for (int i = 0; i < m.w; ++i)
            pf.pattern.cells[static_cast<std::size_t>(j) * m.w + i] = m.at(i, j);
    return pf;
}

void file_to_model(const svc::PatternFile& pf, Model& m) {
    m.seed = pf.seed;
    m.y = pf.y;
    m.cx = pf.center_x;
    m.cz = pf.center_z;
    m.radius = pf.radius;
    m.all_orient = pf.all_orientations;
    m.w = std::clamp(pf.pattern.w, 1, kMaxDim);
    m.h = std::clamp(pf.pattern.h, 1, kMaxDim);
    m.clear();
    for (int j = 0; j < m.h; ++j)
        for (int i = 0; i < m.w; ++i) m.at(i, j) = pf.pattern.at(i, j);
}

bool build_request(const Model& m, svc::SearchRequest& req, std::string& err) {
    long long cx, cz, rad;
    if (!parse_i64(m.cx, cx) || !parse_i64(m.cz, cz) || !parse_i64(m.radius, rad) || rad < 0) {
        err = "center X/Z and radius must be integers (radius >= 0)";
        return false;
    }
    req.seed = svc::seed_from_string(m.seed);
    req.plane_y = m.y;
    req.pattern = model_to_file(m).pattern;
    if (req.pattern.knowns().empty()) {
        err = "pattern has no known cells -- paint some bedrock first";
        return false;
    }
    req.region = svc::Region::centered(cx, cz, rad);
    req.all_orientations = m.all_orient;
    req.match_cap = 1u << 20;
    req.checkpoint_path = m.checkpoint;
    return true;
}

bool fill_from_world(Model& m, std::string& err) {
    long long cx = 0, cz = 0;
    if (!parse_i64(m.cx, cx) || !parse_i64(m.cz, cz)) {
        err = "set integer center X/Z first";
        return false;
    }
    BedrockGenerator gen(svc::seed_from_string(m.seed));
    for (int j = 0; j < m.h; ++j)
        for (int i = 0; i < m.w; ++i)
            m.at(i, j) = gen.is_bedrock_floor(static_cast<int>(cx + i), m.y, static_cast<int>(cz + j))
                             ? svc::Cell::bedrock
                             : svc::Cell::not_bedrock;
    return true;
}

double bedrock_probability(int y) {
    if (y <= -64) return 1.0;
    if (y >= -59) return 0.0;
    return 1.0 - (y + 64) / 5.0;
}

double search_candidates(const Model& m) {
    long long r = 0;
    if (!parse_i64(m.radius, r) || r < 0) return -1;
    const double side = 2.0 * static_cast<double>(r) + 1.0;
    return side * side;
}

std::string format_matches(const std::vector<svc::Match>& matches) {
    std::string out;
    char b[64];
    for (const auto& mm : matches) {
        std::snprintf(b, sizeof(b), "%d %d %u\n", mm.x, mm.z, mm.orient_mask);
        out += b;
    }
    return out;
}

std::string matches_header(const Model& m) {
    return "# rokkdoxx matches  seed=" + m.seed + " y=" + std::to_string(m.y) +
           " size=" + std::to_string(m.w) + "x" + std::to_string(m.h) +
           "  (x z = anchor cell, ~pattern centre; mask bit g = orientation g)\n";
}

std::string orient_names(std::uint8_t mask) {
    static const char* const kNames[8] = {"id", "r90", "r180", "r270",
                                          "m",  "m+r90", "m+r180", "m+r270"};
    std::string out;
    for (int g = 0; g < 8; ++g)
        if (mask & (1u << g)) {
            if (!out.empty()) out += ' ';
            out += kNames[g];
        }
    return out;
}

}  // namespace rokkdoxx::tui
