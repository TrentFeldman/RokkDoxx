// The rokkd wire protocol, plus the tiny JSON value type it is built on.
//
// Framing: every message is a 4-byte little-endian length followed by that many
// bytes of JSON. One request and one response per connection.
//
//   -> {"op":"submit","req":{...}}    <- {"ok":true,"job":N}
//   -> {"op":"poll","job":N}          <- {"ok":true,"status":{...}}
//   -> {"op":"results","job":N}       <- {"ok":true,"matches":[[x,z,mask],...],"truncated":bool}
//   -> {"op":"cancel","job":N}        <- {"ok":true}
//   -> {"op":"backend"}               <- {"ok":true,"backend":"..."}
//   (any error)                       <- {"ok":false,"error":"..."}
//
// Responsibilities: the Json type, the request/status/matches <-> Json
// conversions, and framed socket read/write.
// Not this file's job: the serve loop (daemon.*) or the client side (client.*).
#pragma once

#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "service_types.hpp"

namespace rokkdoxx::svc {

// --- minimal JSON ---------------------------------------------------------
//
// Just enough JSON for the fixed, small message shapes above -- this is what
// keeps the project dependency-free. Not a general-purpose library.
struct Json {
    enum Type { Null, Bool, Num, Str, Arr, Obj };
    Type type = Null;
    bool b = false;
    double num = 0;
    std::string str;
    std::vector<Json> arr;
    std::map<std::string, Json> obj;

    Json() = default;
    Json(bool v) : type(Bool), b(v) {}
    Json(double v) : type(Num), num(v) {}
    Json(long long v) : type(Num), num(static_cast<double>(v)) {}
    Json(int v) : type(Num), num(v) {}
    Json(const char* v) : type(Str), str(v) {}
    Json(std::string v) : type(Str), str(std::move(v)) {}

    static Json array() {
        Json j;
        j.type = Arr;
        return j;
    }
    static Json object() {
        Json j;
        j.type = Obj;
        return j;
    }

    bool is_obj() const { return type == Obj; }
    const Json* find(const std::string& k) const {
        if (type != Obj) return nullptr;
        auto it = obj.find(k);
        return it == obj.end() ? nullptr : &it->second;
    }
    long long as_int(long long d = 0) const { return type == Num ? static_cast<long long>(num) : d; }
    std::uint64_t as_u64(std::uint64_t d = 0) const {
        return type == Num ? static_cast<std::uint64_t>(num) : d;
    }
    double as_num(double d = 0) const { return type == Num ? num : d; }
    bool as_bool(bool d = false) const { return type == Bool ? b : d; }
    std::string as_str(const std::string& d = "") const { return type == Str ? str : d; }

    // --- serialize ---
    void dump(std::string& out) const {
        switch (type) {
            case Null: out += "null"; break;
            case Bool: out += b ? "true" : "false"; break;
            case Num: {
                if (std::floor(num) == num && std::fabs(num) < 9e15) {
                    out += std::to_string(static_cast<long long>(num));
                } else {
                    std::ostringstream ss;
                    ss.precision(17);
                    ss << num;
                    out += ss.str();
                }
                break;
            }
            case Str: dump_str(str, out); break;
            case Arr: {
                out += '[';
                for (std::size_t i = 0; i < arr.size(); ++i) {
                    if (i) out += ',';
                    arr[i].dump(out);
                }
                out += ']';
                break;
            }
            case Obj: {
                out += '{';
                bool first = true;
                for (const auto& [k, v] : obj) {
                    if (!first) out += ',';
                    first = false;
                    dump_str(k, out);
                    out += ':';
                    v.dump(out);
                }
                out += '}';
                break;
            }
        }
    }
    std::string dump() const {
        std::string s;
        dump(s);
        return s;
    }

