# York Ride Planner

A bike route planner for York Region. It combines four inputs:
- the OpenStreetMap road and path network
- the official York Region bike facility layer
- 1 m elevation data
- live wind

For each start/end pair it offers up to six routes, and estimates the time and energy each one takes for your rider profile.

```
web/ (Leaflet UI)  ──HTTP──▶  server/app.py (Flask)  ──argv──▶  bin/biketool (C++)
                                   │ Open-Meteo wind                │ data/build/graph.bin
                                   │ Nominatim search               │ (roads + lanes + elevation)
```

## Quick start

```sh
make            # build bin/biketool (needs GDAL: brew install gdal)
make selftest   # physics + routing sanity checks
make all-data   # download data, build the graph, export map layers
make serve      # http://127.0.0.1:5050
```

`make all-data` runs three steps, which you can also run separately:

| Step | What it does |
|---|---|
| `make data` | Downloads `york_osm.json`, `york_bike_facilities.geojson` and `york_boundary.geojson` into `data/raw/`. Roads are fetched from Overpass in 16 bbox chunks, which are cached in `data/raw/osm_chunks/`, then clipped to the York Region boundary. |
| `make graph` | `biketool build`: splits ways at junctions, applies cycling access and oneway rules, matches York facilities onto edges, and samples elevation. Writes `data/build/graph.bin`. |
| `make layers` | `biketool layers` + `biketool hillshade`: writes the GeoJSON layers and terrain PNGs to `web/static/data/`. |

## Layout

```
include/biketool/        headers, one folder per concern
  geo/        coordinates, local projection, grid spatial index
  io/         Overpass + York GeoJSON loaders, binary graph I/O, JSON output
  elevation/  DTM mosaic sampling, hillshade / elevation overlays
  graph/      graph model, graph builder
  physics/    rider power model, wind
  routing/    cost profiles, A* router
  cli/        argv parsing
src/                     matching .cpp files + main.cpp
third_party/nlohmann/    json.hpp (vendored)
scripts/fetch_data.py    data download (stdlib only)
server/app.py            Flask API + static hosting
web/                     index.html, static/app.js, static/style.css
data/elevation/          your DTM GeoTIFFs
```

## CLI

```sh
bin/biketool route --graph data/build/graph.bin \
  --from 43.8561,-79.3370 --to 43.8710,-79.4370 \
  --wind-speed 5 --wind-dir 270 --mass 85 --power 150 --profiles all
```

The command prints JSON with one entry per distinct route. Each entry includes:
- distance, time, climb and descent
- energy (kJ and kcal) and average headwind
- share of the route on each facility type and share with elevation data
- the line geometry, a per-point grade array, an elevation profile and a list of named steps

## How routes are scored

| Profile | Minimises |
|---|---|
| Shortest | distance |
| Fastest | riding time at your cruising power |
| Bike-friendly | distance weighted by traffic stress (trails and cycle tracks are cheapest, arterials without facilities the most expensive) |
| Flattest | distance + 30 × metres climbed, with a penalty above 4% grade |
| Least energy | mechanical work at the pedals |
| Balanced | a blend of time, comfort and effort |

**Physics model.** The rider holds the cruising speed that their power sustains on flat ground. On climbs and into headwinds they work harder, up to their maximum power, and slow down beyond that. Downhill they coast. Energy counts the following, using the edge's surface:
- gravity
- rolling resistance
- aerodynamic drag against the relative air speed

Wind is taken at 10 m and scaled by 0.6 to rider height.

**Elevation coverage.** The elevation tiles cover SE Vaughan, Richmond Hill and Markham. Elsewhere grades count as 0, and each route reports how much of its length had elevation data.
