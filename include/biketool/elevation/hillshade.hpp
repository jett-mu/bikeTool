#pragma once
// Raster overlays for the web map: a transparent hillshade and a hypsometric
// elevation tint, both resampled onto a regular lat/lon grid so they can be
// placed with a plain Leaflet imageOverlay.

#include <string>

#include "biketool/elevation/dem.hpp"

namespace bt {

struct OverlayOptions {
    double cellSize = 10.0;   // metres, source resolution after downsampling
    double azimuth = 315.0;   // sun direction, degrees
    double altitude = 45.0;   // sun elevation, degrees
    double zFactor = 2.0;     // vertical exaggeration (terrain here is gentle)
};

struct OverlayResult {
    BBox bounds;
    float minElev = 0, maxElev = 0;
    int width = 0, height = 0;
};

// Writes <outDir>/hillshade.png and <outDir>/elevation.png plus
// <outDir>/overlays.json describing their bounds and elevation range.
OverlayResult writeTerrainOverlays(const DemMosaic& dem, const std::string& outDir,
                                   const OverlayOptions& opt = {});

}  // namespace bt
