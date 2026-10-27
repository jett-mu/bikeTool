#pragma once
// Cycling power model. The rider aims for a cruising speed (what `power`
// sustains on flat ground in still air) and works harder on climbs and into
// headwinds, up to `maxPower`; beyond that they slow down. Downhill they coast
// at terminal velocity (capped at `maxSpeed`). Energy is the mechanical work
// actually delivered at the pedals, so climbs and headwinds cost energy even
// where they cost little time.

#include "biketool/graph/graph.hpp"
#include "biketool/physics/wind.hpp"

namespace bt {

struct RiderParams {
    double mass = 85.0;           // rider + bike, kg
    double power = 150.0;         // sustainable cruising power, W
    double maxPower = 250.0;      // short-term climbing power, W
    double cda = 0.45;            // drag area, m^2 (upright commuter)
    double crr = 0.006;           // rolling resistance on smooth asphalt
    double drivetrainEff = 0.97;
    double metabolicEff = 0.24;   // gross efficiency, mechanical -> food energy
    double airDensity = 1.20;     // kg/m^3
    double maxSpeed = 12.5;       // m/s, braking above this (45 km/h)
    double minSpeed = 1.4;        // m/s, below this the rider walks the bike
};

struct RideResult {
    double time = 0.0;     // s
    double work = 0.0;     // J at the pedals (before drivetrain loss)
    double speed = 0.0;    // m/s
};

class EnergyModel {
public:
    EnergyModel(const RiderParams& rider, const Wind& wind);

    const RiderParams& rider() const { return rider_; }
    const Wind& wind() const { return wind_; }
    double cruiseSpeed() const { return cruise_; }

    // One straight piece of road: horizontal length (m), rise (m, may be
    // negative), travel bearing (deg) and surface.
    RideResult ride(double length, double rise, double bearingDeg, Surface surface) const;

    // Whole edge, integrated over its geometry.
    RideResult rideEdge(const Graph& g, const Edge& e) const;

    static double surfaceCrrFactor(Surface s);
    double foodKcal(double workJ) const { return workJ / rider_.metabolicEff / 4184.0; }

private:
    double resistance(double v, double sinT, double cosT, double crr, double headwind) const;

    RiderParams rider_;
    Wind wind_;
    double cruise_ = 0.0;
};

}  // namespace bt
