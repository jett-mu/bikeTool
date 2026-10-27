#include "biketool/elevation/hillshade.hpp"

#include <gdal_priv.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <stdexcept>

#include <nlohmann/json.hpp>

namespace bt {
namespace {

using Rgba = std::array<uint8_t, 4>;

// Horn's method; returns illumination in [0,1] or NaN where neighbours are missing.
std::vector<float> computeShade(const DemGrid& g, const OverlayOptions& opt) {
    std::vector<float> shade(g.values.size(), std::numeric_limits<float>::quiet_NaN());
    const double zen = deg2rad(90.0 - opt.altitude);
    const double az = deg2rad(360.0 - opt.azimuth + 90.0);
    const double cs = g.cellSize;
    for (int r = 1; r < g.height - 1; ++r)
        for (int c = 1; c < g.width - 1; ++c) {
            const float a = g.at(c - 1, r - 1), b = g.at(c, r - 1), cc = g.at(c + 1, r - 1);
            const float d = g.at(c - 1, r), f = g.at(c + 1, r);
            const float gg = g.at(c - 1, r + 1), h = g.at(c, r + 1), i = g.at(c + 1, r + 1);
            if (std::isnan(a) || std::isnan(b) || std::isnan(cc) || std::isnan(d) || std::isnan(f) ||
                std::isnan(gg) || std::isnan(h) || std::isnan(i))
                continue;
            const double dzdx = ((cc + 2 * f + i) - (a + 2 * d + gg)) / (8 * cs) * opt.zFactor;
            const double dzdy = ((gg + 2 * h + i) - (a + 2 * b + cc)) / (8 * cs) * opt.zFactor;
            const double slope = std::atan(std::sqrt(dzdx * dzdx + dzdy * dzdy));
            double aspect = 0.0;
            if (dzdx != 0.0) {
                aspect = std::atan2(dzdy, -dzdx);
                if (aspect < 0) aspect += 2 * kPi;
            } else {
                aspect = dzdy > 0 ? kPi / 2 : (dzdy < 0 ? 3 * kPi / 2 : 0.0);
            }
            const double v = std::cos(zen) * std::cos(slope) +
                             std::sin(zen) * std::sin(slope) * std::cos(az - aspect);
            shade[static_cast<size_t>(r) * g.width + c] = static_cast<float>(std::clamp(v, 0.0, 1.0));
        }
    return shade;
}

Rgba tint(float t) {
    // Low -> high: deep green, olive, sand, rust, near-white.
    static const std::array<std::array<float, 4>, 5> stops = {{
        {0.00f, 46, 125, 80}, {0.30f, 132, 168, 84}, {0.55f, 222, 199, 120},
        {0.80f, 186, 110, 64}, {1.00f, 245, 240, 232}}};
    t = std::clamp(t, 0.0f, 1.0f);
    for (size_t k = 1; k < stops.size(); ++k) {
        if (t <= stops[k][0]) {
            const float u = (t - stops[k - 1][0]) / (stops[k][0] - stops[k - 1][0]);
            Rgba out{};
            for (int ch = 0; ch < 3; ++ch)
                out[ch] = static_cast<uint8_t>(stops[k - 1][ch + 1] + u * (stops[k][ch + 1] - stops[k - 1][ch + 1]));
            out[3] = 255;
            return out;
        }
    }
    return {245, 240, 232, 255};
}

void writePng(const std::string& path, int w, int h, const std::vector<Rgba>& px) {
    GDALDriver* mem = GetGDALDriverManager()->GetDriverByName("MEM");
    GDALDriver* png = GetGDALDriverManager()->GetDriverByName("PNG");
    if (!mem || !png) throw std::runtime_error("GDAL MEM/PNG drivers unavailable");
    GDALDataset* ds = mem->Create("", w, h, 4, GDT_Byte, nullptr);
    std::vector<uint8_t> band(static_cast<size_t>(w) * h);
    for (int b = 0; b < 4; ++b) {
        for (size_t i = 0; i < band.size(); ++i) band[i] = px[i][b];
        if (ds->GetRasterBand(b + 1)->RasterIO(GF_Write, 0, 0, w, h, band.data(), w, h, GDT_Byte, 0, 0) != CE_None)
            throw std::runtime_error("raster write failed");
    }
    GDALDataset* out = png->CreateCopy(path.c_str(), ds, FALSE, nullptr, nullptr, nullptr);
    GDALClose(ds);
    if (!out) throw std::runtime_error("cannot write " + path);
    GDALClose(out);
    std::filesystem::remove(path + ".aux.xml");
}

}  // namespace

OverlayResult writeTerrainOverlays(const DemMosaic& dem, const std::string& outDir, const OverlayOptions& opt) {
    std::filesystem::create_directories(outDir);
    const DemGrid grid = dem.downsample(opt.cellSize);
    const std::vector<float> shade = computeShade(grid, opt);

    OverlayResult res;
    res.minElev = std::numeric_limits<float>::max();
    res.maxElev = std::numeric_limits<float>::lowest();
    for (float v : grid.values)
        if (!std::isnan(v)) {
            res.minElev = std::min(res.minElev, v);
            res.maxElev = std::max(res.maxElev, v);
        }
    for (const auto& t : dem.tiles())
        for (const auto& p : t.outline) res.bounds.extend(p);

    // Output grid in lat/lon with roughly the source resolution.
    const double midLat = (res.bounds.minLat + res.bounds.maxLat) / 2;
    const double mPerDegLat = deg2rad(1.0) * kEarthRadiusM;
    const double mPerDegLon = mPerDegLat * std::cos(deg2rad(midLat));
    res.width = static_cast<int>(std::ceil((res.bounds.maxLon - res.bounds.minLon) * mPerDegLon / opt.cellSize));
    res.height = static_cast<int>(std::ceil((res.bounds.maxLat - res.bounds.minLat) * mPerDegLat / opt.cellSize));

    std::vector<LatLon> centres;
    centres.reserve(static_cast<size_t>(res.width) * res.height);
    for (int r = 0; r < res.height; ++r)
        for (int c = 0; c < res.width; ++c)
            centres.push_back({res.bounds.maxLat - (r + 0.5) / res.height * (res.bounds.maxLat - res.bounds.minLat),
                               res.bounds.minLon + (c + 0.5) / res.width * (res.bounds.maxLon - res.bounds.minLon)});
    const std::vector<XY> native = dem.toNative(centres);

    std::vector<Rgba> hill(centres.size(), Rgba{0, 0, 0, 0});
    std::vector<Rgba> elev(centres.size(), Rgba{0, 0, 0, 0});
    const float flat = static_cast<float>(std::cos(deg2rad(90.0 - opt.altitude)));
    const float range = std::max(1.0f, res.maxElev - res.minElev);
    for (size_t i = 0; i < native.size(); ++i) {
        const int c = static_cast<int>(std::floor((native[i].x - grid.originX) / grid.cellSize));
        const int r = static_cast<int>(std::floor((grid.originY - native[i].y) / grid.cellSize));
        if (c < 0 || r < 0 || c >= grid.width || r >= grid.height) continue;
        const size_t k = static_cast<size_t>(r) * grid.width + c;
        const float s = shade[k];
        if (!std::isnan(s)) {
            if (s < flat) {
                hill[i] = {20, 24, 32, static_cast<uint8_t>(std::min(1.0f, (flat - s) / flat * 1.6f) * 210)};
            } else {
                hill[i] = {255, 252, 240, static_cast<uint8_t>(std::min(1.0f, (s - flat) / (1 - flat) * 1.6f) * 130)};
            }
        }
        const float z = grid.values[k];
        if (!std::isnan(z)) {
            Rgba col = tint((z - res.minElev) / range);
            if (!std::isnan(s)) {  // modulate by shade for relief
                const float m = 0.65f + 0.35f * (s / std::max(flat, 0.01f));
                for (int ch = 0; ch < 3; ++ch) col[ch] = static_cast<uint8_t>(std::clamp(col[ch] * m, 0.0f, 255.0f));
            }
            col[3] = 255;
            elev[i] = col;
        }
    }

    writePng(outDir + "/hillshade.png", res.width, res.height, hill);
    writePng(outDir + "/elevation.png", res.width, res.height, elev);

    nlohmann::json meta = {
        {"bounds", {{res.bounds.minLat, res.bounds.minLon}, {res.bounds.maxLat, res.bounds.maxLon}}},
        {"min_elev", res.minElev},
        {"max_elev", res.maxElev},
        {"width", res.width},
        {"height", res.height},
        {"cell_size_m", opt.cellSize}};
    std::ofstream(outDir + "/overlays.json") << meta.dump(2);
    return res;
}

}  // namespace bt
