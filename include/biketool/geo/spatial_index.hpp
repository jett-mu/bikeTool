#pragma once
// Uniform grid spatial index over projected coordinates. Items are 32-bit ids
// registered either at a point or over the bounding box of a segment; queries
// return candidate ids whose cells intersect a search radius (callers do the
// exact distance test, and must tolerate duplicate candidates for segments).

#include <cmath>
#include <cstdint>
#include <unordered_map>
#include <vector>

#include "biketool/geo/coords.hpp"

namespace bt {

class GridIndex {
public:
    GridIndex(LocalProjection proj, double cellSizeM) : proj_(proj), cell_(cellSizeM) {}

    const LocalProjection& projection() const { return proj_; }

    void insertPoint(uint32_t id, LatLon p) {
        const XY q = proj_.forward(p);
        cells_[key(cellOf(q.x), cellOf(q.y))].push_back(id);
    }

    void insertSegment(uint32_t id, LatLon a, LatLon b) {
        const XY p = proj_.forward(a);
        const XY q = proj_.forward(b);
        const int64_t x0 = cellOf(std::min(p.x, q.x)), x1 = cellOf(std::max(p.x, q.x));
        const int64_t y0 = cellOf(std::min(p.y, q.y)), y1 = cellOf(std::max(p.y, q.y));
        for (int64_t cx = x0; cx <= x1; ++cx)
            for (int64_t cy = y0; cy <= y1; ++cy) cells_[key(cx, cy)].push_back(id);
    }

    // Calls fn(id) for every id stored in a cell within radiusM of p.
    template <class Fn>
    void query(LatLon p, double radiusM, Fn&& fn) const {
        const XY q = proj_.forward(p);
        const int64_t x0 = cellOf(q.x - radiusM), x1 = cellOf(q.x + radiusM);
        const int64_t y0 = cellOf(q.y - radiusM), y1 = cellOf(q.y + radiusM);
        for (int64_t cx = x0; cx <= x1; ++cx)
            for (int64_t cy = y0; cy <= y1; ++cy) {
                auto it = cells_.find(key(cx, cy));
                if (it == cells_.end()) continue;
                for (uint32_t id : it->second) fn(id);
            }
    }

private:
    int64_t cellOf(double v) const { return static_cast<int64_t>(std::floor(v / cell_)); }
    static uint64_t key(int64_t cx, int64_t cy) {
        return (static_cast<uint64_t>(static_cast<uint32_t>(cx)) << 32) |
               static_cast<uint32_t>(cy);
    }

    LocalProjection proj_;
    double cell_;
    std::unordered_map<uint64_t, std::vector<uint32_t>> cells_;
};

}  // namespace bt
