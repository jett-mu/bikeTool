// biketool — York Region bike routing backend.
//
//   biketool build     --osm F --bikes F --dem-dir D --out graph.bin
//   biketool route     --graph graph.bin --from LAT,LON --to LAT,LON [options]
//   biketool layers    --graph graph.bin --out-dir DIR [--dem-dir D]
//   biketool hillshade --dem-dir D --out-dir DIR
//   biketool info      --graph graph.bin
//   biketool selftest
//
// Results are JSON on stdout; failures print {"ok":false,"error":...} on
// stderr and exit non-zero.

#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <iostream>
#include <stdexcept>
#include <string>

#include <nlohmann/json.hpp>

#include "biketool/cli/args.hpp"
#include "biketool/elevation/dem.hpp"
#include "biketool/elevation/hillshade.hpp"
#include "biketool/graph/builder.hpp"
#include "biketool/io/graph_io.hpp"
#include "biketool/io/json_out.hpp"
#include "biketool/physics/energy.hpp"
#include "biketool/routing/cost_models.hpp"
#include "biketool/routing/router.hpp"

using nlohmann::json;
using namespace bt;

namespace {

using Clock = std::chrono::steady_clock;
double msSince(Clock::time_point t) {
    return std::chrono::duration<double, std::milli>(Clock::now() - t).count();
}

void usage() {
    std::fprintf(stderr,
                 "usage: biketool <command> [options]\n\n"
                 "commands:\n"
                 "  build      --osm FILE --out FILE [--bikes FILE] [--dem-dir DIR]\n"
                 "  route      --graph FILE --from LAT,LON --to LAT,LON [--profiles all|a,b]\n"
                 "             [--wind-speed M/S --wind-dir DEG --wind-factor F]\n"
                 "             [--mass KG --power W --max-power W --cda M2 --crr C --max-speed M/S]\n"
                 "  layers     --graph FILE --out-dir DIR [--dem-dir DIR]\n"
                 "  hillshade  --dem-dir DIR --out-dir DIR [--cell M]\n"
                 "  info       --graph FILE\n"
                 "  selftest\n\n"
                 "profiles: shortest, fastest, bike_friendly, flattest, least_energy, balanced\n");
}

RiderParams riderFromArgs(const Args& a) {
    RiderParams r;
    r.mass = std::clamp(a.getDouble("mass", r.mass), 30.0, 250.0);
    r.power = std::clamp(a.getDouble("power", r.power), 30.0, 600.0);
    r.maxPower = std::clamp(a.getDouble("max-power", std::max(r.maxPower, r.power * 1.6)), r.power, 1200.0);
    r.cda = std::clamp(a.getDouble("cda", r.cda), 0.15, 1.2);
    r.crr = std::clamp(a.getDouble("crr", r.crr), 0.002, 0.05);
    r.maxSpeed = std::clamp(a.getDouble("max-speed", r.maxSpeed), 3.0, 25.0);
    return r;
}

Wind windFromArgs(const Args& a) {
    Wind w;
    w.speed10m = std::clamp(a.getDouble("wind-speed", 0.0), 0.0, 60.0);
    w.fromDeg = normDeg(a.getDouble("wind-dir", 0.0));
    w.heightFactor = std::clamp(a.getDouble("wind-factor", w.heightFactor), 0.0, 1.5);
    return w;
}

int cmdBuild(const Args& a) {
    BuildOptions opt;
    opt.osmPath = a.require("osm");
    opt.bikesPath = a.get("bikes");
    opt.demDir = a.get("dem-dir");
    const std::string out = a.require("out");
    const auto t0 = Clock::now();
    BuildStats st;
    const Graph g = buildGraph(opt, st);
    std::filesystem::create_directories(std::filesystem::path(out).parent_path());
    saveGraph(g, out);
    std::cout << json{{"ok", true},
                      {"out", out},
                      {"osm_ways", st.osmWays},
                      {"usable_ways", st.usableWays},
                      {"segments", st.segments},
                      {"dropped_segments", st.droppedSegments},
                      {"bridge_segments", st.bridgeSegments},
                      {"nodes", st.nodes},
                      {"edges", st.edges},
                      {"points", st.points},
                      {"points_with_elevation", st.pointsWithElevation},
                      {"york_features", st.yorkFeatures},
                      {"york_matched_segments", st.yorkMatchedSegments},
                      {"york_upgraded_segments", st.yorkUpgradedSegments},
                      {"network_km", std::round(st.networkKm)},
                      {"seconds", std::round(msSince(t0) / 100.0) / 10.0}}
                     .dump(2)
              << "\n";
    return 0;
}

int cmdRoute(const Args& a) {
    const auto t0 = Clock::now();
    LatLon from, to;
    if (!parseLatLon(a.require("from"), from)) throw std::runtime_error("--from must be LAT,LON");
    if (!parseLatLon(a.require("to"), to)) throw std::runtime_error("--to must be LAT,LON");
    const auto profiles = parseProfiles(a.get("profiles", "all"));
    const RiderParams rider = riderFromArgs(a);
    const Wind wind = windFromArgs(a);

    const Graph g = loadGraph(a.require("graph"));
    const double loadMs = msSince(t0);
    Router router(g);
    const Snap start = router.snap(from);
    const Snap end = router.snap(to);

    const EnergyModel model(rider, wind);
    EdgePhysicsCache physics(g, model);

    json routes = json::array();
    std::vector<std::vector<Leg>> seen;
    size_t settled = 0;
    const auto tRoute = Clock::now();
    for (Profile p : profiles) {
        CostModel cost(p, g, physics);
        const PathResult res = router.route(start, end, cost);
        settled += res.settled;
        if (!res.found) continue;
        bool dup = false;
        for (size_t i = 0; i < seen.size(); ++i)
            if (seen[i] == res.legs) {
                routes[i]["also"].push_back(profileId(p));
                dup = true;
                break;
            }
        if (dup) continue;
        seen.push_back(res.legs);
        json r = routeToJson(router.describe(res.legs, model), p, model);
        r["also"] = json::array();
        routes.push_back(std::move(r));
    }

    std::cout << json{{"ok", true},
                      {"start", snapToJson(start)},
                      {"end", snapToJson(end)},
                      {"wind", {{"speed_10m_ms", wind.speed10m},
                                {"from_deg", wind.fromDeg},
                                {"rider_height_ms", std::round(wind.riderSpeed() * 100.0) / 100.0}}},
                      {"rider", {{"mass_kg", rider.mass},
                                 {"power_w", rider.power},
                                 {"max_power_w", rider.maxPower},
                                 {"cda", rider.cda},
                                 {"crr", rider.crr},
                                 {"cruise_kmh", std::round(model.cruiseSpeed() * 36.0) / 10.0}}},
                      {"routes", routes},
                      {"stats", {{"load_ms", std::round(loadMs)},
                                 {"route_ms", std::round(msSince(tRoute))},
                                 {"total_ms", std::round(msSince(t0))},
                                 {"settled_nodes", settled}}}}
                     .dump()
              << "\n";
    return 0;
}

int cmdLayers(const Args& a) {
    const Graph g = loadGraph(a.require("graph"));
    const std::string outDir = a.require("out-dir");
    std::unique_ptr<DemMosaic> dem;
    if (a.has("dem-dir")) dem = std::make_unique<DemMosaic>(a.get("dem-dir"));
    const LayerStats st = writeMapLayers(g, dem.get(), outDir);
    const BBox b = g.bounds();
    std::ofstream(outDir + "/network_meta.json")
        << json{{"bounds", {{b.minLat, b.minLon}, {b.maxLat, b.maxLon}}}, {"nodes", g.nodes.size()}, {"edges", g.edges.size()}}
               .dump(2);
    std::cout << json{{"ok", true},
                      {"out_dir", outDir},
                      {"bike_features", st.bikeFeatures},
                      {"steep_features", st.steepFeatures},
                      {"coverage_tiles", st.coverageFeatures}}
                     .dump(2)
              << "\n";
    return 0;
}

int cmdHillshade(const Args& a) {
    const DemMosaic dem(a.require("dem-dir"), true);
    OverlayOptions opt;
    opt.cellSize = std::clamp(a.getDouble("cell", opt.cellSize), 2.0, 100.0);
    const OverlayResult r = writeTerrainOverlays(dem, a.require("out-dir"), opt);
    std::cout << json{{"ok", true},
                      {"width", r.width},
                      {"height", r.height},
                      {"min_elev", r.minElev},
                      {"max_elev", r.maxElev}}
                     .dump(2)
              << "\n";
    return 0;
}

int cmdInfo(const Args& a) {
    const Graph g = loadGraph(a.require("graph"));
    double km[static_cast<size_t>(Facility::Count)] = {};
    double total = 0, withElev = 0, york = 0, osm = 0, both = 0;
    size_t covered = 0;
    for (const auto& e : g.edges) {
        if (e.reversed) continue;  // count each physical segment once where possible
        km[static_cast<size_t>(e.facility)] += e.length / 1000.0;
        total += e.length / 1000.0;
        withElev += e.length * e.elevCoverage / 1000.0;
        covered += e.elevCoverage > 0.9;
        if ((e.source & kSourceYork) && (e.source & kSourceOsm)) both += e.length / 1000.0;
        else if (e.source & kSourceYork) york += e.length / 1000.0;
        else if (e.source & kSourceOsm) osm += e.length / 1000.0;
    }
    json fac = json::object();
    for (size_t f = 0; f < static_cast<size_t>(Facility::Count); ++f)
        fac[facilityName(static_cast<Facility>(f))] = std::round(km[f]);
    const BBox b = g.bounds();
    std::cout << json{{"ok", true},
                      {"nodes", g.nodes.size()},
                      {"edges", g.edges.size()},
                      {"points", g.points.size()},
                      {"names", g.names.size()},
                      {"forward_network_km", std::round(total)},
                      {"km_with_elevation", std::round(withElev)},
                      {"forward_edges_with_elevation", covered},
                      {"facility_km", fac},
                      {"facility_source_km", {{"osm", std::round(osm)}, {"york", std::round(york)}, {"both", std::round(both)}}},
                      {"bounds", {{b.minLat, b.minLon}, {b.maxLat, b.maxLon}}}}
                     .dump(2)
              << "\n";
    return 0;
}

// ---- selftest ---------------------------------------------------------------

struct TestRun {
    int passed = 0, failed = 0;
    void check(bool ok, const std::string& what) {
        std::fprintf(stderr, "  [%s] %s\n", ok ? "PASS" : "FAIL", what.c_str());
        (ok ? passed : failed)++;
    }
};

// Adds a straight two-way edge pair with linear elevation, densified to ~10 m.
void addStraight(Graph& g, uint32_t a, uint32_t b, RoadClass road = RoadClass::Residential,
                 Facility fac = Facility::None) {
    const LatLon pa = g.nodePos(a), pb = g.nodePos(b);
    const double len = haversine(pa, pb);
    const int steps = std::max(1, static_cast<int>(std::ceil(len / 10.0)));
    const uint32_t start = static_cast<uint32_t>(g.points.size());
    double gain = 0, loss = 0, maxUp = 0, maxDown = 0;
    for (int k = 0; k <= steps; ++k) {
        const double t = static_cast<double>(k) / steps;
        const LatLon p = lerp(pa, pb, t);
        const float z = g.nodes[a].elev + static_cast<float>(t) * (g.nodes[b].elev - g.nodes[a].elev);
        g.points.push_back({static_cast<float>(p.lat), static_cast<float>(p.lon), z});
    }
    const double dz = g.nodes[b].elev - g.nodes[a].elev;
    (dz > 0 ? gain : loss) = std::fabs(dz);
    (dz > 0 ? maxUp : maxDown) = std::fabs(dz) / len;
    Edge e;
    e.geomStart = start;
    e.geomCount = static_cast<uint32_t>(steps + 1);
    e.length = static_cast<float>(len);
    e.elevCoverage = 1.0f;
    e.road = road;
    e.facility = fac;
    Edge f = e, r = e;
    f.from = a, f.to = b, f.gain = gain, f.loss = loss, f.maxGrade = maxUp;
    r.from = b, r.to = a, r.gain = loss, r.loss = gain, r.maxGrade = maxDown, r.reversed = 1;
    g.edges.push_back(f);
    g.edges.push_back(r);
}

int cmdSelftest() {
    TestRun t;
    std::fprintf(stderr, "physics\n");
    const RiderParams rider;
    const EnergyModel calm(rider, Wind{});
    t.check(calm.cruiseSpeed() > 5.5 && calm.cruiseSpeed() < 9.0, "cruise speed is plausible (" +
            std::to_string(calm.cruiseSpeed() * 3.6) + " km/h)");
    const RideResult flat = calm.ride(1000, 0, 0, Surface::Paved);
    t.check(std::fabs(flat.work - rider.power * flat.time) / flat.work < 0.02, "flat ride uses cruise power");
    const RideResult climb = calm.ride(1000, 50, 0, Surface::Paved);
    t.check(climb.work >= rider.mass * 9.80665 * 50 / rider.drivetrainEff, "climb work covers potential energy");
    t.check(climb.time > flat.time, "climbing is slower");
    t.check(calm.ride(1000, -60, 0, Surface::Paved).work == 0.0, "steep descent coasts for free");
    t.check(calm.ride(1000, -60, 0, Surface::Paved).speed <= rider.maxSpeed + 1e-9, "descent speed is capped");
    const EnergyModel windy(rider, Wind{6.0, 0.0, 1.0});  // 6 m/s from the north
    t.check(windy.ride(1000, 0, 0, Surface::Paved).work > flat.work * 1.2, "headwind costs more energy");
    t.check(windy.ride(1000, 0, 180, Surface::Paved).time < flat.time, "tailwind is faster");
    t.check(calm.ride(1000, 0, 0, Surface::Gravel).work > flat.work, "gravel costs more than asphalt");

    std::fprintf(stderr, "routing\n");
    // 3x3 grid, ~200 m spacing; centre node sits on a 40 m hill.
    Graph g;
    g.names.push_back("");
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c)
            g.nodes.push_back({43.85f + r * 0.0018f, -79.40f + c * 0.0025f, (r == 1 && c == 1) ? 240.0f : 200.0f});
    auto id = [](int r, int c) { return static_cast<uint32_t>(r * 3 + c); };
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 2; ++c) addStraight(g, id(r, c), id(r, c + 1));
    for (int r = 0; r < 2; ++r)
        for (int c = 0; c < 3; ++c)
            addStraight(g, id(r, c), id(r + 1, c), RoadClass::Residential,
                        c == 0 ? Facility::OffRoadTrail : Facility::None);
    std::stable_sort(g.edges.begin(), g.edges.end(), [](const Edge& x, const Edge& y) { return x.from < y.from; });
    g.buildCsr();

    Router router(g);
    const EnergyModel model(rider, Wind{});
    EdgePhysicsCache physics(g, model);
    const Snap s = router.snap(g.nodePos(id(1, 0)));
    const Snap e = router.snap(g.nodePos(id(1, 2)));
    t.check(s.distance < 1.0 && e.distance < 1.0, "snaps to nodes");

    CostModel shortest(Profile::Shortest, g, physics);
    const PathResult ps = router.route(s, e, shortest);
    const RouteDetail ds = router.describe(ps.legs, model);
    t.check(ps.found && ds.gain > 30.0, "shortest goes over the hill");

    CostModel flattest(Profile::Flattest, g, physics);
    const PathResult pf = router.route(s, e, flattest);
    const RouteDetail df = router.describe(pf.legs, model);
    t.check(pf.found && df.gain < 1.0 && df.distance > ds.distance, "flattest detours around the hill");

    CostModel energy(Profile::LeastEnergy, g, physics);
    const RouteDetail de = router.describe(router.route(s, e, energy).legs, model);
    t.check(de.work <= ds.work + 1.0, "least-energy uses no more energy than shortest");

    const Snap mid1 = router.snap(lerp(g.nodePos(id(0, 0)), g.nodePos(id(0, 1)), 0.3));
    const Snap mid2 = router.snap(lerp(g.nodePos(id(0, 0)), g.nodePos(id(0, 1)), 0.8));
    const PathResult pm = router.route(mid1, mid2, shortest);
    const RouteDetail dm = router.describe(pm.legs, model);
    t.check(pm.found && pm.legs.size() == 1 && std::fabs(dm.distance - 0.5 * g.edges[0].length) < 15.0,
            "mid-edge to mid-edge on one street");

    std::fprintf(stderr, "graph io\n");
    const std::string tmp = (std::filesystem::temp_directory_path() / "biketool_selftest.bin").string();
    saveGraph(g, tmp);
    const Graph g2 = loadGraph(tmp);
    std::filesystem::remove(tmp);
    t.check(g2.nodes.size() == g.nodes.size() && g2.edges.size() == g.edges.size() &&
                g2.points.size() == g.points.size() && g2.firstOut == g.firstOut,
            "binary round trip");

    std::fprintf(stderr, "%d passed, %d failed\n", t.passed, t.failed);
    std::cout << json{{"ok", t.failed == 0}, {"passed", t.passed}, {"failed", t.failed}}.dump() << "\n";
    return t.failed == 0 ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
    try {
        const Args args(argc, argv);
        const std::string& cmd = args.command();
        if (cmd == "build") return cmdBuild(args);
        if (cmd == "route") return cmdRoute(args);
        if (cmd == "layers") return cmdLayers(args);
        if (cmd == "hillshade") return cmdHillshade(args);
        if (cmd == "info") return cmdInfo(args);
        if (cmd == "selftest") return cmdSelftest();
        usage();
        return cmd.empty() || cmd == "help" ? 0 : 2;
    } catch (const std::exception& e) {
        std::cerr << json{{"ok", false}, {"error", e.what()}}.dump() << "\n";
        return 1;
    }
}
