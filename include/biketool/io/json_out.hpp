#pragma once
// JSON for route responses and GeoJSON map layers consumed by the web UI.

#include <string>

#include <nlohmann/json.hpp>

#include "biketool/elevation/dem.hpp"
#include "biketool/graph/graph.hpp"
#include "biketool/routing/router.hpp"

namespace bt {

nlohmann::json snapToJson(const Snap& s);

nlohmann::json routeToJson(const RouteDetail& d, Profile p, const EnergyModel& model,
                           size_t maxProfilePoints = 800);

struct LayerStats {
    size_t bikeFeatures = 0, steepFeatures = 0, coverageFeatures = 0;
};

// Writes bike_network.geojson, steep_segments.geojson and (if dem is given)
// dem_coverage.geojson into outDir.
LayerStats writeMapLayers(const Graph& g, const DemMosaic* dem, const std::string& outDir);

}  // namespace bt
