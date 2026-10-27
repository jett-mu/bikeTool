#include "biketool/elevation/dem.hpp"

#include <gdal_priv.h>
#include <ogr_spatialref.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <limits>
#include <stdexcept>

namespace bt {
namespace {

constexpr float kNaN = std::numeric_limits<float>::quiet_NaN();

OGRCoordinateTransformation* ct(void* p) { return static_cast<OGRCoordinateTransformation*>(p); }

struct TileRaster {
    std::vector<float> data;
    int width = 0, height = 0;
};

TileRaster readTile(const DemTile& t) {
    auto* ds = static_cast<GDALDataset*>(GDALOpen(t.path.c_str(), GA_ReadOnly));
    if (!ds) throw std::runtime_error("cannot open " + t.path);
    TileRaster r;
    r.width = t.width;
    r.height = t.height;
    r.data.resize(static_cast<size_t>(r.width) * r.height);
    const CPLErr err = ds->GetRasterBand(1)->RasterIO(GF_Read, 0, 0, r.width, r.height, r.data.data(),
                                                     r.width, r.height, GDT_Float32, 0, 0);
    GDALClose(ds);
    if (err != CE_None) throw std::runtime_error("read failed: " + t.path);
    if (t.hasNoData)
        for (auto& v : r.data)
            if (v == static_cast<float>(t.noData) || v < -1000.0f) v = kNaN;
    return r;
}

}  // namespace

DemMosaic::DemMosaic(const std::string& dir, bool verbose) : verbose_(verbose) {
    GDALAllRegister();
    namespace fs = std::filesystem;
    if (!fs::is_directory(dir)) throw std::runtime_error("DEM directory not found: " + dir);

    std::vector<std::string> paths;
    for (const auto& entry : fs::directory_iterator(dir)) {
        const auto ext = entry.path().extension().string();
        if (ext == ".tif" || ext == ".tiff") paths.push_back(entry.path().string());
    }
    std::sort(paths.begin(), paths.end());

    for (const auto& path : paths) {
        auto* ds = static_cast<GDALDataset*>(GDALOpen(path.c_str(), GA_ReadOnly));
        if (!ds) {
            std::fprintf(stderr, "warning: skipping unreadable DEM %s\n", path.c_str());
            continue;
        }
        DemTile t;
        t.path = path;
        ds->GetGeoTransform(t.geoTransform.data());
        t.width = ds->GetRasterXSize();
        t.height = ds->GetRasterYSize();
        int ok = 0;
        t.noData = ds->GetRasterBand(1)->GetNoDataValue(&ok);
        t.hasNoData = ok != 0;
        const auto& g = t.geoTransform;
        t.minX = g[0];
        t.maxX = g[0] + g[1] * t.width;
        t.maxY = g[3];
        t.minY = g[3] + g[5] * t.height;
        if (wkt_.empty()) wkt_ = ds->GetProjectionRef();
        GDALClose(ds);
        tiles_.push_back(std::move(t));
    }
    if (tiles_.empty()) throw std::runtime_error("no DEM tiles found in " + dir);

    OGRSpatialReference wgs, native;
    wgs.importFromEPSG(4326);
    wgs.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);  // lon, lat
    native.importFromWkt(wkt_.c_str());
    native.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
    toNative_ = OGRCreateCoordinateTransformation(&wgs, &native);
    toWgs_ = OGRCreateCoordinateTransformation(&native, &wgs);
    if (!toNative_ || !toWgs_) throw std::runtime_error("cannot build DEM coordinate transform");

    for (auto& t : tiles_) {
        t.outline = toWgs84({{t.minX, t.maxY}, {t.maxX, t.maxY}, {t.maxX, t.minY},
                             {t.minX, t.minY}, {t.minX, t.maxY}});
    }
    if (verbose_) std::fprintf(stderr, "DEM: %zu tiles in %s\n", tiles_.size(), dir.c_str());
}

DemMosaic::~DemMosaic() {
    OGRCoordinateTransformation::DestroyCT(ct(toNative_));
    OGRCoordinateTransformation::DestroyCT(ct(toWgs_));
}

std::vector<XY> DemMosaic::toNative(const std::vector<LatLon>& pts) const {
    std::vector<double> x(pts.size()), y(pts.size());
    for (size_t i = 0; i < pts.size(); ++i) {
        x[i] = pts[i].lon;
        y[i] = pts[i].lat;
    }
    std::vector<XY> out(pts.size());
    if (!pts.empty() && !ct(toNative_)->Transform(pts.size(), x.data(), y.data()))
        throw std::runtime_error("coordinate transform failed");
    for (size_t i = 0; i < pts.size(); ++i) out[i] = {x[i], y[i]};
    return out;
}

std::vector<LatLon> DemMosaic::toWgs84(const std::vector<XY>& pts) const {
    std::vector<double> x(pts.size()), y(pts.size());
    for (size_t i = 0; i < pts.size(); ++i) {
        x[i] = pts[i].x;
        y[i] = pts[i].y;
    }
    if (!pts.empty() && !ct(toWgs_)->Transform(pts.size(), x.data(), y.data()))
        throw std::runtime_error("coordinate transform failed");
    std::vector<LatLon> out(pts.size());
    for (size_t i = 0; i < pts.size(); ++i) out[i] = {y[i], x[i]};
    return out;
}

