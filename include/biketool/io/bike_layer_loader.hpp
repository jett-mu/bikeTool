#pragma once
// Loads the York Region "socBikePath" open-data layer (GeoJSON, WGS84) and
// maps its free-text facility classes onto our Facility ranking.

#include <string>
#include <vector>

#include "biketool/geo/coords.hpp"
#include "biketool/graph/graph.hpp"

namespace bt {

// Which OSM edges a York feature may be matched onto.
enum class MatchScope { OnRoad, OffRoad, Any };

struct BikeFeature {
    Facility facility = Facility::None;
    MatchScope scope = MatchScope::Any;
    std::string rawClass;
    std::string name;
    std::vector<std::vector<LatLon>> lines;
};

// Returns Facility::None for classes that are not cycling facilities
// (unsigned shared roadways, hiking trails closed to bikes, connectors).
Facility classifyYorkFacility(const std::string& rawClass, bool cyclingAllowed, MatchScope& scope);

// Skips closed / proposed features and unusable classes.
std::vector<BikeFeature> loadYorkBikeLayer(const std::string& path);

}  // namespace bt
