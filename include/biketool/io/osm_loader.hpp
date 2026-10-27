#pragma once
// Loads an Overpass API JSON dump (`out body; >; out skel qt;`) into plain
// node and way tables.

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "biketool/geo/coords.hpp"

namespace bt {

using Tags = std::unordered_map<std::string, std::string>;

struct OsmWay {
    int64_t id = 0;
    std::vector<int64_t> nodes;
    Tags tags;

    const std::string& tag(const std::string& key) const;  // "" when absent
};

struct OsmData {
    std::unordered_map<int64_t, LatLon> nodes;
    std::vector<OsmWay> ways;
};

OsmData loadOsmJson(const std::string& path);  // throws std::runtime_error

}  // namespace bt
