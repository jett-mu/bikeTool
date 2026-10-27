#include "biketool/graph/graph.hpp"

#include <algorithm>
#include <iterator>

namespace bt {

const char* facilityName(Facility f) {
    switch (f) {
        case Facility::None: return "none";
        case Facility::Sharrow: return "sharrow";
        case Facility::PavedShoulder: return "paved_shoulder";
        case Facility::BikeLane: return "bike_lane";
        case Facility::BufferedLane: return "buffered_lane";
        case Facility::CycleTrack: return "cycle_track";
        case Facility::OffRoadTrail: return "trail";
        default: return "unknown";
    }
}

const char* roadClassName(RoadClass r) {
    static const char* names[] = {"trunk", "primary", "secondary", "tertiary", "unclassified",
                                  "residential", "living_street", "service", "cycleway", "path",
                                  "footway", "track", "pedestrian", "other"};
    const auto i = static_cast<size_t>(r);
    return i < std::size(names) ? names[i] : "other";
}

const char* surfaceName(Surface s) {
    static const char* names[] = {"paved", "compacted", "gravel", "unpaved"};
    const auto i = static_cast<size_t>(s);
    return i < std::size(names) ? names[i] : "paved";
}

bool isMotorRoad(RoadClass r) {
    switch (r) {
        case RoadClass::Trunk: case RoadClass::Primary: case RoadClass::Secondary:
        case RoadClass::Tertiary: case RoadClass::Unclassified: case RoadClass::Residential:
        case RoadClass::LivingStreet: case RoadClass::Service:
            return true;
        default:
            return false;
    }
}

BBox Graph::bounds() const {
    BBox b;
    for (const auto& n : nodes) b.extend({n.lat, n.lon});
    return b;
}

void Graph::buildCsr() {
    firstOut.assign(nodes.size() + 1, 0);
    for (const auto& e : edges) ++firstOut[e.from + 1];
    for (size_t i = 1; i < firstOut.size(); ++i) firstOut[i] += firstOut[i - 1];
}

}  // namespace bt
