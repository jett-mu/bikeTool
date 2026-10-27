#pragma once
// Mosaic of digital terrain model GeoTIFF tiles (any projected CRS readable by
// GDAL). Sampling is batched: points are bucketed per tile and each tile is
// read into memory once, which keeps random I/O off multi-hundred-MB rasters.

#include <array>
#include <string>
#include <vector>

#include "biketool/geo/coords.hpp"

namespace bt {

struct DemTile {
    std::string path;
    std::array<double, 6> geoTransform{};
    int width = 0, height = 0;
    double noData = -32767.0;
    bool hasNoData = false;
    double minX = 0, maxX = 0, minY = 0, maxY = 0;  // native CRS extent
    std::vector<LatLon> outline;                      // corners in WGS84, closed ring
};

// Downsampled grid of the whole mosaic in the native CRS (row 0 = north).
struct DemGrid {
    double originX = 0, originY = 0;  // top-left corner
    double cellSize = 0;
    int width = 0, height = 0;
    std::vector<float> values;  // NaN = no data
    float at(int col, int row) const { return values[static_cast<size_t>(row) * width + col]; }
};

class DemMosaic {
public:
    // Scans `dir` for *.tif / *.tiff tiles. Throws if none are usable.
    explicit DemMosaic(const std::string& dir, bool verbose = false);
    ~DemMosaic();
    DemMosaic(const DemMosaic&) = delete;
    DemMosaic& operator=(const DemMosaic&) = delete;

    const std::vector<DemTile>& tiles() const { return tiles_; }

    // Bilinear elevation in metres for each point; NaN outside coverage.
    std::vector<float> sample(const std::vector<LatLon>& pts) const;

    // Mosaic resampled to `cellSize` metres (block average).
    DemGrid downsample(double cellSize) const;

    // WGS84 <-> native CRS (all tiles must share one CRS).
    std::vector<XY> toNative(const std::vector<LatLon>& pts) const;
    std::vector<LatLon> toWgs84(const std::vector<XY>& pts) const;

private:
    std::vector<DemTile> tiles_;
    std::string wkt_;
    void* toNative_ = nullptr;  // OGRCoordinateTransformation*
    void* toWgs_ = nullptr;
    bool verbose_ = false;
};

}  // namespace bt
