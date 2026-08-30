// The on-disk pattern format shared by rokktui and rokksearch:
//
//   # rokkdoxx pattern
//   seed <string>
//   y <int>
//   center <x> <z>
//   radius <int>
//   orientations <all|exact>
//   size <w> <h>
//   <h lines of w chars: '#' bedrock, 'o' not-bedrock, '.' unknown>
#pragma once

#include <string>

#include "service_types.hpp"

namespace rokkdoxx::svc {

struct PatternFile {
    std::string seed = "0";
    int y = -60;
    std::string center_x = "0";
    std::string center_z = "0";
    std::string radius = "5000";
    bool all_orientations = true;
    Pattern pattern;
};

bool load_pattern_file(const std::string& path, PatternFile& out, std::string& err);
bool save_pattern_file(const std::string& path, const PatternFile& pf, std::string& err);

}  // namespace rokkdoxx::svc
