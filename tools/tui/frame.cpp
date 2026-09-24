#include "frame.hpp"

#include <algorithm>

namespace rokkdoxx::tui {

std::string clip_visible(const std::string& s, int cols) {
    std::string out;
    int visible = 0;
    std::size_t i = 0;
    while (i < s.size()) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        if (c == 0x1b && i + 1 < s.size() && s[i + 1] == '[') {
            // CSI: copy through the final byte (0x40..0x7E); costs no width.
            std::size_t j = i + 2;
            while (j < s.size() && !(s[j] >= 0x40 && s[j] <= 0x7e)) ++j;
            out.append(s, i, j + 1 - i);
            i = j + 1;
            continue;
        }
        if ((c & 0xC0) != 0x80) {  // a new character, not a UTF-8 continuation
            if (visible == cols) {
                out += "\x1b[0m";
                return out;
            }
            ++visible;
        }
        out += static_cast<char>(c);
        ++i;
    }
    return out;
}

void Frame::begin(TermSize size) {
    size_ = size;
    cur_.clear();
    partial_.clear();
}

void Frame::line(const std::string& s) {
    partial_ += s;
    cur_.push_back(std::move(partial_));
    partial_.clear();
}

int Frame::rows_left() const { return std::max(0, size_.rows - static_cast<int>(cur_.size())); }

std::string Frame::flush() {
    if (!partial_.empty()) line();
    const int rows = std::max(1, size_.rows);
    // One column short of the edge: writing the last column of the last row
    // can scroll some terminals.
    const int width = std::max(1, size_.cols - 1);
    if (static_cast<int>(cur_.size()) > rows) cur_.resize(static_cast<std::size_t>(rows));
    for (auto& l : cur_) l = clip_visible(l, width);

    const bool full = full_ || !(size_ == prev_size_);
    std::string out;
    if (full) out += "\x1b[0m\x1b[H\x1b[2J";
    const std::size_t n = std::max(cur_.size(), full ? 0 : prev_.size());
    for (std::size_t r = 0; r < n; ++r) {
        const std::string empty;
        const std::string& now = r < cur_.size() ? cur_[r] : empty;
        if (!full && r < prev_.size() && prev_[r] == now) continue;
        if (full && now.empty()) continue;
        out += "\x1b[" + std::to_string(r + 1) + ";1H";
        out += now;
        out += "\x1b[0m\x1b[K";
    }
    prev_ = std::move(cur_);
    cur_.clear();
    prev_size_ = size_;
    full_ = false;
    return out;
}

}  // namespace rokkdoxx::tui
