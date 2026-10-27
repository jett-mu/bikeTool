#include "biketool/io/json_out.hpp"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <map>
#include <stdexcept>

namespace bt {
namespace {

using nlohmann::json;

double round6(double v) { return std::round(v * 1e6) / 1e6; }
double round2(double v) { return std::round(v * 100.0) / 100.0; }

json coord(LatLon p) { return json::array({round6(p.lon), round6(p.lat)}); }

json lineString(const std::vector<LatLon>& pts) {
    json coords = json::array();
    for (const auto& p : pts) coords.push_back(coord(p));
    return {{"type", "LineString"}, {"coordinates", std::move(coords)}};
}

const char* sourceName(uint8_t s) {
    if ((s & kSourceOsm) && (s & kSourceYork)) return "both";
    if (s & kSourceYork) return "york";
    if (s & kSourceOsm) return "osm";
    return "none";
}

void writeJson(const std::string& path, const json& doc) {
    std::ofstream out(path);
    if (!out) throw std::runtime_error("cannot write " + path);
    out << doc.dump();
}

}  // namespace

json snapToJson(const Snap& s) {
    return {{"requested", coord(s.requested)}, {"snapped", coord(s.snapped)}, {"distance_m", round2(s.distance)}};
}

json routeToJson(const RouteDetail& d, Profile p, const EnergyModel& model, size_t maxProfilePoints) {
    json share = json::object();
    double onFacility = 0;
    for (size_t f = 0; f < static_cast<size_t>(Facility::Count); ++f) {
        if (d.facilityLength[f] <= 0) continue;
        share[facilityName(static_cast<Facility>(f))] = round2(d.facilityLength[f] / std::max(1.0, d.distance));
        if (f != static_cast<size_t>(Facility::None)) onFacility += d.facilityLength[f];
    }

    json profile = json::array();
    const size_t n = d.profile.size();
    const size_t stride = std::max<size_t>(1, (n + maxProfilePoints - 1) / std::max<size_t>(1, maxProfilePoints));
    for (size_t i = 0; i < n; i += stride) {
        const auto& pt = d.profile[i];
        profile.push_back({std::round(pt.dist), std::isnan(pt.elev) ? json(nullptr) : json(round2(pt.elev)),
                           std::round(pt.grade * 1000.0) / 10.0, i});
    }
    if (n > 0 && (n - 1) % stride != 0) {
        const auto& pt = d.profile.back();
        profile.push_back({std::round(pt.dist), std::isnan(pt.elev) ? json(nullptr) : json(round2(pt.elev)),
                           std::round(pt.grade * 1000.0) / 10.0, n - 1});
    }

    json steps = json::array();
    for (const auto& s : d.steps)
        steps.push_back({{"name", s.name}, {"facility", facilityName(s.facility)}, {"road", roadClassName(s.road)},
                         {"length_m", std::round(s.length)}, {"start", s.startIndex}, {"end", s.endIndex}});

    // Per-point grade and facility arrays let the UI colour the line.
    json grades = json::array();
    for (const auto& pt : d.profile) grades.push_back(std::round(pt.grade * 1000.0) / 10.0);

    return {
        {"profile", profileId(p)},
        {"label", profileLabel(p)},
        {"distance_m", std::round(d.distance)},
        {"time_s", std::round(d.time)},
        {"avg_speed_kmh", d.time > 0 ? round2(d.distance / d.time * 3.6) : 0.0},
        {"gain_m", round2(d.gain)},
        {"loss_m", round2(d.loss)},
        {"max_grade_pct", round2(d.maxGrade * 100.0)},
        {"energy_kj", round2(d.work / 1000.0)},
        {"food_kcal", std::round(model.foodKcal(d.work))},
        {"avg_power_w", d.time > 0 ? std::round(d.work * model.rider().drivetrainEff / d.time) : 0.0},
        {"avg_headwind_ms", round2(d.avgHeadwind)},
        {"elevation_coverage", round2(d.elevCoverage)},
        {"bike_share", round2(onFacility / std::max(1.0, d.distance))},
        {"facility_share", share},
        {"geometry", lineString(d.coords)},
        {"grades_pct", grades},
        {"elevation_profile", profile},
        {"steps", steps},
    };
}

LayerStats writeMapLayers(const Graph& g, const DemMosaic* dem, const std::string& outDir) {
    std::filesystem::create_directories(outDir);
    LayerStats stats;
    const BBox box = g.bounds();
    const LocalProjection proj({(box.minLat + box.maxLat) / 2, (box.minLon + box.maxLon) / 2});

    // Merge both directions of each physical segment (they share geomStart).
    struct SegInfo {
        Facility facility = Facility::None;
        uint8_t source = 0;
        uint32_t edge = 0;
    };
    std::map<uint32_t, SegInfo> segs;
    for (uint32_t i = 0; i < g.edges.size(); ++i) {
        const Edge& e = g.edges[i];
        auto& s = segs[e.geomStart];
        if (static_cast<uint8_t>(e.facility) > static_cast<uint8_t>(s.facility)) s.facility = e.facility;
        s.source |= e.source;
        s.edge = i;
    }

    json bike = json::array(), steep = json::array();
    for (const auto& [geomStart, info] : segs) {
        const Edge& e = g.edges[info.edge];
        std::vector<LatLon> pts;
        std::vector<float> z;
        for (uint32_t k = 0; k < e.geomCount; ++k) {
            const GeoPoint& p = g.points[geomStart + k];
            pts.push_back({p.lat, p.lon});
            z.push_back(p.elev);
        }
        if (info.facility != Facility::None) {
            bike.push_back({{"type", "Feature"},
                            {"properties",
                             {{"f", facilityName(info.facility)},
                              {"s", sourceName(info.source)},
                              {"n", g.names[e.nameIdx]},
                              {"r", roadClassName(e.road)}}},
                            {"geometry", lineString(simplify(pts, 2.0, proj))}});
        }

        // Steep runs: contiguous stretches whose 25 m grade exceeds 4%.
        std::vector<double> cum(pts.size(), 0.0);
        for (size_t k = 1; k < pts.size(); ++k) cum[k] = cum[k - 1] + haversine(pts[k - 1], pts[k]);
        size_t runStart = SIZE_MAX;
        double runMax = 0;
        auto flush = [&](size_t endIdx) {
            if (runStart != SIZE_MAX && cum[endIdx] - cum[runStart] >= 30.0) {
                std::vector<LatLon> run(pts.begin() + runStart, pts.begin() + endIdx + 1);
                steep.push_back({{"type", "Feature"},
                                 {"properties", {{"g", std::round(runMax * 1000.0) / 10.0}, {"n", g.names[e.nameIdx]}}},
                                 {"geometry", lineString(simplify(run, 2.0, proj))}});
            }
            runStart = SIZE_MAX;
            runMax = 0;
        };
        for (size_t k = 0, j = 0; k < pts.size(); ++k) {
            while (j < pts.size() && cum[j] - cum[k] < 25.0) ++j;
            double grade = 0;
            if (j < pts.size() && !std::isnan(z[k]) && !std::isnan(z[j])) grade = (z[j] - z[k]) / (cum[j] - cum[k]);
            if (std::fabs(grade) >= 0.04) {
                if (runStart == SIZE_MAX) runStart = k;
                runMax = std::max(runMax, std::fabs(grade));
            } else if (runStart != SIZE_MAX) {
                flush(std::min(pts.size() - 1, j > 0 ? j - 1 : k));
            }
        }
        flush(pts.size() - 1);
    }
    stats.bikeFeatures = bike.size();
    stats.steepFeatures = steep.size();
    writeJson(outDir + "/bike_network.geojson", {{"type", "FeatureCollection"}, {"features", bike}});
    writeJson(outDir + "/steep_segments.geojson", {{"type", "FeatureCollection"}, {"features", steep}});

    if (dem) {
        json tiles = json::array();
        for (const auto& t : dem->tiles()) {
            json ring = json::array();
            for (const auto& p : t.outline) ring.push_back(coord(p));
            tiles.push_back({{"type", "Feature"},
                             {"properties", {{"tile", std::filesystem::path(t.path).filename().string()}}},
                             {"geometry", {{"type", "Polygon"}, {"coordinates", json::array({ring})}}}});
        }
        stats.coverageFeatures = tiles.size();
        writeJson(outDir + "/dem_coverage.geojson", {{"type", "FeatureCollection"}, {"features", tiles}});
    }
    return stats;
}

}  // namespace bt
