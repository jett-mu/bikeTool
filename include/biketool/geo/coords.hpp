#pragma once
// Geographic primitives: lat/lon points, great-circle distance, bearings and a
// local metric projection used for spatial indexing and geometry operations.

#include <string>
#include <vector>

namespace bt {

constexpr double kEarthRadiusM = 6371008.8;
constexpr double kPi = 3.14159265358979323846;

inline double deg2rad(double d) { return d * kPi / 180.0; }
inline double rad2deg(double r) { return r * 180.0 / kPi; }

struct LatLon {
    double lat = 0.0;
    double lon = 0.0;
};

struct XY {
    double x = 0.0;  // metres east
    double y = 0.0;  // metres north
};

struct BBox {
    double minLat = 90, minLon = 180, maxLat = -90, maxLon = -180;
    void extend(LatLon p);
    bool contains(LatLon p) const;
    bool valid() const { return minLat <= maxLat && minLon <= maxLon; }
};

// Great-circle distance in metres.
double haversine(LatLon a, LatLon b);

// Initial bearing from a to b in degrees, 0 = north, clockwise, [0, 360).
double bearingDeg(LatLon a, LatLon b);

// Normalise an angle to [0, 360).
double normDeg(double d);

// Smallest absolute difference between two bearings, [0, 180].
double bearingDiff(double a, double b);

// Difference between two undirected line orientations, [0, 90].
double orientationDiff(double a, double b);

LatLon lerp(LatLon a, LatLon b, double t);

// Parse "lat,lon"; returns false on malformed input.
bool parseLatLon(const std::string& text, LatLon& out);

// Equirectangular projection around a reference point. Accurate to well under
// 0.1% over a region the size of York Region, which is all we need for
// proximity tests and simplification.
class LocalProjection {
public:
    LocalProjection() = default;
    explicit LocalProjection(LatLon origin);
    XY forward(LatLon p) const;
    LatLon inverse(XY p) const;

private:
    LatLon origin_{};
    double kx_ = 1.0;
    double ky_ = 1.0;
};

// Distance from point p to segment ab in projected metres.
double pointSegmentDistance(XY p, XY a, XY b);

// Douglas-Peucker simplification with tolerance in metres.
std::vector<LatLon> simplify(const std::vector<LatLon>& pts, double toleranceM,
                             const LocalProjection& proj);

}  // namespace bt
