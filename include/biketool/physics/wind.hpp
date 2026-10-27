#pragma once
// Wind as reported by weather services: speed at 10 m and the direction it
// blows FROM (meteorological convention), reduced to rider height.

namespace bt {

struct Wind {
    double speed10m = 0.0;     // m/s at 10 m
    double fromDeg = 0.0;      // direction wind comes from, 0 = north
    double heightFactor = 0.6; // log-profile reduction from 10 m to ~1.5 m in suburbs

    double riderSpeed() const { return speed10m * heightFactor; }

    // Headwind component (positive = against the rider) for a travel bearing.
    double headwind(double travelBearingDeg) const;

    // Crosswind magnitude for a travel bearing.
    double crosswind(double travelBearingDeg) const;
};

}  // namespace bt
