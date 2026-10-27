#pragma once
// Routing profiles. Each turns an edge into a non-negative cost and provides
// an admissible A* heuristic from straight-line distance.

#include <string>
#include <vector>

#include "biketool/graph/graph.hpp"
#include "biketool/physics/energy.hpp"

namespace bt {

enum class Profile { Shortest = 0, Fastest, BikeFriendly, Flattest, LeastEnergy, Balanced, Count };

const char* profileId(Profile p);     // machine id, e.g. "least_energy"
const char* profileLabel(Profile p);  // display label
bool parseProfile(const std::string& id, Profile& out);
std::vector<Profile> parseProfiles(const std::string& list);  // "all" or comma separated

// Traffic-stress multiplier (lower = more comfortable). Never below kMinStress.
constexpr double kMinStress = 0.6;
double comfortStress(const Edge& e);

// Lazily evaluated physics per edge, shared by all profiles in one query.
class EdgePhysicsCache {
public:
    EdgePhysicsCache(const Graph& g, const EnergyModel& model);
    const RideResult& get(uint32_t edgeId);
    const EnergyModel& model() const { return model_; }

private:
    const Graph& g_;
    const EnergyModel& model_;
    std::vector<RideResult> results_;
    std::vector<char> done_;
};

class CostModel {
public:
    CostModel(Profile p, const Graph& g, EdgePhysicsCache& physics);
    Profile profile() const { return profile_; }
    double edgeCost(uint32_t edgeId);
    double heuristic(double straightLineM) const { return straightLineM * hFactor_; }

private:
    Profile profile_;
    const Graph& g_;
    EdgePhysicsCache& physics_;
    double hFactor_ = 0.0;
};

}  // namespace bt
