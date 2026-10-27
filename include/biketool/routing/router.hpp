#pragma once
// A* routing over the graph with a pluggable CostModel. Start and end snap to
// the nearest geometry point of any edge (not just junctions), so routes can
// begin and end mid-block; partial edges are represented as Legs.

#include <optional>
#include <string>
#include <vector>

#include "biketool/graph/graph.hpp"
#include "biketool/routing/cost_models.hpp"

namespace bt {

// A traversal of edge `edge` from geometry index `from` to `to` (inclusive,
// travel-direction indices, from < to).
struct Leg {
    uint32_t edge = 0;
    uint32_t from = 0, to = 0;
    bool operator==(const Leg&) const = default;
};

struct Snap {
    uint32_t point = 0;       // index into Graph::points
    LatLon requested{};
    LatLon snapped{};
    double distance = 0.0;    // metres from requested to snapped
};

struct PathResult {
    bool found = false;
    std::vector<Leg> legs;
    double cost = 0.0;
    size_t settled = 0;       // nodes settled, for diagnostics
};

// Per-point output along a route.
struct ProfilePoint {
    double dist = 0;          // metres from start
    float elev = kNoElevation;
    double grade = 0;         // smoothed, fraction
};

struct RouteStep {            // consecutive legs sharing a name + facility
    std::string name;
    Facility facility = Facility::None;
    RoadClass road = RoadClass::Other;
    double length = 0;
    size_t startIndex = 0, endIndex = 0;  // into RouteDetail::coords
};

struct RouteDetail {
    std::vector<LatLon> coords;
    std::vector<ProfilePoint> profile;   // one per coord
    std::vector<RouteStep> steps;
    double distance = 0, time = 0, work = 0;
    double gain = 0, loss = 0, maxGrade = 0;
    double avgHeadwind = 0;              // length-weighted, m/s
    double elevCoverage = 0;             // fraction of distance with DEM data
    double facilityLength[static_cast<size_t>(Facility::Count)] = {};
};

class Router {
public:
    explicit Router(const Graph& g);

    // Throws std::runtime_error if the graph is empty.
    Snap snap(LatLon p) const;

    PathResult route(const Snap& start, const Snap& end, CostModel& cost);

    RouteDetail describe(const std::vector<Leg>& legs, const EnergyModel& model) const;

private:
    struct EdgeAt {
        uint32_t edge;
        uint32_t index;  // travel-direction geometry index of the snapped point
    };
    std::vector<EdgeAt> edgesAtPoint(uint32_t point) const;
    double legLength(const Leg& leg) const;

    const Graph& g_;
    std::vector<uint32_t> byGeom_;  // edge ids sorted by geomStart
    std::vector<double> dist_;
    std::vector<uint32_t> parentEdge_;
    std::vector<uint32_t> visitStamp_;
    uint32_t stamp_ = 0;
};

}  // namespace bt
