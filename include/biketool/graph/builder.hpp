#pragma once
// Builds the routing graph from raw inputs:
//   OSM ways  -> split at junctions, cycling access & oneway rules, road class
//   York bike layer -> matched onto edges by proximity + orientation
//   DEM tiles -> per-point elevation, gain/loss, grades (bridges flattened)

#include <cstddef>
#include <string>

#include "biketool/graph/graph.hpp"

namespace bt {

struct BuildOptions {
    std::string osmPath;
    std::string bikesPath;       // optional
    std::string demDir;          // optional
    double densifyM = 10.0;      // geometry spacing for elevation sampling
    double matchRadiusM = 12.0;  // York feature must lie this close to the edge
    double matchAngleDeg = 30.0; // ...and run roughly parallel to it
    double matchFraction = 0.6;  // share of edge samples that must match
    bool verbose = true;
};

struct BuildStats {
    size_t osmWays = 0, usableWays = 0;
    size_t segments = 0, droppedSegments = 0;
    size_t nodes = 0, edges = 0, points = 0, pointsWithElevation = 0;
    size_t yorkFeatures = 0, yorkMatchedSegments = 0, yorkUpgradedSegments = 0;
    size_t bridgeSegments = 0;
    double networkKm = 0.0;
};

Graph buildGraph(const BuildOptions& opt, BuildStats& stats);

}  // namespace bt
