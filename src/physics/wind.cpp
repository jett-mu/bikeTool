#include "biketool/physics/wind.hpp"

#include <cmath>

#include "biketool/geo/coords.hpp"

namespace bt {

double Wind::headwind(double travelBearingDeg) const {
    // Riding straight into the wind means travelling toward `fromDeg`.
    return riderSpeed() * std::cos(deg2rad(travelBearingDeg - fromDeg));
}

double Wind::crosswind(double travelBearingDeg) const {
    return std::fabs(riderSpeed() * std::sin(deg2rad(travelBearingDeg - fromDeg)));
}

}  // namespace bt