std::vector<float> DemMosaic::sample(const std::vector<LatLon>& pts) const {
    std::vector<float> out(pts.size(), kNaN);
    const std::vector<XY> native = toNative(pts);

    // Bucket point indices by the tile that contains them.
    std::vector<std::vector<uint32_t>> buckets(tiles_.size());
    for (size_t i = 0; i < native.size(); ++i) {
        const XY p = native[i];
        for (size_t t = 0; t < tiles_.size(); ++t) {
            const auto& tile = tiles_[t];
            if (p.x >= tile.minX && p.x < tile.maxX && p.y > tile.minY && p.y <= tile.maxY) {
                buckets[t].push_back(static_cast<uint32_t>(i));
                break;
            }
        }
    }

    for (size_t t = 0; t < tiles_.size(); ++t) {
        if (buckets[t].empty()) continue;
        const auto& tile = tiles_[t];
        if (verbose_)
            std::fprintf(stderr, "  sampling %zu points from %s\n", buckets[t].size(),
                         std::filesystem::path(tile.path).filename().c_str());
        const TileRaster r = readTile(tile);
        const auto& g = tile.geoTransform;
        for (uint32_t idx : buckets[t]) {
            // Pixel-centre based bilinear interpolation.
            const double fx = (native[idx].x - g[0]) / g[1] - 0.5;
            const double fy = (native[idx].y - g[3]) / g[5] - 0.5;
            const int x0 = std::clamp(static_cast<int>(std::floor(fx)), 0, r.width - 2);
            const int y0 = std::clamp(static_cast<int>(std::floor(fy)), 0, r.height - 2);
            const double tx = std::clamp(fx - x0, 0.0, 1.0);
            const double ty = std::clamp(fy - y0, 0.0, 1.0);
            const size_t base = static_cast<size_t>(y0) * r.width + x0;
            const float v00 = r.data[base], v10 = r.data[base + 1];
            const float v01 = r.data[base + r.width], v11 = r.data[base + r.width + 1];
            if (std::isnan(v00) || std::isnan(v10) || std::isnan(v01) || std::isnan(v11)) {
                // Fall back to nearest valid neighbour at nodata edges.
                const int nx = std::clamp(static_cast<int>(std::lround(fx)), 0, r.width - 1);
                const int ny = std::clamp(static_cast<int>(std::lround(fy)), 0, r.height - 1);
                out[idx] = r.data[static_cast<size_t>(ny) * r.width + nx];
                continue;
            }
            out[idx] = static_cast<float>((v00 * (1 - tx) + v10 * tx) * (1 - ty) +
                                          (v01 * (1 - tx) + v11 * tx) * ty);
        }
    }
    return out;
}

DemGrid DemMosaic::downsample(double cellSize) const {
    DemGrid grid;
    double minX = tiles_[0].minX, maxX = tiles_[0].maxX, minY = tiles_[0].minY, maxY = tiles_[0].maxY;
    for (const auto& t : tiles_) {
        minX = std::min(minX, t.minX);
        maxX = std::max(maxX, t.maxX);
        minY = std::min(minY, t.minY);
        maxY = std::max(maxY, t.maxY);
    }
    grid.originX = minX;
    grid.originY = maxY;
    grid.cellSize = cellSize;
    grid.width = static_cast<int>(std::ceil((maxX - minX) / cellSize));
    grid.height = static_cast<int>(std::ceil((maxY - minY) / cellSize));
    grid.values.assign(static_cast<size_t>(grid.width) * grid.height, kNaN);

    for (const auto& t : tiles_) {
        auto* ds = static_cast<GDALDataset*>(GDALOpen(t.path.c_str(), GA_ReadOnly));
        if (!ds) continue;
        const int bw = static_cast<int>(std::lround((t.maxX - t.minX) / cellSize));
        const int bh = static_cast<int>(std::lround((t.maxY - t.minY) / cellSize));
        std::vector<float> buf(static_cast<size_t>(bw) * bh);
        GDALRasterIOExtraArg extra;
        INIT_RASTERIO_EXTRA_ARG(extra);
        extra.eResampleAlg = GRIORA_Average;
        if (verbose_)
            std::fprintf(stderr, "  downsampling %s\n", std::filesystem::path(t.path).filename().c_str());
        const CPLErr err = ds->GetRasterBand(1)->RasterIO(GF_Read, 0, 0, t.width, t.height, buf.data(), bw,
                                                         bh, GDT_Float32, 0, 0, &extra);
        GDALClose(ds);
        if (err != CE_None) continue;
        const int col0 = static_cast<int>(std::lround((t.minX - minX) / cellSize));
        const int row0 = static_cast<int>(std::lround((maxY - t.maxY) / cellSize));
        for (int r = 0; r < bh; ++r)
            for (int c = 0; c < bw; ++c) {
                const int gr = row0 + r, gc = col0 + c;
                if (gr < 0 || gc < 0 || gr >= grid.height || gc >= grid.width) continue;
                float v = buf[static_cast<size_t>(r) * bw + c];
                if ((t.hasNoData && v == static_cast<float>(t.noData)) || v < -1000.0f) v = kNaN;
                grid.values[static_cast<size_t>(gr) * grid.width + gc] = v;
            }
    }
    return grid;
}

}  // namespace bt
