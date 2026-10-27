#include "biketool/geo/coords.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace bt {

void BBox::extend(LatLon p) {
    minLat = std::min(minLat, p.lat);
    maxLat = std::max(maxLat, p.lat);
    minLon = std::min(minLon, p.lon);
    maxLon = std::max(maxLon, p.lon);
}

bool BBox::contains(LatLon p) const {
    return p.lat >= minLat && p.lat <= maxLat && p.lon >= minLon && p.lon <= maxLon;
}

double haversine(LatLon a, LatLon b) {
    const double dLat = deg2rad(b.lat - a.lat);
    const double dLon = deg2rad(b.lon - a.lon);
    const double s = std::sin(dLat / 2) * std::sin(dLat / 2) +
                     std::cos(deg2rad(a.lat)) * std::cos(deg2rad(b.lat)) *
                         std::sin(dLon / 2) * std::sin(dLon / 2);
    return 2.0 * kEarthRadiusM * std::asin(std::min(1.0, std::sqrt(s)));
}

double bearingDeg(LatLon a, LatLon b) {
    const double p1 = deg2rad(a.lat);
    const double p2 = deg2rad(b.lat);
    const double dl = deg2rad(b.lon - a.lon);
    const double y = std::sin(dl) * std::cos(p2);
    const double x = std::cos(p1) * std::sin(p2) - std::sin(p1) * std::cos(p2) * std::cos(dl);
    return normDeg(rad2deg(std::atan2(y, x)));
}

double normDeg(double d) {
    d = std::fmod(d, 360.0);
    return d < 0 ? d + 360.0 : d;
}

double bearingDiff(double a, double b) {
    const double d = std::fabs(normDeg(a) - normDeg(b));
    return d > 180.0 ? 360.0 - d : d;
}

double orientationDiff(double a, double b) {
    const double d = bearingDiff(a, b);
    return d > 90.0 ? 180.0 - d : d;
}

LatLon lerp(LatLon a, LatLon b, double t) {
    return {a.lat + (b.lat - a.lat) * t, a.lon + (b.lon - a.lon) * t};
}

bool parseLatLon(const std::string& text, LatLon& out) {
    const auto comma = text.find(',');
    if (comma == std::string::npos) return false;
    char* end = nullptr;
    const std::string latStr = text.substr(0, comma);
    const std::string lonStr = text.substr(comma + 1);
    const double lat = std::strtod(latStr.c_str(), &end);
    if (end == latStr.c_str()) return false;
    const double lon = std::strtod(lonStr.c_str(), &end);
    if (end == lonStr.c_str()) return false;
    if (!std::isfinite(lat) || !std::isfinite(lon) || std::fabs(lat) > 90 || std::fabs(lon) > 180)
        return false;
    out = {lat, lon};
    return true;
}

LocalProjection::LocalProjection(LatLon origin)
    : origin_(origin),
      kx_(deg2rad(1.0) * kEarthRadiusM * std::cos(deg2rad(origin.lat))),
      ky_(deg2rad(1.0) * kEarthRadiusM) {}

XY LocalProjection::forward(LatLon p) const {
    return {(p.lon - origin_.lon) * kx_, (p.lat - origin_.lat) * ky_};
}

LatLon LocalProjection::inverse(XY p) const {
    return {origin_.lat + p.y / ky_, origin_.lon + p.x / kx_};
}

double pointSegmentDistance(XY p, XY a, XY b) {
    const double dx = b.x - a.x;
    const double dy = b.y - a.y;
    const double len2 = dx * dx + dy * dy;
    double t = len2 > 0 ? ((p.x - a.x) * dx + (p.y - a.y) * dy) / len2 : 0.0;
    t = std::clamp(t, 0.0, 1.0);
    const double ex = a.x + t * dx - p.x;
    const double ey = a.y + t * dy - p.y;
    return std::sqrt(ex * ex + ey * ey);
}

std::vector<LatLon> simplify(const std::vector<LatLon>& pts, double toleranceM,
                             const LocalProjection& proj) {
    if (pts.size() < 3) return pts;
    std::vector<XY> xy(pts.size());
    for (size_t i = 0; i < pts.size(); ++i) xy[i] = proj.forward(pts[i]);

    std::vector<char> keep(pts.size(), 0);
    keep.front() = keep.back() = 1;
    std::vector<std::pair<size_t, size_t>> stack{{0, pts.size() - 1}};
    while (!stack.empty()) {
        auto [lo, hi] = stack.back();
        stack.pop_back();
        double best = -1;
        size_t bestIdx = lo;
        for (size_t i = lo + 1; i < hi; ++i) {
            const double d = pointSegmentDistance(xy[i], xy[lo], xy[hi]);
            if (d > best) {
                best = d;
                bestIdx = i;
            }
        }
        if (best > toleranceM) {
            keep[bestIdx] = 1;
            stack.push_back({lo, bestIdx});
            stack.push_back({bestIdx, hi});
        }
    }
    std::vector<LatLon> out;
    for (size_t i = 0; i < pts.size(); ++i)
        if (keep[i]) out.push_back(pts[i]);
    return out;
}

}  // namespace bt
