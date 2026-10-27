#!/usr/bin/env python3
"""Download raw York Region datasets into data/raw/.

  - OSM bikeable road network (Overpass API)            -> york_osm.json
  - York Region official bike facilities (ArcGIS REST)  -> york_bike_facilities.geojson
  - York Region boundary (Overpass, outer ways)          -> york_boundary.geojson

Standard library only. Existing files are kept unless --force is given.
"""
import argparse
import json
import sys
import time
import urllib.parse
import urllib.request
from pathlib import Path

USER_AGENT = "biketool/0.1 (York Region bike route planner)"
OVERPASS_URLS = [
    "https://overpass-api.de/api/interpreter",
    "https://overpass.private.coffee/api/interpreter",
    "https://maps.mail.ru/osm/tools/overpass/api/interpreter",
]
YORK_RELATION_ID = 324214                    # OSM relation "York Region"
BOUNDARY_MARGIN_DEG = 0.004                  # keep roads within ~400 m of the border
BIKE_LAYER_URL = ("https://ww8.yorkmaps.ca/arcgis/rest/services/OpenData/"
                  "OpenDataRepository/MapServer/105/query")
BIKE_PAGE_SIZE = 2000
# York Region bounding box (from the boundary relation), split into a grid so
# each Overpass request stays well under server time limits.
YORK_BBOX = (43.74, -79.78, 44.51, -79.15)  # south, west, north, east
GRID_ROWS, GRID_COLS = 4, 4

# Area filters are too slow on the public servers, so chunks are plain bbox
# queries and the result is clipped to the boundary polygon locally.
ROAD_QUERY = """
[out:json][timeout:300];
(
  way["highway"~"^(trunk|trunk_link|primary|primary_link|secondary|secondary_link|tertiary|tertiary_link|unclassified|residential|living_street|service|cycleway|path|track|pedestrian|bridleway)$"]
     ["access"!~"^(private|no)$"]["bicycle"!="no"]["area"!="yes"]
     ["service"!~"^(driveway|parking_aisle|drive-through|emergency_access)$"]({bbox});
  way["highway"="footway"]["bicycle"~"^(yes|designated|permissive)$"]({bbox});
);
out body;
>;
out skel qt;
"""

BOUNDARY_QUERY = f"""
[out:json][timeout:180];
rel({YORK_RELATION_ID});
out geom;
"""


def http(url, data=None, retries=4, timeout=900):
    body = urllib.parse.urlencode(data).encode() if data else None
    for attempt in range(retries):
        try:
            req = urllib.request.Request(url, data=body, headers={"User-Agent": USER_AGENT})
            with urllib.request.urlopen(req, timeout=timeout) as resp:
                return resp.read()
        except Exception as exc:  # noqa: BLE001 - network errors are varied
            wait = 10 * (attempt + 1)
            print(f"  request failed ({exc}); retrying in {wait}s", file=sys.stderr)
            time.sleep(wait)
    raise RuntimeError(f"giving up on {url}")


def overpass(query, timeout=400):
    last = None
    for url in OVERPASS_URLS:
        try:
            return http(url, {"data": query}, retries=2, timeout=timeout)
        except RuntimeError as exc:
            last = exc
            print(f"  {url} unavailable, trying next mirror", file=sys.stderr)
    raise RuntimeError(f"all Overpass mirrors failed: {last}")


def boundary_rings(path: Path):
    """Chain the boundary's outer ways into closed rings of (lon, lat)."""
    lines = [[tuple(pt) for pt in line] for feat in json.loads(path.read_text())["features"]
             for line in feat["geometry"]["coordinates"]]
    rings = []
    while lines:
        ring = lines.pop()
        grown = True
        while ring[0] != ring[-1] and grown:
            grown = False
            for i, line in enumerate(lines):
                if line[0] == ring[-1]:
                    ring = ring + line[1:]
                elif line[-1] == ring[-1]:
                    ring = ring + line[::-1][1:]
                elif line[-1] == ring[0]:
                    ring = line[:-1] + ring
                elif line[0] == ring[0]:
                    ring = line[::-1][:-1] + ring
                else:
                    continue
                lines.pop(i)
                grown = True
                break
        rings.append(ring)
    return rings


