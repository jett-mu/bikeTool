#!/usr/bin/env python3
"""Flask front door for the York Region bike tool.

Serves the web UI and wraps the C++ `biketool` executable:
  GET /api/route    -> biketool route ...   (wind resolved live or manually)
  GET /api/wind     -> current wind from Open-Meteo (cached)
  GET /api/geocode  -> Nominatim search limited to York Region (throttled)
  GET /api/meta     -> map bounds, overlay metadata, available layers
"""
import json
import math
import os
import subprocess
import threading
import time
import urllib.parse
import urllib.request
from pathlib import Path

from flask import Flask, jsonify, request, send_from_directory

ROOT = Path(__file__).resolve().parent.parent
WEB_DIR = ROOT / "web"
DATA_DIR = WEB_DIR / "static" / "data"
BIN = Path(os.environ.get("BIKETOOL_BIN", ROOT / "bin" / "biketool"))
GRAPH = Path(os.environ.get("BIKETOOL_GRAPH", ROOT / "data" / "build" / "graph.bin"))

USER_AGENT = "biketool/0.1 (York Region bike route planner)"
YORK_VIEWBOX = "-79.78,44.51,-79.15,43.74"  # lon/lat: west,north,east,south
PROFILES = ["shortest", "fastest", "bike_friendly", "flattest", "least_energy", "balanced"]
WIND_TTL_S = 600
ROUTE_TIMEOUT_S = 45

app = Flask(__name__, static_folder=str(WEB_DIR / "static"), static_url_path="/static")


class ApiError(Exception):
    def __init__(self, message, status=400):
        super().__init__(message)
        self.status = status


@app.errorhandler(ApiError)
def handle_api_error(err):
    return jsonify({"ok": False, "error": str(err)}), err.status


def http_json(url, timeout=10):
    req = urllib.request.Request(url, headers={"User-Agent": USER_AGENT})
    with urllib.request.urlopen(req, timeout=timeout) as resp:
        return json.loads(resp.read())


def arg_float(name, default, lo, hi):
    raw = request.args.get(name)
    if raw in (None, ""):
        return default
    try:
        value = float(raw)
    except ValueError:
        raise ApiError(f"'{name}' must be a number")
    if not math.isfinite(value):
        raise ApiError(f"'{name}' must be finite")
    return min(hi, max(lo, value))


def arg_latlon(name):
    raw = request.args.get(name, "")
    try:
        lat, lon = (float(v) for v in raw.split(","))
    except ValueError:
        raise ApiError(f"'{name}' must be 'lat,lon'")
    if not (-90 <= lat <= 90 and -180 <= lon <= 180):
        raise ApiError(f"'{name}' is out of range")
    return lat, lon


# ---- wind -------------------------------------------------------------------

_wind_cache = {}
_wind_lock = threading.Lock()


def live_wind(lat, lon):
    key = (round(lat, 1), round(lon, 1))
    with _wind_lock:
        hit = _wind_cache.get(key)
        if hit and time.time() - hit[0] < WIND_TTL_S:
            return hit[1]
    params = urllib.parse.urlencode({
        "latitude": f"{lat:.4f}", "longitude": f"{lon:.4f}",
        "current": "wind_speed_10m,wind_direction_10m,wind_gusts_10m,temperature_2m",
        "wind_speed_unit": "ms", "timezone": "America/Toronto",
    })
    doc = http_json(f"https://api.open-meteo.com/v1/forecast?{params}")
    cur = doc["current"]
    wind = {
        "source": "live",
        "speed_ms": float(cur["wind_speed_10m"]),
        "from_deg": float(cur["wind_direction_10m"]),
        "gusts_ms": float(cur.get("wind_gusts_10m") or 0.0),
        "temperature_c": cur.get("temperature_2m"),
        "observed_at": cur.get("time"),
        "fetched_at": int(time.time()),
    }
    with _wind_lock:
        _wind_cache[key] = (time.time(), wind)
    return wind


@app.get("/api/wind")
def api_wind():
    lat = arg_float("lat", 43.87, -90, 90)
    lon = arg_float("lon", -79.40, -180, 180)
    try:
        return jsonify({"ok": True, **live_wind(lat, lon)})
    except Exception as exc:  # noqa: BLE001 - upstream errors are varied
        raise ApiError(f"wind service unavailable: {exc}", 502)


# ---- routing ------------------------------------------------------------------

