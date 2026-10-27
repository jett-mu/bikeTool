#include "biketool/routing/router.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <queue>
#include <stdexcept>
#include <unordered_map>

namespace bt {
namespace {

constexpr uint32_t kNone = std::numeric_limits<uint32_t>::max();
constexpr double kInf = std::numeric_limits<double>::infinity();

struct QueueItem {
    double f;
    double g;
    uint32_t node;
    bool operator>(const QueueItem& o) const { return f > o.f; }
};

}  // namespace

Router::Router(const Graph& g) : g_(g) {
    byGeom_.resize(g.edges.size());
    for (uint32_t i = 0; i < byGeom_.size(); ++i) byGeom_[i] = i;
    std::sort(byGeom_.begin(), byGeom_.end(),
              [&](uint32_t a, uint32_t b) { return g.edges[a].geomStart < g.edges[b].geomStart; });
    dist_.assign(g.nodes.size(), kInf);
    parentEdge_.assign(g.nodes.size(), kNone);
    visitStamp_.assign(g.nodes.size(), 0);
}

Snap Router::snap(LatLon p) const {
    if (g_.points.empty()) throw std::runtime_error("graph has no geometry");
    // Linear scan with an equirectangular metric: ~2M points takes a few ms,
    // cheaper than building an index for a one-shot CLI query.
    const double kx = std::cos(deg2rad(p.lat));
    double best = kInf;
    uint32_t bestIdx = 0;
    for (uint32_t i = 0; i < g_.points.size(); ++i) {
        const double dx = (g_.points[i].lon - p.lon) * kx;
        const double dy = g_.points[i].lat - p.lat;
        const double d = dx * dx + dy * dy;
        if (d < best) {
            best = d;
            bestIdx = i;
        }
    }
    Snap s;
    s.point = bestIdx;
    s.requested = p;
    s.snapped = {g_.points[bestIdx].lat, g_.points[bestIdx].lon};
    s.distance = haversine(p, s.snapped);
    return s;
}

std::vector<Router::EdgeAt> Router::edgesAtPoint(uint32_t point) const {
    std::vector<EdgeAt> out;
    auto it = std::upper_bound(byGeom_.begin(), byGeom_.end(), point,
                               [&](uint32_t p, uint32_t e) { return p < g_.edges[e].geomStart; });
    if (it == byGeom_.begin()) return out;
    const uint32_t gs = g_.edges[*(it - 1)].geomStart;
    for (auto jt = it; jt != byGeom_.begin();) {
        --jt;
        const Edge& e = g_.edges[*jt];
        if (e.geomStart != gs) break;
        if (point >= gs + e.geomCount) continue;
        const uint32_t local = point - gs;
        out.push_back({*jt, e.reversed ? e.geomCount - 1 - local : local});
    }
    return out;
}

double Router::legLength(const Leg& leg) const {
    const Edge& e = g_.edges[leg.edge];
    if (leg.from == 0 && leg.to == e.geomCount - 1) return e.length;
    double len = 0;
    for (uint32_t i = leg.from; i < leg.to; ++i) {
        const GeoPoint& a = g_.edgePoint(e, i);
        const GeoPoint& b = g_.edgePoint(e, i + 1);
        len += haversine({a.lat, a.lon}, {b.lat, b.lon});
    }
    return len;
}

PathResult Router::route(const Snap& start, const Snap& end, CostModel& cost) {
    PathResult res;
    ++stamp_;
    auto legCost = [&](const Leg& leg) {
        const Edge& e = g_.edges[leg.edge];
        if (leg.to <= leg.from) return 0.0;
        const double frac = e.length > 0 ? legLength(leg) / e.length : 1.0;
        return cost.edgeCost(leg.edge) * std::min(1.0, frac);
    };

    std::priority_queue<QueueItem, std::vector<QueueItem>, std::greater<>> heap;
    std::unordered_map<uint32_t, std::optional<Leg>> startLeg;  // initial node -> leg that reached it
    auto h = [&](uint32_t n) { return cost.heuristic(haversine(g_.nodePos(n), end.snapped)); };
    auto seed = [&](uint32_t node, double g, std::optional<Leg> leg) {
        if (visitStamp_[node] == stamp_ && dist_[node] <= g) return;
        visitStamp_[node] = stamp_;
        dist_[node] = g;
        parentEdge_[node] = kNone;
        startLeg[node] = leg;
        heap.push({g + h(node), g, node});
    };

    const auto startEdges = edgesAtPoint(start.point);
    const auto endEdges = edgesAtPoint(end.point);
    for (const EdgeAt& s : startEdges) {
        const Edge& e = g_.edges[s.edge];
        const uint32_t last = e.geomCount - 1;
        if (s.index == 0) seed(e.from, 0.0, std::nullopt);
        if (s.index == last) seed(e.to, 0.0, std::nullopt);
        else {
            const Leg leg{s.edge, s.index, last};
            seed(e.to, legCost(leg), leg);
        }
    }

    // Arriving at a target node completes the route with an optional final leg.
    struct Target {
        double extra;
        std::optional<Leg> leg;
    };
    std::unordered_map<uint32_t, Target> targets;
    auto addTarget = [&](uint32_t node, double extra, std::optional<Leg> leg) {
        auto it = targets.find(node);
        if (it == targets.end() || extra < it->second.extra) targets[node] = {extra, leg};
    };
    for (const EdgeAt& t : endEdges) {
        const Edge& e = g_.edges[t.edge];
        if (t.index == 0) addTarget(e.from, 0.0, std::nullopt);
        else {
            const Leg leg{t.edge, 0, t.index};
            addTarget(e.from, legCost(leg), leg);
        }
        if (t.index == e.geomCount - 1) addTarget(e.to, 0.0, std::nullopt);
    }

    double best = kInf;
    std::vector<Leg> bestDirect;
    uint32_t bestNode = kNone;
    // Start and end on the same edge, in travel order.
    for (const EdgeAt& s : startEdges)
        for (const EdgeAt& t : endEdges)
            if (s.edge == t.edge && s.index <= t.index) {
                const Leg leg{s.edge, s.index, t.index};
                const double c = legCost(leg);
                if (c < best) {
                    best = c;
                    bestDirect = s.index < t.index ? std::vector<Leg>{leg} : std::vector<Leg>{};
                    res.found = true;
                }
            }

    while (!heap.empty()) {
        const QueueItem item = heap.top();
        heap.pop();
        if (item.f >= best) break;
        if (item.g > dist_[item.node]) continue;
        ++res.settled;
        if (auto it = targets.find(item.node); it != targets.end()) {
            const double total = item.g + it->second.extra;
            if (total < best) {
                best = total;
                bestNode = item.node;
                res.found = true;
            }
        }
        for (uint32_t ei = g_.firstOut[item.node]; ei < g_.firstOut[item.node + 1]; ++ei) {
            const uint32_t v = g_.edges[ei].to;
            const double ng = item.g + cost.edgeCost(ei);
            if (visitStamp_[v] == stamp_ && ng >= dist_[v]) continue;
            visitStamp_[v] = stamp_;
            dist_[v] = ng;
            parentEdge_[v] = ei;
            heap.push({ng + h(v), ng, v});
        }
    }

    if (!res.found) return res;
    res.cost = best;
    if (bestNode == kNone) {
        res.legs = bestDirect;
        return res;
    }
    std::vector<Leg> legs;
    if (const auto& endLeg = targets[bestNode].leg) legs.push_back(*endLeg);
    uint32_t n = bestNode;
    while (parentEdge_[n] != kNone) {
        const Edge& e = g_.edges[parentEdge_[n]];
        legs.push_back({parentEdge_[n], 0, e.geomCount - 1});
        n = e.from;
    }
    if (const auto& sl = startLeg[n]) legs.push_back(*sl);
    std::reverse(legs.begin(), legs.end());
    res.legs = std::move(legs);
    return res;
}

RouteDetail Router::describe(const std::vector<Leg>& legs, const EnergyModel& model) const {
    RouteDetail d;
    double covered = 0, headwindSum = 0;
    for (const Leg& leg : legs) {
        const Edge& e = g_.edges[leg.edge];
        const std::string& name = g_.names[e.nameIdx];
        if (d.steps.empty() || d.steps.back().name != name || d.steps.back().facility != e.facility) {
            RouteStep st;
            st.name = name;
            st.facility = e.facility;
            st.road = e.road;
            st.startIndex = d.coords.empty() ? 0 : d.coords.size() - 1;
            d.steps.push_back(st);
        }
        RouteStep& step = d.steps.back();
        for (uint32_t i = leg.from; i <= leg.to; ++i) {
            const GeoPoint& p = g_.edgePoint(e, i);
            const LatLon pos{p.lat, p.lon};
            if (i == leg.from && !d.coords.empty()) {
                if (std::isnan(d.profile.back().elev)) d.profile.back().elev = p.elev;
                continue;  // shared with previous leg's last point
            }
            if (!d.coords.empty() && i > leg.from) {
                const GeoPoint& q = g_.edgePoint(e, i - 1);
                const LatLon prev{q.lat, q.lon};
                const double len = haversine(prev, pos);
                const bool hasZ = !std::isnan(p.elev) && !std::isnan(q.elev);
                const double rise = hasZ ? p.elev - q.elev : 0.0;
                const double brg = bearingDeg(prev, pos);
                const RideResult r = model.ride(len, rise, brg, e.surface);
                d.time += r.time;
                d.work += r.work;
                d.distance += len;
                step.length += len;
                headwindSum += model.wind().headwind(brg) * len;
                d.facilityLength[static_cast<size_t>(e.facility)] += len;
                if (hasZ) {
                    covered += len;
                    (rise > 0 ? d.gain : d.loss) += std::fabs(rise);
                }
            }
            d.coords.push_back(pos);
            d.profile.push_back({d.distance, p.elev, 0.0});
        }
        step.endIndex = d.coords.size() - 1;
    }
    d.avgHeadwind = d.distance > 0 ? headwindSum / d.distance : 0;
    d.elevCoverage = d.distance > 0 ? covered / d.distance : 0;

    // Grade at each point over a +/-25 m window.
    auto& pr = d.profile;
    for (size_t i = 0, lo = 0, hi = 0; i < pr.size(); ++i) {
        while (lo < i && pr[i].dist - pr[lo].dist > 25.0) ++lo;
        while (hi + 1 < pr.size() && pr[hi + 1].dist - pr[i].dist <= 25.0) ++hi;
        if (hi < i) hi = i;
        const double span = pr[hi].dist - pr[lo].dist;
        if (span > 5.0 && !std::isnan(pr[hi].elev) && !std::isnan(pr[lo].elev)) {
            pr[i].grade = (pr[hi].elev - pr[lo].elev) / span;
            d.maxGrade = std::max(d.maxGrade, pr[i].grade);
        }
    }
    return d;
}

}  // namespace bt
