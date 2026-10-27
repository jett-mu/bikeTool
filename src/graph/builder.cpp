#include "biketool/graph/builder.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <numeric>
#include <optional>
#include <unordered_map>

#include "biketool/elevation/dem.hpp"
#include "biketool/geo/spatial_index.hpp"
#include "biketool/io/bike_layer_loader.hpp"
#include "biketool/io/osm_loader.hpp"

namespace bt {
namespace {

void log(bool verbose, const char* fmt, auto... args) {
    if (verbose) std::fprintf(stderr, fmt, args...);
}

std::optional<RoadClass> roadClassOf(const std::string& hw) {
    static const std::unordered_map<std::string, RoadClass> table = {
        {"trunk", RoadClass::Trunk}, {"trunk_link", RoadClass::Trunk},
        {"primary", RoadClass::Primary}, {"primary_link", RoadClass::Primary},
        {"secondary", RoadClass::Secondary}, {"secondary_link", RoadClass::Secondary},
        {"tertiary", RoadClass::Tertiary}, {"tertiary_link", RoadClass::Tertiary},
        {"unclassified", RoadClass::Unclassified}, {"residential", RoadClass::Residential},
        {"living_street", RoadClass::LivingStreet}, {"service", RoadClass::Service},
        {"cycleway", RoadClass::Cycleway}, {"path", RoadClass::Path}, {"bridleway", RoadClass::Path},
        {"footway", RoadClass::Footway}, {"track", RoadClass::Track}, {"pedestrian", RoadClass::Pedestrian}};
    auto it = table.find(hw);
    if (it == table.end()) return std::nullopt;
    return it->second;
}

Surface surfaceOf(const OsmWay& w, RoadClass road) {
    const std::string& s = w.tag("surface");
    if (s.empty()) {
        const std::string& tt = w.tag("tracktype");
        if (road == RoadClass::Track) return tt == "grade1" ? Surface::Paved : Surface::Gravel;
        if (road == RoadClass::Path) return Surface::Compacted;
        return Surface::Paved;
    }
    if (s == "asphalt" || s == "paved" || s == "concrete" || s == "paving_stones" || s == "sett" ||
        s == "chipseal" || s.rfind("concrete:", 0) == 0 || s == "metal" || s == "wood")
        return Surface::Paved;
    if (s == "compacted" || s == "fine_gravel") return Surface::Compacted;
    if (s == "gravel" || s == "pebblestone" || s == "unhewn_cobblestone") return Surface::Gravel;
    return Surface::Unpaved;  // dirt, ground, grass, sand, mud, unpaved...
}

Facility facilityFromValue(const std::string& v, bool buffered) {
    if (v == "track" || v == "opposite_track") return Facility::CycleTrack;
    if (v == "lane" || v == "opposite_lane") return buffered ? Facility::BufferedLane : Facility::BikeLane;
    if (v == "shoulder") return Facility::PavedShoulder;
    if (v == "shared_lane" || v == "share_busway" || v == "opposite_share_busway") return Facility::Sharrow;
    return Facility::None;
}

Facility maxFacility(Facility a, Facility b) { return static_cast<uint8_t>(a) >= static_cast<uint8_t>(b) ? a : b; }

// Facility for one travel direction; sides are the keys to consider.
Facility osmFacility(const OsmWay& w, RoadClass road, std::initializer_list<const char*> sides) {
    Facility f = Facility::None;
    if (road == RoadClass::Cycleway) f = Facility::OffRoadTrail;
    const std::string& bicycle = w.tag("bicycle");
    if ((road == RoadClass::Path || road == RoadClass::Footway || road == RoadClass::Pedestrian) &&
        bicycle == "designated")
        f = Facility::OffRoadTrail;
    for (const char* side : sides) {
        const std::string key = side;
        const std::string& buf = w.tag(key + ":buffer");
        const bool buffered = (!buf.empty() && buf != "no") || !w.tag(key + ":separation").empty();
        f = maxFacility(f, facilityFromValue(w.tag(key), buffered));
    }
    return f;
}

struct Access {
    bool forward = true;
    bool backward = true;
};

Access accessOf(const OsmWay& w) {
    Access a;
    std::string oneway = w.tag("oneway");
    const std::string& junction = w.tag("junction");
    if (oneway.empty() && (junction == "roundabout" || junction == "circular")) oneway = "yes";
    if (oneway == "yes" || oneway == "true" || oneway == "1") a.backward = false;
    if (oneway == "-1" || oneway == "reverse") a.forward = false;

    bool contraflow = w.tag("oneway:bicycle") == "no";
    for (const char* key : {"cycleway", "cycleway:left", "cycleway:right", "cycleway:both"}) {
        const std::string& v = w.tag(key);
        if (v.rfind("opposite", 0) == 0) contraflow = true;
        const std::string& ow = w.tag(std::string(key) + ":oneway");
        if (ow == "no" || ow == "-1") contraflow = true;
    }
    if (contraflow) a.forward = a.backward = true;
    return a;
}

bool usableWay(const OsmWay& w) {
    const std::string& access = w.tag("access");
    const std::string& bicycle = w.tag("bicycle");
    if (bicycle == "no" || bicycle == "dismount") return false;
    if ((access == "private" || access == "no") && bicycle != "yes" && bicycle != "designated") return false;
    if (w.tag("area") == "yes") return false;
    return w.nodes.size() >= 2;
}

struct Segment {
    uint32_t a = 0, b = 0;           // graph node ids
    std::vector<LatLon> pts;          // densified
    std::vector<float> elev;
    std::string name;
    RoadClass road = RoadClass::Other;
    Surface surface = Surface::Paved;
    Facility facFwd = Facility::None, facBwd = Facility::None;
    uint8_t srcFwd = kSourceNone, srcBwd = kSourceNone;
    Access access;
    bool bridge = false;
};

void densifyInto(std::vector<LatLon>& out, LatLon a, LatLon b, double spacing) {
    const double d = haversine(a, b);
    const int steps = std::max(1, static_cast<int>(std::ceil(d / spacing)));
    for (int k = 1; k < steps; ++k) out.push_back(lerp(a, b, static_cast<double>(k) / steps));
    out.push_back(b);
}

struct UnionFind {
    std::vector<uint32_t> parent, size;
    explicit UnionFind(size_t n) : parent(n), size(n, 1) { std::iota(parent.begin(), parent.end(), 0u); }
    uint32_t find(uint32_t x) {
        while (parent[x] != x) x = parent[x] = parent[parent[x]];
        return x;
    }
    void unite(uint32_t a, uint32_t b) {
        a = find(a);
        b = find(b);
        if (a == b) return;
        if (size[a] < size[b]) std::swap(a, b);
        parent[b] = a;
        size[a] += size[b];
    }
};

// ---- York facility matching ------------------------------------------------

struct YorkPiece {
    LatLon a, b;
    uint32_t feature;
};

void matchYorkFacilities(std::vector<Segment>& segs, const std::vector<BikeFeature>& feats,
                         const BuildOptions& opt, BuildStats& stats, const LocalProjection& proj) {
    std::vector<YorkPiece> pieces;
    GridIndex index(proj, 30.0);
    for (uint32_t f = 0; f < feats.size(); ++f)
        for (const auto& line : feats[f].lines)
            for (size_t i = 0; i + 1 < line.size(); ++i) {
                index.insertSegment(static_cast<uint32_t>(pieces.size()), line[i], line[i + 1]);
                pieces.push_back({line[i], line[i + 1], f});
            }

    std::vector<uint32_t> votes(static_cast<size_t>(Facility::Count));
    for (auto& seg : segs) {
        if (seg.pts.size() < 2) continue;
        std::fill(votes.begin(), votes.end(), 0);
        size_t matched = 0;
        const bool motor = isMotorRoad(seg.road);
        for (size_t i = 0; i < seg.pts.size(); ++i) {
            const LatLon p = seg.pts[i];
            const LatLon q = seg.pts[i + 1 < seg.pts.size() ? i + 1 : i - 1];
            const double localBearing = bearingDeg(p, q);
            const XY px = proj.forward(p);
            double bestDist = opt.matchRadiusM;
            Facility best = Facility::None;
            index.query(p, opt.matchRadiusM, [&](uint32_t id) {
                const YorkPiece& yp = pieces[id];
                const BikeFeature& feat = feats[yp.feature];
                if (feat.scope == MatchScope::OnRoad && !motor) return;
                if (feat.scope == MatchScope::OffRoad && motor) return;
                if (orientationDiff(localBearing, bearingDeg(yp.a, yp.b)) > opt.matchAngleDeg) return;
                const double d = pointSegmentDistance(px, proj.forward(yp.a), proj.forward(yp.b));
                if (d <= bestDist) {
                    bestDist = d;
                    best = feat.facility;
                }
            });
            if (best != Facility::None) {
                ++matched;
                ++votes[static_cast<size_t>(best)];
            }
        }
        if (static_cast<double>(matched) / seg.pts.size() < opt.matchFraction) continue;
        const auto top = static_cast<Facility>(std::max_element(votes.begin(), votes.end()) - votes.begin());
        ++stats.yorkMatchedSegments;
        bool upgraded = false;
        auto apply = [&](Facility& fac, uint8_t& src) {
            if (static_cast<uint8_t>(top) > static_cast<uint8_t>(fac)) {
                fac = top;
                src = kSourceYork;
                upgraded = true;
            } else {
                src |= kSourceYork;
            }
        };
        apply(seg.facFwd, seg.srcFwd);
        apply(seg.facBwd, seg.srcBwd);
        if (upgraded) ++stats.yorkUpgradedSegments;
    }
}

// ---- Elevation post-processing -------------------------------------------

void smoothAndFlatten(Segment& seg) {
    auto& z = seg.elev;
    const size_t n = z.size();
    if (n < 3) return;
    if (seg.bridge && !std::isnan(z.front()) && !std::isnan(z.back())) {
        // Bare-earth DTMs drop into the valley under a bridge; span it instead.
        double total = 0;
        std::vector<double> cum(n, 0.0);
        for (size_t i = 1; i < n; ++i) cum[i] = total += haversine(seg.pts[i - 1], seg.pts[i]);
        for (size_t i = 1; i + 1 < n; ++i)
            z[i] = static_cast<float>(z.front() + (z.back() - z.front()) * (total > 0 ? cum[i] / total : 0.0));
        return;
    }
    // 5-point moving average on interior points; endpoints stay fixed so
    // segments sharing a junction agree on its height.
    std::vector<float> s = z;
    for (size_t i = 1; i + 1 < n; ++i) {
        double sum = 0;
        int cnt = 0;
        for (size_t j = (i >= 2 ? i - 2 : 0); j <= std::min(n - 1, i + 2); ++j)
            if (!std::isnan(z[j])) {
                sum += z[j];
                ++cnt;
            }
        s[i] = cnt ? static_cast<float>(sum / cnt) : z[i];
    }
    z = std::move(s);
}

struct ElevStats {
    float gain = 0, loss = 0, maxUp = 0, maxDown = 0, coverage = 0;
};

ElevStats elevationStats(const Segment& seg, double length) {
    ElevStats st;
    const auto& z = seg.elev;
    const auto& p = seg.pts;
    double covered = 0;
    std::vector<double> cum(p.size(), 0.0);
    for (size_t i = 1; i < p.size(); ++i) {
        const double d = haversine(p[i - 1], p[i]);
        cum[i] = cum[i - 1] + d;
        if (std::isnan(z[i - 1]) || std::isnan(z[i])) continue;
        covered += d;
        const float dz = z[i] - z[i - 1];
        (dz > 0 ? st.gain : st.loss) += std::fabs(dz);
    }
    // Grades over >= 25 m windows to avoid single-sample spikes.
    for (size_t i = 0, j = 0; i < p.size(); ++i) {
        while (j < p.size() && cum[j] - cum[i] < 25.0) ++j;
        if (j >= p.size()) break;
        if (std::isnan(z[i]) || std::isnan(z[j])) continue;
        const float g = static_cast<float>((z[j] - z[i]) / (cum[j] - cum[i]));
        st.maxUp = std::max(st.maxUp, g);
        st.maxDown = std::max(st.maxDown, -g);
    }
    st.coverage = length > 0 ? static_cast<float>(covered / length) : 0.0f;
    return st;
}

}  // namespace

Graph buildGraph(const BuildOptions& opt, BuildStats& stats) {
    const bool v = opt.verbose;
    log(v, "Loading OSM data from %s...\n", opt.osmPath.c_str());
    OsmData osm = loadOsmJson(opt.osmPath);
    stats.osmWays = osm.ways.size();
    log(v, "  %zu ways, %zu nodes\n", osm.ways.size(), osm.nodes.size());

    // Count how many usable ways reference each node to find junctions.
    std::vector<const OsmWay*> ways;
    std::unordered_map<int64_t, uint32_t> useCount;
    for (const auto& w : osm.ways) {
        if (!usableWay(w) || !roadClassOf(w.tag("highway"))) continue;
        ways.push_back(&w);
        for (size_t i = 0; i < w.nodes.size(); ++i) {
            // Endpoints always count as junctions (+2), interior nodes once per way.
            useCount[w.nodes[i]] += (i == 0 || i + 1 == w.nodes.size()) ? 2 : 1;
        }
    }
    stats.usableWays = ways.size();

    std::unordered_map<int64_t, uint32_t> graphNode;
    std::vector<LatLon> nodePos;
    auto nodeFor = [&](int64_t osmId, LatLon p) {
        auto [it, inserted] = graphNode.emplace(osmId, static_cast<uint32_t>(nodePos.size()));
        if (inserted) nodePos.push_back(p);
        return it->second;
    };

    std::vector<Segment> segs;
    for (const OsmWay* w : ways) {
        const RoadClass road = *roadClassOf(w->tag("highway"));
        const Surface surface = surfaceOf(*w, road);
        const Access access = accessOf(*w);
        Facility fwd, bwd;
        if (!access.backward || !access.forward) {
            fwd = bwd = osmFacility(*w, road, {"cycleway", "cycleway:both", "cycleway:right", "cycleway:left"});
        } else {
            fwd = osmFacility(*w, road, {"cycleway", "cycleway:both", "cycleway:right"});
            bwd = osmFacility(*w, road, {"cycleway", "cycleway:both", "cycleway:left"});
        }
        const std::string& bridgeTag = w->tag("bridge");
        const bool bridge = (!bridgeTag.empty() && bridgeTag != "no") ||
                            (!w->tag("tunnel").empty() && w->tag("tunnel") != "no");
        std::string name = w->tag("name");
        if (name.empty()) name = w->tag("ref");

        size_t start = 0;
        for (size_t i = 1; i < w->nodes.size(); ++i) {
            const bool last = i + 1 == w->nodes.size();
            if (!last && useCount[w->nodes[i]] < 2) continue;
            Segment seg;
            bool complete = true;
            for (size_t k = start; k <= i; ++k) {
                auto it = osm.nodes.find(w->nodes[k]);
                if (it == osm.nodes.end()) {
                    complete = false;
                    break;
                }
                if (seg.pts.empty()) seg.pts.push_back(it->second);
                else densifyInto(seg.pts, seg.pts.back(), it->second, opt.densifyM);
            }
            const int64_t aId = w->nodes[start], bId = w->nodes[i];
            start = i;
            if (!complete || aId == bId || seg.pts.size() < 2) {
                ++stats.droppedSegments;
                continue;
            }
            seg.a = nodeFor(aId, seg.pts.front());
            seg.b = nodeFor(bId, seg.pts.back());
            seg.name = name;
            seg.road = road;
            seg.surface = surface;
            seg.facFwd = fwd;
            seg.facBwd = bwd;
            seg.srcFwd = fwd != Facility::None ? kSourceOsm : kSourceNone;
            seg.srcBwd = bwd != Facility::None ? kSourceOsm : kSourceNone;
            seg.access = access;
            seg.bridge = bridge;
            stats.bridgeSegments += bridge;
            segs.push_back(std::move(seg));
        }
    }
    osm = OsmData{};  // release memory
    log(v, "  %zu segments, %zu junction nodes\n", segs.size(), nodePos.size());

    // Keep only the largest connected component so every snap is routable.
    {
        UnionFind uf(nodePos.size());
        for (const auto& s : segs) uf.unite(s.a, s.b);
        uint32_t bestRoot = 0, bestSize = 0;
        for (uint32_t n = 0; n < nodePos.size(); ++n)
            if (uf.find(n) == n && uf.size[n] > bestSize) {
                bestSize = uf.size[n];
                bestRoot = n;
            }
        const size_t before = segs.size();
        std::erase_if(segs, [&](const Segment& s) { return uf.find(s.a) != bestRoot; });
        stats.droppedSegments += before - segs.size();
        log(v, "  largest component keeps %zu of %zu segments\n", segs.size(), before);
    }

    BBox box;
    for (const auto& p : nodePos) box.extend(p);
    const LocalProjection proj({(box.minLat + box.maxLat) / 2, (box.minLon + box.maxLon) / 2});

    if (!opt.bikesPath.empty()) {
        log(v, "Matching York Region bike facilities from %s...\n", opt.bikesPath.c_str());
        const auto feats = loadYorkBikeLayer(opt.bikesPath);
        stats.yorkFeatures = feats.size();
        matchYorkFacilities(segs, feats, opt, stats, proj);
        log(v, "  %zu usable features; %zu segments matched, %zu upgraded\n", feats.size(),
            stats.yorkMatchedSegments, stats.yorkUpgradedSegments);
    }

    for (auto& s : segs) s.elev.assign(s.pts.size(), kNoElevation);
    if (!opt.demDir.empty()) {
        log(v, "Sampling elevation from %s...\n", opt.demDir.c_str());
        DemMosaic dem(opt.demDir, v);
        std::vector<LatLon> all;
        for (const auto& s : segs) all.insert(all.end(), s.pts.begin(), s.pts.end());
        const std::vector<float> z = dem.sample(all);
        size_t k = 0;
        for (auto& s : segs)
            for (auto& e : s.elev) e = z[k++];
        for (auto& s : segs) smoothAndFlatten(s);
    }

    // ---- Assemble graph -----------------------------------------------------
    Graph g;
    g.names.push_back("");
    std::unordered_map<std::string, uint32_t> nameIdx{{"", 0}};
    std::vector<uint32_t> remap(nodePos.size(), UINT32_MAX);
    auto mapNode = [&](uint32_t n, float elev) {
        if (remap[n] == UINT32_MAX) {
            remap[n] = static_cast<uint32_t>(g.nodes.size());
            g.nodes.push_back({static_cast<float>(nodePos[n].lat), static_cast<float>(nodePos[n].lon), elev});
        }
        return remap[n];
    };

    for (const auto& s : segs) {
        const uint32_t a = mapNode(s.a, s.elev.front());
        const uint32_t b = mapNode(s.b, s.elev.back());
        auto [it, inserted] = nameIdx.emplace(s.name, static_cast<uint32_t>(g.names.size()));
        if (inserted) g.names.push_back(s.name);

        double length = 0;
        for (size_t i = 1; i < s.pts.size(); ++i) length += haversine(s.pts[i - 1], s.pts[i]);
        const ElevStats es = elevationStats(s, length);
        stats.networkKm += length / 1000.0;

        const uint32_t geomStart = static_cast<uint32_t>(g.points.size());
        for (size_t i = 0; i < s.pts.size(); ++i) {
            g.points.push_back({static_cast<float>(s.pts[i].lat), static_cast<float>(s.pts[i].lon), s.elev[i]});
            stats.pointsWithElevation += !std::isnan(s.elev[i]);
        }

        Edge e;
        e.geomStart = geomStart;
        e.geomCount = static_cast<uint32_t>(s.pts.size());
        e.nameIdx = it->second;
        e.length = static_cast<float>(length);
        e.elevCoverage = es.coverage;
        e.road = s.road;
        e.surface = s.surface;
        if (s.access.forward) {
            Edge f = e;
            f.from = a;
            f.to = b;
            f.gain = es.gain;
            f.loss = es.loss;
            f.maxGrade = es.maxUp;
            f.facility = s.facFwd;
            f.source = s.srcFwd;
            g.edges.push_back(f);
        }
        if (s.access.backward) {
            Edge r = e;
            r.from = b;
            r.to = a;
            r.gain = es.loss;
            r.loss = es.gain;
            r.maxGrade = es.maxDown;
            r.facility = s.facBwd;
            r.source = s.srcBwd;
            r.reversed = 1;
            g.edges.push_back(r);
        }
    }
    std::stable_sort(g.edges.begin(), g.edges.end(), [](const Edge& x, const Edge& y) { return x.from < y.from; });
    g.buildCsr();

    stats.segments = segs.size();
    stats.nodes = g.nodes.size();
    stats.edges = g.edges.size();
    stats.points = g.points.size();
    log(v, "Graph: %zu nodes, %zu edges, %.0f km of network\n", stats.nodes, stats.edges, stats.networkKm);
    return g;
}

}  // namespace bt
