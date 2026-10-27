#include "biketool/routing/cost_models.hpp"

#include <algorithm>
#include <cmath>
#include <sstream>
#include <stdexcept>

namespace bt {
namespace {

struct ProfileInfo {
    const char* id;
    const char* label;
};

constexpr ProfileInfo kProfiles[] = {
    {"shortest", "Shortest"},       {"fastest", "Fastest"},
    {"bike_friendly", "Bike-friendly"}, {"flattest", "Flattest"},
    {"least_energy", "Least energy"}, {"balanced", "Balanced"},
};

constexpr double kEnergyPerMetreFloor = 1.0;  // J/m tie-breaker so free descents aren't "free" detours

double roadStress(RoadClass r) {
    switch (r) {
        case RoadClass::Trunk: return 3.0;
        case RoadClass::Primary: return 2.2;
        case RoadClass::Secondary: return 1.8;
        case RoadClass::Tertiary: return 1.4;
        case RoadClass::Unclassified: return 1.15;
        case RoadClass::Residential: return 1.0;
        case RoadClass::LivingStreet: return 0.95;
        case RoadClass::Service: return 1.15;
        case RoadClass::Cycleway: return 0.7;
        case RoadClass::Path: return 0.8;
        case RoadClass::Footway: return 0.9;
        case RoadClass::Track: return 1.2;
        case RoadClass::Pedestrian: return 0.95;
        default: return 1.2;
    }
}

double surfacePenalty(Surface s) {
    switch (s) {
        case Surface::Compacted: return 0.1;
        case Surface::Gravel: return 0.3;
        case Surface::Unpaved: return 0.6;
        default: return 0.0;
    }
}

}  // namespace

const char* profileId(Profile p) { return kProfiles[static_cast<int>(p)].id; }
const char* profileLabel(Profile p) { return kProfiles[static_cast<int>(p)].label; }

bool parseProfile(const std::string& id, Profile& out) {
    for (int i = 0; i < static_cast<int>(Profile::Count); ++i)
        if (id == kProfiles[i].id) {
            out = static_cast<Profile>(i);
            return true;
        }
    return false;
}

std::vector<Profile> parseProfiles(const std::string& list) {
    std::vector<Profile> out;
    if (list.empty() || list == "all") {
        for (int i = 0; i < static_cast<int>(Profile::Count); ++i) out.push_back(static_cast<Profile>(i));
        return out;
    }
    std::stringstream ss(list);
    std::string tok;
    while (std::getline(ss, tok, ',')) {
        Profile p;
        if (!parseProfile(tok, p)) throw std::runtime_error("unknown profile '" + tok + "'");
        if (std::find(out.begin(), out.end(), p) == out.end()) out.push_back(p);
    }
    return out;
}

double comfortStress(const Edge& e) {
    double s;
    switch (e.facility) {
        case Facility::OffRoadTrail: s = 0.65; break;
        case Facility::CycleTrack: s = kMinStress; break;
        default: {
            double mult = 1.0;
            if (e.facility == Facility::BufferedLane) mult = 0.45;
            else if (e.facility == Facility::BikeLane) mult = 0.55;
            else if (e.facility == Facility::PavedShoulder) mult = 0.7;
            else if (e.facility == Facility::Sharrow) mult = 0.85;
            s = roadStress(e.road) * mult;
            if (e.facility != Facility::None) s = std::max(s, 0.7);
        }
    }
    return std::max(kMinStress, s + surfacePenalty(e.surface));
}

EdgePhysicsCache::EdgePhysicsCache(const Graph& g, const EnergyModel& model)
    : g_(g), model_(model), results_(g.edges.size()), done_(g.edges.size(), 0) {}

const RideResult& EdgePhysicsCache::get(uint32_t edgeId) {
    if (!done_[edgeId]) {
        results_[edgeId] = model_.rideEdge(g_, g_.edges[edgeId]);
        done_[edgeId] = 1;
    }
    return results_[edgeId];
}

CostModel::CostModel(Profile p, const Graph& g, EdgePhysicsCache& physics)
    : profile_(p), g_(g), physics_(physics) {
    const double vmax = physics.model().rider().maxSpeed;
    switch (p) {
        case Profile::Shortest: hFactor_ = 1.0; break;
        case Profile::Fastest: hFactor_ = 1.0 / vmax; break;
        case Profile::BikeFriendly: hFactor_ = kMinStress; break;
        case Profile::Flattest: hFactor_ = 1.0; break;
        case Profile::LeastEnergy: hFactor_ = kEnergyPerMetreFloor; break;
        case Profile::Balanced: hFactor_ = (0.6 + 0.4 * kMinStress) / vmax; break;
        default: hFactor_ = 0.0;
    }
}

double CostModel::edgeCost(uint32_t edgeId) {
    const Edge& e = g_.edges[edgeId];
    switch (profile_) {
        case Profile::Shortest:
            return e.length;
        case Profile::Fastest:
            return physics_.get(edgeId).time;
        case Profile::BikeFriendly:
            return e.length * comfortStress(e);
        case Profile::Flattest:
            return e.length * (1.0 + 10.0 * std::max(0.0, static_cast<double>(e.maxGrade) - 0.04)) + 30.0 * e.gain;
        case Profile::LeastEnergy:
            return physics_.get(edgeId).work + kEnergyPerMetreFloor * e.length;
        case Profile::Balanced: {
            const RideResult& r = physics_.get(edgeId);
            return r.time * (0.6 + 0.4 * comfortStress(e)) + 0.3 * r.work / physics_.model().rider().power;
        }
        default:
            return e.length;
    }
}

}  // namespace bt
