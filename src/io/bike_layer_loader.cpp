#include "biketool/io/bike_layer_loader.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <stdexcept>

#include <nlohmann/json.hpp>

namespace bt {
namespace {

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::tolower(c); });
    return s;
}

bool contains(const std::string& s, const char* needle) { return s.find(needle) != std::string::npos; }

std::string stringProp(const nlohmann::json& props, const char* key) {
    auto it = props.find(key);
    return (it != props.end() && it->is_string()) ? it->get<std::string>() : std::string();
}

std::vector<LatLon> parseLine(const nlohmann::json& coords) {
    std::vector<LatLon> line;
    for (const auto& c : coords)
        if (c.is_array() && c.size() >= 2) line.push_back({c[1].get<double>(), c[0].get<double>()});
    return line;
}

}  // namespace

Facility classifyYorkFacility(const std::string& rawClass, bool cyclingAllowed, MatchScope& scope) {
    const std::string c = lower(rawClass);
    scope = MatchScope::Any;
    if (contains(c, "protected") || contains(c, "cycle track")) return Facility::CycleTrack;
    if (contains(c, "buffered")) { scope = MatchScope::OnRoad; return Facility::BufferedLane; }
    if (contains(c, "bike lane")) { scope = MatchScope::OnRoad; return Facility::BikeLane; }
    if (contains(c, "urban shoulder") || contains(c, "paved shoulder")) {
        scope = MatchScope::OnRoad;
        return Facility::PavedShoulder;
    }
    if (contains(c, "unsigned")) return Facility::None;  // ordinary street, nothing marked
    if (contains(c, "shared roadway") || contains(c, "shaerd roadway")) {
        scope = MatchScope::OnRoad;
        return Facility::Sharrow;
    }
    if (contains(c, "hiking")) {
        scope = MatchScope::OffRoad;
        return cyclingAllowed ? Facility::OffRoadTrail : Facility::None;
    }
    if (contains(c, "multi") || contains(c, "pathway") || contains(c, "trail")) {
        scope = MatchScope::OffRoad;
        return Facility::OffRoadTrail;
    }
    return Facility::None;
}

std::vector<BikeFeature> loadYorkBikeLayer(const std::string& path) {
    std::ifstream in(path);
    if (!in) throw std::runtime_error("cannot open " + path);
    nlohmann::json doc;
    try {
        in >> doc;
    } catch (const nlohmann::json::exception& e) {
        throw std::runtime_error("invalid GeoJSON in " + path + ": " + e.what());
    }

    std::vector<BikeFeature> out;
    for (const auto& f : doc.value("features", nlohmann::json::array())) {
        const auto& props = f.contains("properties") && f["properties"].is_object() ? f["properties"]
                                                                                    : nlohmann::json::object();
        const std::string status = lower(stringProp(props, "BIKE_FACILITY_STATUS"));
        if (contains(status, "closed") || contains(status, "proposed") || contains(status, "planned") ||
            contains(status, "construction"))
            continue;
        const std::string cycling = lower(stringProp(props, "CYCLING"));
        BikeFeature feat;
        feat.rawClass = stringProp(props, "BIKE_FACILITY_CLASS");
        feat.name = stringProp(props, "BIKE_FACILITY_NAME");
        feat.facility = classifyYorkFacility(feat.rawClass, cycling == "true", feat.scope);
        if (feat.facility == Facility::None) continue;

        const auto& geom = f.value("geometry", nlohmann::json());
        if (!geom.is_object()) continue;
        const std::string type = geom.value("type", "");
        if (type == "LineString") {
            feat.lines.push_back(parseLine(geom["coordinates"]));
        } else if (type == "MultiLineString") {
            for (const auto& part : geom["coordinates"]) feat.lines.push_back(parseLine(part));
        }
        feat.lines.erase(std::remove_if(feat.lines.begin(), feat.lines.end(),
                                        [](const auto& l) { return l.size() < 2; }),
                         feat.lines.end());
        if (!feat.lines.empty()) out.push_back(std::move(feat));
    }
    return out;
}

}  // namespace bt