    // --- parse ---
    static bool parse(const std::string& in, Json& out) {
        std::size_t i = 0;
        skip_ws(in, i);
        if (!parse_value(in, i, out)) return false;
        skip_ws(in, i);
        return i == in.size();
    }

private:
    static void dump_str(const std::string& s, std::string& out) {
        out += '"';
        for (char c : s) {
            switch (c) {
                case '"': out += "\\\""; break;
                case '\\': out += "\\\\"; break;
                case '\n': out += "\\n"; break;
                case '\r': out += "\\r"; break;
                case '\t': out += "\\t"; break;
                default:
                    if (static_cast<unsigned char>(c) < 0x20) {
                        char buf[8];
                        std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                        out += buf;
                    } else {
                        out += c;
                    }
            }
        }
        out += '"';
    }
    static void skip_ws(const std::string& s, std::size_t& i) {
        while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r')) ++i;
    }
    static bool parse_value(const std::string& s, std::size_t& i, Json& out) {
        skip_ws(s, i);
        if (i >= s.size()) return false;
        char c = s[i];
        if (c == '{') return parse_obj(s, i, out);
        if (c == '[') return parse_arr(s, i, out);
        if (c == '"') {
            out.type = Str;
            return parse_str(s, i, out.str);
        }
        if (c == 't' || c == 'f') {
            if (s.compare(i, 4, "true") == 0) {
                out = Json(true);
                i += 4;
                return true;
            }
            if (s.compare(i, 5, "false") == 0) {
                out = Json(false);
                i += 5;
                return true;
            }
            return false;
        }
        if (c == 'n') {
            if (s.compare(i, 4, "null") == 0) {
                out = Json();
                i += 4;
                return true;
            }
            return false;
        }
        // number
        std::size_t start = i;
        while (i < s.size() && (std::isdigit(static_cast<unsigned char>(s[i])) || s[i] == '-' ||
                                s[i] == '+' || s[i] == '.' || s[i] == 'e' || s[i] == 'E'))
            ++i;
        if (i == start) return false;
        try {
            out = Json(std::stod(s.substr(start, i - start)));
        } catch (...) {
            return false;
        }
        return true;
    }
    static bool parse_str(const std::string& s, std::size_t& i, std::string& out) {
        if (s[i] != '"') return false;
        ++i;
        out.clear();
        while (i < s.size()) {
            char c = s[i++];
            if (c == '"') return true;
            if (c == '\\') {
                if (i >= s.size()) return false;
                char e = s[i++];
                switch (e) {
                    case '"': out += '"'; break;
                    case '\\': out += '\\'; break;
                    case '/': out += '/'; break;
                    case 'n': out += '\n'; break;
                    case 'r': out += '\r'; break;
                    case 't': out += '\t'; break;
                    case 'b': out += '\b'; break;
                    case 'f': out += '\f'; break;
                    case 'u': {
                        if (i + 4 > s.size()) return false;
                        int cp = std::stoi(s.substr(i, 4), nullptr, 16);
                        i += 4;
                        if (cp < 0x80) {
                            out += static_cast<char>(cp);
                        } else if (cp < 0x800) {
                            out += static_cast<char>(0xC0 | (cp >> 6));
                            out += static_cast<char>(0x80 | (cp & 0x3F));
                        } else {
                            out += static_cast<char>(0xE0 | (cp >> 12));
                            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
                            out += static_cast<char>(0x80 | (cp & 0x3F));
                        }
                        break;
                    }
                    default: return false;
                }
            } else {
                out += c;
            }
        }
        return false;
    }
    static bool parse_arr(const std::string& s, std::size_t& i, Json& out) {
        out = array();
        ++i;  // [
        skip_ws(s, i);
        if (i < s.size() && s[i] == ']') {
            ++i;
            return true;
        }
        for (;;) {
            Json v;
            if (!parse_value(s, i, v)) return false;
            out.arr.push_back(std::move(v));
            skip_ws(s, i);
            if (i >= s.size()) return false;
            if (s[i] == ',') {
                ++i;
                continue;
            }
            if (s[i] == ']') {
                ++i;
                return true;
            }
            return false;
        }
    }
    static bool parse_obj(const std::string& s, std::size_t& i, Json& out) {
        out = object();
        ++i;  // {
        skip_ws(s, i);
        if (i < s.size() && s[i] == '}') {
            ++i;
            return true;
        }
        for (;;) {
            skip_ws(s, i);
            std::string key;
            if (!parse_str(s, i, key)) return false;
            skip_ws(s, i);
            if (i >= s.size() || s[i] != ':') return false;
            ++i;
            Json v;
            if (!parse_value(s, i, v)) return false;
            out.obj.emplace(std::move(key), std::move(v));
            skip_ws(s, i);
            if (i >= s.size()) return false;
            if (s[i] == ',') {
                ++i;
                continue;
            }
            if (s[i] == '}') {
                ++i;
                return true;
            }
            return false;
        }
    }
};

// --- protocol conversions ----------------------------------------------

Json request_to_json(const SearchRequest& r);
bool request_from_json(const Json& j, SearchRequest& out);

Json status_to_json(const JobStatus& s);
JobStatus status_from_json(const Json& j);

Json matches_to_json(const std::vector<Match>& m);
std::vector<Match> matches_from_json(const Json& j);

// Framed I/O on a connected socket fd. Return false on EOF / error.
bool send_message(int fd, const std::string& payload);
bool recv_message(int fd, std::string& payload);

}  // namespace rokkdoxx::svc