class BoundaryClip:
    """Point-in-polygon (even-odd over all rings) plus a near-border margin so
    boundary roads such as Steeles Avenue are kept whole."""

    def __init__(self, rings, margin):
        self.rings = rings
        self.margin = margin
        self.near = set()
        for ring in rings:
            for (x0, y0), (x1, y1) in zip(ring, ring[1:]):
                steps = max(1, int(max(abs(x1 - x0), abs(y1 - y0)) / (margin / 2)))
                for k in range(steps + 1):
                    x, y = x0 + (x1 - x0) * k / steps, y0 + (y1 - y0) * k / steps
                    self.near.add((int(x // margin), int(y // margin)))

    def keeps(self, lon, lat):
        cx, cy = int(lon // self.margin), int(lat // self.margin)
        if any((cx + dx, cy + dy) in self.near for dx in (-1, 0, 1) for dy in (-1, 0, 1)):
            return True
        inside = False
        for ring in self.rings:
            for (x0, y0), (x1, y1) in zip(ring, ring[1:]):
                if (y0 > lat) != (y1 > lat) and lon < x0 + (lat - y0) * (x1 - x0) / (y1 - y0):
                    inside = not inside
        return inside


def clip_to_boundary(elements, boundary_path: Path):
    clip = BoundaryClip(boundary_rings(boundary_path), BOUNDARY_MARGIN_DEG)
    nodes = {el["id"]: el for el in elements if el["type"] == "node"}
    verdict = {}

    def node_ok(nid):
        if nid not in verdict:
            n = nodes.get(nid)
            verdict[nid] = bool(n) and clip.keeps(n["lon"], n["lat"])
        return verdict[nid]

    ways = [el for el in elements if el["type"] == "way"]
    # Test a few nodes per way first; most ways are entirely inside or outside.
    kept = [w for w in ways if any(node_ok(n) for n in w["nodes"][::max(1, len(w["nodes"]) // 4)])
            or any(node_ok(n) for n in w["nodes"])]
    used = {n for w in kept for n in w["nodes"]}
    return kept + [nodes[n] for n in used if n in nodes]


def fetch_roads(out: Path):
    print("Fetching OSM road network for York Region in "
          f"{GRID_ROWS * GRID_COLS} chunks...", flush=True)
    chunk_dir = out.parent / "osm_chunks"
    chunk_dir.mkdir(exist_ok=True)
    s, w, n, e = YORK_BBOX
    dlat, dlon = (n - s) / GRID_ROWS, (e - w) / GRID_COLS
    elements = {}
    for r in range(GRID_ROWS):
        for c in range(GRID_COLS):
            chunk = chunk_dir / f"chunk_{r}_{c}.json"
            if not chunk.exists():
                bbox = f"{s + r * dlat:.5f},{w + c * dlon:.5f},{s + (r + 1) * dlat:.5f},{w + (c + 1) * dlon:.5f}"
                raw = overpass(ROAD_QUERY.format(bbox=bbox))
                doc = json.loads(raw)
                if "remark" in doc and not doc.get("elements"):
                    raise RuntimeError("Overpass error: " + doc["remark"])
                chunk.write_bytes(raw)
                time.sleep(2)  # be polite to the public servers
            doc = json.loads(chunk.read_bytes())
            for el in doc["elements"]:
                elements[(el["type"], el["id"])] = el
            ways = sum(1 for el in doc["elements"] if el["type"] == "way")
            print(f"  chunk {r},{c}: {ways} ways", flush=True)
    merged = list(elements.values())
    boundary = out.parent / "york_boundary.geojson"
    if boundary.exists():
        before = sum(1 for el in merged if el["type"] == "way")
        merged = clip_to_boundary(merged, boundary)
        print(f"  clipped to York Region boundary: {before} -> "
              f"{sum(1 for el in merged if el['type'] == 'way')} ways", flush=True)
    ways = sum(1 for el in merged if el["type"] == "way")
    if ways == 0:
        raise RuntimeError("Overpass returned no ways")
    out.write_text(json.dumps({"elements": merged}))
    print(f"  {ways} ways, {len(merged) - ways} nodes -> {out}", flush=True)


def fetch_bike_layer(out: Path):
    print("Fetching York Region bike facility layer...")
    features, offset = [], 0
    while True:
        params = {
            "where": "1=1", "outFields": "*", "outSR": "4326", "f": "geojson",
            "resultOffset": str(offset), "resultRecordCount": str(BIKE_PAGE_SIZE),
        }
        page = json.loads(http(BIKE_LAYER_URL + "?" + urllib.parse.urlencode(params), timeout=120))
        batch = page.get("features", [])
        features.extend(batch)
        print(f"  page @{offset}: {len(batch)} features")
        if len(batch) < BIKE_PAGE_SIZE and not page.get("properties", {}).get("exceededTransferLimit"):
            break
        offset += len(batch)
        if not batch:
            break
    out.write_text(json.dumps({"type": "FeatureCollection", "features": features}))
    print(f"  {len(features)} features -> {out}")


def fetch_boundary(out: Path):
    print("Fetching York Region boundary...")
    doc = json.loads(overpass(BOUNDARY_QUERY, timeout=300))
    lines = []
    for rel in doc["elements"]:
        for m in rel.get("members", []):
            if m.get("type") == "way" and m.get("role") in ("outer", "") and "geometry" in m:
                lines.append([[p["lon"], p["lat"]] for p in m["geometry"]])
    fc = {"type": "FeatureCollection", "features": [{
        "type": "Feature", "properties": {"name": "York Region"},
        "geometry": {"type": "MultiLineString", "coordinates": lines}}]}
    out.write_text(json.dumps(fc))
    print(f"  {len(lines)} boundary ways -> {out}")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--out-dir", default="data/raw")
    ap.add_argument("--force", action="store_true", help="re-download existing files")
    args = ap.parse_args()

    out_dir = Path(args.out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)
    jobs = [
        ("york_bike_facilities.geojson", fetch_bike_layer),
        ("york_boundary.geojson", fetch_boundary),
        ("york_osm.json", fetch_roads),
    ]
    for name, fn in jobs:
        path = out_dir / name
        if path.exists() and not args.force:
            print(f"{path} exists, skipping (use --force to refresh)")
            continue
        fn(path)


if __name__ == "__main__":
    main()