@app.get("/api/route")
def api_route():
    if not BIN.exists():
        raise ApiError("backend not built; run `make`", 503)
    if not GRAPH.exists():
        raise ApiError("routing graph missing; run `make all-data`", 503)

    start, end = arg_latlon("from"), arg_latlon("to")
    profiles = request.args.get("profiles", "all")
    if profiles != "all" and any(p not in PROFILES for p in profiles.split(",")):
        raise ApiError("unknown profile")

    mode = request.args.get("wind", "live")
    warnings = []
    if mode == "manual":
        wind = {"source": "manual",
                "speed_ms": arg_float("wind_speed", 0.0, 0.0, 40.0),
                "from_deg": arg_float("wind_dir", 0.0, 0.0, 360.0)}
    elif mode == "none":
        wind = {"source": "none", "speed_ms": 0.0, "from_deg": 0.0}
    else:
        mid = ((start[0] + end[0]) / 2, (start[1] + end[1]) / 2)
        try:
            wind = live_wind(*mid)
        except Exception as exc:  # noqa: BLE001
            warnings.append(f"Live wind unavailable ({exc}); assuming calm.")
            wind = {"source": "none", "speed_ms": 0.0, "from_deg": 0.0}

    argv = [
        str(BIN), "route", "--graph", str(GRAPH),
        "--from", f"{start[0]:.6f},{start[1]:.6f}", "--to", f"{end[0]:.6f},{end[1]:.6f}",
        "--profiles", profiles,
        "--wind-speed", f"{wind['speed_ms']:.2f}", "--wind-dir", f"{wind['from_deg']:.1f}",
        "--mass", f"{arg_float('mass', 85, 30, 250):.1f}",
        "--power", f"{arg_float('power', 150, 30, 600):.0f}",
        "--cda", f"{arg_float('cda', 0.45, 0.15, 1.2):.3f}",
        "--crr", f"{arg_float('crr', 0.006, 0.002, 0.05):.4f}",
    ]
    if request.args.get("max_power"):
        argv += ["--max-power", f"{arg_float('max_power', 250, 30, 1200):.0f}"]

    try:
        proc = subprocess.run(argv, capture_output=True, text=True, timeout=ROUTE_TIMEOUT_S)
    except subprocess.TimeoutExpired:
        raise ApiError("routing timed out", 504)
    if proc.returncode != 0:
        try:
            message = json.loads(proc.stderr.strip().splitlines()[-1])["error"]
        except (ValueError, KeyError, IndexError):
            message = proc.stderr.strip() or "routing failed"
        raise ApiError(message, 500)

    result = json.loads(proc.stdout)
    result["wind"] = {**result.get("wind", {}), **wind}
    for key in ("start", "end"):
        if result[key]["distance_m"] > 250:
            warnings.append(f"The {key} point is {result[key]['distance_m']:.0f} m from the nearest road.")
    if not result["routes"]:
        warnings.append("No route found between these points.")
    result["warnings"] = warnings
    return jsonify(result)


# ---- geocoding ----------------------------------------------------------------

_geo_lock = threading.Lock()
_geo_last = [0.0]
_geo_cache = {}


@app.get("/api/geocode")
def api_geocode():
    q = request.args.get("q", "").strip()
    if len(q) < 3:
        return jsonify({"ok": True, "results": []})
    if q in _geo_cache:
        return jsonify({"ok": True, "results": _geo_cache[q]})
    params = urllib.parse.urlencode({
        "q": q, "format": "jsonv2", "limit": 6, "countrycodes": "ca",
        "viewbox": YORK_VIEWBOX, "bounded": 1, "addressdetails": 1,
    })
    rows = nominatim(f"search?{params}")
    results = [{"name": r.get("display_name", ""), "label": short_label(r), "lat": float(r["lat"]),
                "lon": float(r["lon"])} for r in rows]
    _geo_cache[q] = results
    return jsonify({"ok": True, "results": results})


@app.get("/api/reverse")
def api_reverse():
    lat = arg_float("lat", 0.0, -90, 90)
    lon = arg_float("lon", 0.0, -180, 180)
    key = (round(lat, 4), round(lon, 4))
    if key not in _geo_cache:
        params = urllib.parse.urlencode({"lat": f"{lat:.6f}", "lon": f"{lon:.6f}", "format": "jsonv2", "zoom": 17})
        row = nominatim(f"reverse?{params}")
        _geo_cache[key] = {"label": short_label(row, prefer_street=True) if isinstance(row, dict) else "",
                           "name": row.get("display_name", "") if isinstance(row, dict) else ""}
    return jsonify({"ok": True, **_geo_cache[key]})


def nominatim(path):
    with _geo_lock:  # Nominatim usage policy: at most 1 request per second
        wait = 1.0 - (time.time() - _geo_last[0])
        if wait > 0:
            time.sleep(wait)
        _geo_last[0] = time.time()
        try:
            return http_json(f"https://nominatim.openstreetmap.org/{path}")
        except Exception as exc:  # noqa: BLE001
            raise ApiError(f"geocoder unavailable: {exc}", 502)


def short_label(row, prefer_street=False):
    """'123 Main Street, Markham' rather than the full display name. Dropped
    pins prefer the street; searches prefer the place the user typed."""
    addr = row.get("address", {}) if isinstance(row, dict) else {}
    street = " ".join(p for p in (addr.get("house_number"), addr.get("road")) if p)
    place = row.get("name") or ""
    town = addr.get("city") or addr.get("town") or addr.get("village") or addr.get("suburb") or ""
    area = addr.get("neighbourhood") or addr.get("hamlet") or addr.get("quarter") or addr.get("suburb") or ""
    named = place if place and place != addr.get("road") else ""
    first = (street or named or area) if prefer_street else (named or street or area)
    parts = [p for p in (first, town) if p]
    return ", ".join(dict.fromkeys(parts)) or row.get("display_name", "").split(",")[0]


# ---- metadata & pages -----------------------------------------------------------

def read_json(path):
    try:
        return json.loads(path.read_text())
    except (OSError, ValueError):
        return None


@app.get("/api/meta")
def api_meta():
    layers = {name: (DATA_DIR / f"{name}.geojson").exists()
              for name in ("bike_network", "steep_segments", "dem_coverage", "york_boundary")}
    return jsonify({
        "ok": True,
        "graph_ready": GRAPH.exists() and BIN.exists(),
        "network": read_json(DATA_DIR / "network_meta.json"),
        "overlays": read_json(DATA_DIR / "overlays.json"),
        "layers": layers,
        "profiles": PROFILES,
    })


@app.get("/")
def index():
    return send_from_directory(WEB_DIR, "index.html")


if __name__ == "__main__":
    port = int(os.environ.get("PORT", "5050"))  # 5000 is taken by AirPlay on macOS
    print(f"Bike tool running at http://127.0.0.1:{port}")
    app.run(host="127.0.0.1", port=port, debug=os.environ.get("FLASK_DEBUG") == "1")
