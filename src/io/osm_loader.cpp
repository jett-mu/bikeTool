#include "biketool/io/osm_loader.hpp"

#include <fstream>
#include <stdexcept>

#include <nlohmann/json.hpp>

namespace bt {

const std::string& OsmWay::tag(const std::string& key) const {
    static const std::string empty;
    auto it = tags.find(key);
    return it == tags.end() ? empty : it->second;
}

OsmData loadOsmJson(const std::string& path) {
    std::ifstream in(path);
    if (!in) throw std::runtime_error("cannot open " + path);
    nlohmann::json doc;
    try {
        in >> doc;
    } catch (const nlohmann::json::exception& e) {
        throw std::runtime_error("invalid OSM JSON in " + path + ": " + e.what());
    }
    if (!doc.contains("elements")) throw std::runtime_error(path + " has no 'elements' array");

    OsmData data;
    for (const auto& el : doc["elements"]) {
        const std::string& type = el.at("type").get_ref<const std::string&>();
        if (type == "node") {
            data.nodes[el.at("id").get<int64_t>()] = {el.at("lat").get<double>(), el.at("lon").get<double>()};
        } else if (type == "way") {
            OsmWay w;
            w.id = el.at("id").get<int64_t>();
            for (const auto& n : el.at("nodes")) w.nodes.push_back(n.get<int64_t>());
            if (el.contains("tags"))
                for (const auto& [k, v] : el["tags"].items())
                    if (v.is_string()) w.tags.emplace(k, v.get<std::string>());
            data.ways.push_back(std::move(w));
        }
    }
    return data;
}

}  // namespace bt
