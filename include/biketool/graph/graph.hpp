#pragma once
// Routable cycling graph: directed edges in CSR order, with per-edge geometry
// (densified, carrying elevation), road attributes and bike facility class.

#include <cmath>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

#include "biketool/geo/coords.hpp"

namespace bt {

// Ordered from least to most protected; the numeric value doubles as a rank.
enum class Facility : uint8_t {
    None = 0,
    Sharrow,        // signed / shared lane
    PavedShoulder,
    BikeLane,
    BufferedLane,
    CycleTrack,     // physically separated, on the road corridor
    OffRoadTrail,   // multi-use path / trail away from traffic
    Count
};

enum class RoadClass : uint8_t {
    Trunk = 0, Primary, Secondary, Tertiary, Unclassified, Residential, LivingStreet,
    Service, Cycleway, Path, Footway, Track, Pedestrian, Other, Count
};

enum class Surface : uint8_t { Paved = 0, Compacted, Gravel, Unpaved, Count };

// Where the facility classification came from (bit flags).
enum FacilitySource : uint8_t { kSourceNone = 0, kSourceOsm = 1, kSourceYork = 2 };

const char* facilityName(Facility f);
const char* roadClassName(RoadClass r);
const char* surfaceName(Surface s);
bool isMotorRoad(RoadClass r);

constexpr float kNoElevation = std::numeric_limits<float>::quiet_NaN();

struct Node {
    float lat = 0, lon = 0;
    float elev = kNoElevation;
};

struct GeoPoint {
    float lat = 0, lon = 0;
    float elev = kNoElevation;  // NaN outside DEM coverage
};

struct Edge {
    uint32_t from = 0, to = 0;
    uint32_t geomStart = 0;   // index into Graph::points
    uint32_t geomCount = 0;   // number of points (>= 2)
    uint32_t nameIdx = 0;     // index into Graph::names (0 = unnamed)
    float length = 0;         // metres along geometry
    float gain = 0, loss = 0; // metres climbed / descended in travel direction
    float maxGrade = 0;       // steepest uphill grade in travel direction (fraction)
    float elevCoverage = 0;   // fraction of length with DEM elevation
    RoadClass road = RoadClass::Other;
    Facility facility = Facility::None;
    Surface surface = Surface::Paved;
    uint8_t source = kSourceNone;
    uint8_t reversed = 0;     // geometry is traversed back-to-front
    uint8_t pad[3] = {0, 0, 0};
};

struct Graph {
    std::vector<Node> nodes;
    std::vector<Edge> edges;          // sorted by `from`
    std::vector<uint32_t> firstOut;   // CSR offsets, size nodes.size() + 1
    std::vector<GeoPoint> points;     // shared edge geometry
    std::vector<std::string> names;   // names[0] == ""

    LatLon nodePos(uint32_t n) const { return {nodes[n].lat, nodes[n].lon}; }
    BBox bounds() const;

    // Geometry point i (0-based, in travel direction) of edge e.
    const GeoPoint& edgePoint(const Edge& e, uint32_t i) const {
        return points[e.geomStart + (e.reversed ? e.geomCount - 1 - i : i)];
    }

    // Rebuild firstOut after edges have been sorted by `from`.
    void buildCsr();
};

}  // namespace bt
