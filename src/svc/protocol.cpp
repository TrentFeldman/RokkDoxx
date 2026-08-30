#include "protocol.hpp"

#include <unistd.h>

#include <cstdint>
#include <cstring>

namespace rokkdoxx::svc {

namespace {

std::string cells_to_str(const Pattern& p) {
    std::string s;
    s.reserve(p.cells.size());
    for (Cell c : p.cells) s += (c == Cell::bedrock ? '#' : c == Cell::not_bedrock ? 'o' : '.');
    return s;
}

}  // namespace

Json request_to_json(const SearchRequest& r) {
    Json j = Json::object();
    j.obj["seed"] = Json(static_cast<long long>(r.seed));
    j.obj["y"] = Json(r.plane_y);
    j.obj["w"] = Json(r.pattern.w);
    j.obj["h"] = Json(r.pattern.h);
    j.obj["cells"] = Json(cells_to_str(r.pattern));
    j.obj["x0"] = Json(static_cast<long long>(r.region.x0));
    j.obj["x1"] = Json(static_cast<long long>(r.region.x1));
    j.obj["z0"] = Json(static_cast<long long>(r.region.z0));
    j.obj["z1"] = Json(static_cast<long long>(r.region.z1));
    j.obj["all_orient"] = Json(r.all_orientations);
    j.obj["cap"] = Json(static_cast<long long>(r.match_cap));
    j.obj["tile"] = Json(r.tile_side);
    j.obj["checkpoint"] = Json(r.checkpoint_path);
    return j;
}

bool request_from_json(const Json& j, SearchRequest& out) {
    if (!j.is_obj()) return false;
    auto g = [&](const char* k) { return j.find(k); };
    if (!g("seed") || !g("cells") || !g("w")) return false;
    out.seed = g("seed")->as_int();
    out.plane_y = static_cast<int>(g("y") ? g("y")->as_int(-60) : -60);
    out.pattern.w = static_cast<int>(g("w")->as_int());
    out.pattern.h = static_cast<int>(g("h") ? g("h")->as_int() : 0);
    const std::string cells = g("cells")->as_str();
    out.pattern.cells.resize(cells.size());
    for (std::size_t i = 0; i < cells.size(); ++i)
        out.pattern.cells[i] = cells[i] == '#'   ? Cell::bedrock
                               : cells[i] == 'o' ? Cell::not_bedrock
                                                 : Cell::unknown;
    if (out.pattern.h == 0 && out.pattern.w > 0)
        out.pattern.h = static_cast<int>(cells.size()) / out.pattern.w;
    out.region.x0 = g("x0") ? g("x0")->as_int() : 0;
    out.region.x1 = g("x1") ? g("x1")->as_int() : 0;
    out.region.z0 = g("z0") ? g("z0")->as_int() : 0;
    out.region.z1 = g("z1") ? g("z1")->as_int() : 0;
    out.all_orientations = g("all_orient") ? g("all_orient")->as_bool(true) : true;
    out.match_cap = static_cast<std::uint32_t>(g("cap") ? g("cap")->as_u64(1u << 20) : (1u << 20));
    out.tile_side = static_cast<int>(g("tile") ? g("tile")->as_int(4096) : 4096);
    out.checkpoint_path = g("checkpoint") ? g("checkpoint")->as_str() : "";
    return true;
}

Json status_to_json(const JobStatus& s) {
    Json j = Json::object();
    j.obj["state"] = Json(std::string(to_string(s.state)));
    j.obj["progress"] = Json(s.progress);
    j.obj["total"] = Json(static_cast<long long>(s.candidates_total));
    j.obj["done"] = Json(static_cast<long long>(s.candidates_done));
    j.obj["matches"] = Json(static_cast<long long>(s.matches));
    j.obj["elapsed"] = Json(s.elapsed_s);
    j.obj["rate"] = Json(s.rate);
    j.obj["truncated"] = Json(s.truncated);
    j.obj["error"] = Json(s.error);
    return j;
}

JobStatus status_from_json(const Json& j) {
    JobStatus s;
    const std::string st = j.find("state") ? j.find("state")->as_str() : "error";
    s.state = st == "pending"     ? JobState::pending
              : st == "running"   ? JobState::running
              : st == "done"      ? JobState::done
              : st == "cancelled" ? JobState::cancelled
                                  : JobState::error;
    s.progress = j.find("progress") ? j.find("progress")->as_num() : 0.0;
    s.candidates_total = j.find("total") ? j.find("total")->as_int() : 0;
    s.candidates_done = j.find("done") ? j.find("done")->as_int() : 0;
    s.matches = j.find("matches") ? j.find("matches")->as_u64() : 0;
    s.elapsed_s = j.find("elapsed") ? j.find("elapsed")->as_num() : 0.0;
    s.rate = j.find("rate") ? j.find("rate")->as_num() : 0.0;
    s.truncated = j.find("truncated") ? j.find("truncated")->as_bool() : false;
    s.error = j.find("error") ? j.find("error")->as_str() : "";
    return s;
}

Json matches_to_json(const std::vector<Match>& m) {
    Json a = Json::array();
    for (const Match& x : m) {
        Json e = Json::array();
        e.arr.push_back(Json(x.x));
        e.arr.push_back(Json(x.z));
        e.arr.push_back(Json(static_cast<int>(x.orient_mask)));
        a.arr.push_back(std::move(e));
    }
    return a;
}

std::vector<Match> matches_from_json(const Json& j) {
    std::vector<Match> out;
    if (j.type != Json::Arr) return out;
    for (const Json& e : j.arr) {
        if (e.type != Json::Arr || e.arr.size() < 3) continue;
        out.push_back({static_cast<int>(e.arr[0].as_int()), static_cast<int>(e.arr[1].as_int()),
                       static_cast<std::uint8_t>(e.arr[2].as_int())});
    }
    return out;
}

// --- framed socket I/O ---

static bool write_all(int fd, const char* p, std::size_t n) {
    while (n) {
        ssize_t w = ::write(fd, p, n);
        if (w <= 0) return false;
        p += w;
        n -= static_cast<std::size_t>(w);
    }
    return true;
}

static bool read_all(int fd, char* p, std::size_t n) {
    while (n) {
        ssize_t r = ::read(fd, p, n);
        if (r <= 0) return false;
        p += r;
        n -= static_cast<std::size_t>(r);
    }
    return true;
}

bool send_message(int fd, const std::string& payload) {
    std::uint32_t len = static_cast<std::uint32_t>(payload.size());
    unsigned char hdr[4] = {static_cast<unsigned char>(len & 0xFF),
                            static_cast<unsigned char>((len >> 8) & 0xFF),
                            static_cast<unsigned char>((len >> 16) & 0xFF),
                            static_cast<unsigned char>((len >> 24) & 0xFF)};
    if (!write_all(fd, reinterpret_cast<char*>(hdr), 4)) return false;
    return write_all(fd, payload.data(), payload.size());
}

bool recv_message(int fd, std::string& payload) {
    unsigned char hdr[4];
    if (!read_all(fd, reinterpret_cast<char*>(hdr), 4)) return false;
    std::uint32_t len = static_cast<std::uint32_t>(hdr[0]) | (static_cast<std::uint32_t>(hdr[1]) << 8) |
                        (static_cast<std::uint32_t>(hdr[2]) << 16) |
                        (static_cast<std::uint32_t>(hdr[3]) << 24);
    if (len > (64u << 20)) return false;  // 64 MB sanity cap
    payload.resize(len);
    return len == 0 || read_all(fd, payload.data(), len);
}

}  // namespace rokkdoxx::svc
