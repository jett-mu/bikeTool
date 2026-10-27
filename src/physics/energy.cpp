#include "biketool/physics/energy.hpp"

#include <algorithm>
#include <cmath>

namespace bt {
namespace {

constexpr double kG = 9.80665;

// Bisection for f(v) = 0 on [lo, hi] given f(lo) < 0 < f(hi).
template <class F>
double bisect(F&& f, double lo, double hi) {
    for (int i = 0; i < 40; ++i) {
        const double mid = 0.5 * (lo + hi);
        (f(mid) < 0 ? lo : hi) = mid;
    }
    return 0.5 * (lo + hi);
}

}  // namespace

EnergyModel::EnergyModel(const RiderParams& rider, const Wind& wind) : rider_(rider), wind_(wind) {
    const double pw = rider_.power * rider_.drivetrainEff;
    cruise_ = bisect([&](double v) { return resistance(v, 0.0, 1.0, rider_.crr, 0.0) * v - pw; }, 0.0, 40.0);
    cruise_ = std::min(cruise_, rider_.maxSpeed);
}

double EnergyModel::surfaceCrrFactor(Surface s) {
    switch (s) {
        case Surface::Paved: return 1.0;
        case Surface::Compacted: return 1.6;
        case Surface::Gravel: return 2.2;
        case Surface::Unpaved: return 3.0;
        default: return 1.0;
    }
}

double EnergyModel::resistance(double v, double sinT, double cosT, double crr, double headwind) const {
    const double air = v + headwind;
    return rider_.mass * kG * (sinT + crr * cosT) + 0.5 * rider_.airDensity * rider_.cda * air * std::fabs(air);
}

RideResult EnergyModel::ride(double length, double rise, double bearingDeg, Surface surface) const {
    RideResult r;
    if (length <= 0.0) return r;
    const double dist = std::sqrt(length * length + rise * rise);
    const double sinT = rise / dist;
    const double cosT = length / dist;
    const double crr = rider_.crr * surfaceCrrFactor(surface);
    const double h = wind_.headwind(bearingDeg);
    const double eta = rider_.drivetrainEff;
    auto F = [&](double v) { return resistance(v, sinT, cosT, crr, h); };

    // Terminal (coasting) speed where gravity balances rolling + drag.
    const double pull = -rider_.mass * kG * (sinT + crr * cosT);
    double vCoast = 0.0;
    if (pull > 0.0) vCoast = std::max(0.0, std::sqrt(2.0 * pull / (rider_.airDensity * rider_.cda)) - h);

    double v = 0.0;
    double force = 0.0;
    if (vCoast >= cruise_) {
        v = std::min(vCoast, rider_.maxSpeed);  // freewheel (and brake above maxSpeed)
        force = 0.0;
    } else {
        const double pw = rider_.power * eta;
        const double vEasy = bisect([&](double s) { return F(s) * s - pw; }, 0.0, 40.0);
        if (vEasy >= cruise_) {
            v = std::min(vEasy, rider_.maxSpeed);  // easy conditions: keep normal effort, go faster
        } else if (F(cruise_) * cruise_ <= rider_.maxPower * eta) {
            v = cruise_;                            // push harder to hold cruising speed
        } else {
            const double pmax = rider_.maxPower * eta;
            v = bisect([&](double s) { return F(s) * s - pmax; }, 0.0, cruise_);
            v = std::max(v, rider_.minSpeed);       // walk the steepest bits
        }
        force = std::max(0.0, F(v));
    }
    r.speed = v;
    r.time = dist / v;
    r.work = force * dist / eta;
    return r;
}

RideResult EnergyModel::rideEdge(const Graph& g, const Edge& e) const {
    RideResult total;
    for (uint32_t i = 0; i + 1 < e.geomCount; ++i) {
        const GeoPoint& a = g.edgePoint(e, i);
        const GeoPoint& b = g.edgePoint(e, i + 1);
        const LatLon pa{a.lat, a.lon}, pb{b.lat, b.lon};
        const double len = haversine(pa, pb);
        if (len <= 0.0) continue;
        const double rise = (std::isnan(a.elev) || std::isnan(b.elev)) ? 0.0 : static_cast<double>(b.elev - a.elev);
        const RideResult piece = ride(len, rise, bearingDeg(pa, pb), e.surface);
        total.time += piece.time;
        total.work += piece.work;
    }
    total.speed = total.time > 0 ? e.length / total.time : 0.0;
    return total;
}

}  // namespace bt
