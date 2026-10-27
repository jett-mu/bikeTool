/* York Ride Planner frontend. Talks to the Flask API, draws layers and routes. */
(() => {
  "use strict";

  // ---- constants -------------------------------------------------------------

  const PROFILES = {
    balanced:      { label: "Balanced",      color: "#7b4fc4", blurb: "Time, comfort and effort together" },
    bike_friendly: { label: "Bike-friendly", color: "#16936a", blurb: "Prefers trails and bike lanes" },
    fastest:       { label: "Fastest",       color: "#d0452f", blurb: "Least time at your power" },
    least_energy:  { label: "Least energy",  color: "#d08f00", blurb: "Lowest effort, counting hills and wind" },
    flattest:      { label: "Flattest",      color: "#2f6fc0", blurb: "Avoids climbing" },
    shortest:      { label: "Shortest",      color: "#56656d", blurb: "Fewest metres" },
  };
  const PROFILE_ORDER = Object.keys(PROFILES);
  const RECOMMENDED = "balanced";

  const FACILITIES = {
    trail:          { label: "Off-road trail",  color: "#2e7d4f" },
    cycle_track:    { label: "Cycle track",     color: "#0a8c86" },
    buffered_lane:  { label: "Buffered lane",   color: "#2556b8" },
    bike_lane:      { label: "Bike lane",       color: "#4a8fe0" },
    paved_shoulder: { label: "Paved shoulder",  color: "#9a77d0" },
    sharrow:        { label: "Signed route",    color: "#c99a1e" },
    none:           { label: "Shared road",     color: "#a7b1ad" },
  };

  const GRADE_STOPS = [
    { max: -4, color: "#4b8fd1", label: "Downhill" },
    { max: 2,  color: "#3a9b5c", label: "Under 2%" },
    { max: 4,  color: "#cfae22", label: "2 to 4%" },
    { max: 6,  color: "#e07b28", label: "4 to 6%" },
    { max: 8,  color: "#c8432f", label: "6 to 8%" },
    { max: Infinity, color: "#7d1d3f", label: "Over 8%" },
  ];

  const PRESETS = {
    commuter: { label: "Commuter",  mass: 85,  power: 150, cda: "0.45" },
    road:     { label: "Road bike", mass: 78,  power: 200, cda: "0.32" },
    cargo:    { label: "Cargo",     mass: 130, power: 140, cda: "0.55" },
    ebike:    { label: "E-assist",  mass: 100, power: 300, cda: "0.55" },
  };

  const COMPASS = ["N", "NNE", "NE", "ENE", "E", "ESE", "SE", "SSE", "S", "SSW", "SW", "WSW", "W", "WNW", "NW", "NNW"];
  const YORK_CENTER = [43.93, -79.45];
  const touch = window.matchMedia("(pointer: coarse)").matches;
  const prefersDark = window.matchMedia("(prefers-color-scheme: dark)");

  // ---- state -----------------------------------------------------------------

  const state = {
    start: null, end: null,
    routes: [], selected: null, hovered: null, colorMode: "profile",
    rider: { mass: 85, power: 150, cda: "0.45" },
    wind: { mode: "live", speed: 4, dir: 270 },
    live: null, meta: null, lastResult: null,
    picking: null, loading: false,
    drawerOpen: !window.matchMedia("(max-width: 820px)").matches,  // phones start with the profile folded
  };

  const $ = (id) => document.getElementById(id);

  // ---- utilities -------------------------------------------------------------

  const fmtKm = (m) => (m >= 10000 ? (m / 1000).toFixed(0) : (m / 1000).toFixed(1)) + " km";
  const fmtTime = (s) => {
    const min = Math.max(1, Math.round(s / 60));
    return min < 60 ? `${min} min` : `${Math.floor(min / 60)} h ${String(min % 60).padStart(2, "0")}`;
  };
  const kmh = (ms) => Math.round(ms * 3.6);
  const compass = (deg) => COMPASS[Math.round((((deg % 360) + 360) % 360) / 22.5) % 16];
  const gradeColor = (g) => GRADE_STOPS.find((s) => g <= s.max).color;
  const esc = (s) => String(s).replace(/[&<>"']/g, (c) => ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;", "'": "&#39;" }[c]));
  const debounce = (fn, ms) => { let t; return (...a) => { clearTimeout(t); t = setTimeout(() => fn(...a), ms); }; };
  const cssVar = (name) => getComputedStyle(document.documentElement).getPropertyValue(name).trim();
  const isDark = () => (document.documentElement.dataset.theme || (prefersDark.matches ? "dark" : "light")) === "dark";
  const pinHtml = (which) => `<span class="pin pin-${which === "start" ? "a" : "b"}" aria-hidden="true"><span>${which === "start" ? "A" : "B"}</span></span>`;

  let toastTimer;
  function toast(msg) {
    const el = $("toast");
    el.textContent = msg;
    el.hidden = false;
    clearTimeout(toastTimer);
    toastTimer = setTimeout(() => { el.hidden = true; }, 2800);
  }

  async function getJSON(url, opts) {
    const res = await fetch(url, opts);
    let body = null;
    try { body = await res.json(); } catch (_) { /* non-JSON error page */ }
    if (!res.ok || (body && body.ok === false)) throw new Error((body && body.error) || `Request failed (${res.status})`);
    return body;
  }

  // ---- map, panes, base layers --------------------------------------------------

  const map = L.map("map", { zoomControl: false, preferCanvas: true, minZoom: 9 }).setView(YORK_CENTER, 11);
  L.control.zoom({ position: "bottomright" }).addTo(map);
  L.control.scale({ position: "bottomleft", imperial: false }).addTo(map);
  map.createPane("terrain").style.zIndex = 250;
  map.createPane("network").style.zIndex = 350;
  map.createPane("routes").style.zIndex = 450;
  map.createPane("labels").style.zIndex = 470;
  map.getPane("labels").style.pointerEvents = "none";
  // SVG so the invisible hit lines around each route reliably catch the pointer.
  const routeRenderer = L.svg({ pane: "routes", padding: 0.4 });

  const osmAttr = '&copy; <a href="https://www.openstreetmap.org/copyright">OpenStreetMap</a>';
  const esri = (path, opts = {}) => L.tileLayer(`https://server.arcgisonline.com/ArcGIS/rest/services/${path}/MapServer/tile/{z}/{y}/{x}`,
    { attribution: "Tiles &copy; Esri, HERE, Garmin, OpenStreetMap contributors", maxNativeZoom: 16, maxZoom: 19, ...opts });
  const BASES = {
    light: { label: "Light", base: esri("Canvas/World_Light_Gray_Base"), labels: esri("Canvas/World_Light_Gray_Reference", { pane: "labels" }) },
    dark: { label: "Dark", base: esri("Canvas/World_Dark_Gray_Base"), labels: esri("Canvas/World_Dark_Gray_Reference", { pane: "labels" }) },
    streets: { label: "Streets", base: L.tileLayer("https://tile.openstreetmap.org/{z}/{x}/{y}.png", { attribution: osmAttr, maxZoom: 19 }) },
    cycle: { label: "Cycling", base: L.tileLayer("https://{s}.tile-cyclosm.openstreetmap.fr/cyclosm/{z}/{x}/{y}.png", { attribution: `${osmAttr}, CyclOSM`, maxZoom: 19 }) },
    satellite: {
      label: "Satellite",
      base: esri("World_Imagery", { attribution: "Imagery &copy; Esri", maxNativeZoom: 19 }),
      labels: esri("Reference/World_Boundaries_and_Places", { pane: "labels", maxNativeZoom: 19 }),
    },
  };
  let currentBase = null;
  function setBase(key) {
    if (currentBase) {
      map.removeLayer(BASES[currentBase].base);
      if (BASES[currentBase].labels) map.removeLayer(BASES[currentBase].labels);
    }
    currentBase = key;
    BASES[key].base.addTo(map);
    if (BASES[key].labels) BASES[key].labels.addTo(map);
    const radio = document.querySelector(`input[name="base"][value="${key}"]`);
    if (radio) radio.checked = true;
  }

  // ---- overlays -------------------------------------------------------------------

  const overlays = {
    bike_network: {
      label: "Bike lanes and trails", on: true,
      make: async () => {
        const data = await getJSON("/static/data/bike_network.geojson");
        return L.geoJSON(data, {
          pane: "network",
          style: (f) => ({ color: (FACILITIES[f.properties.f] || FACILITIES.none).color, weight: f.properties.f === "sharrow" ? 1.5 : 2.2, opacity: 0.75 }),
          onEachFeature: (f, layer) => {
            const p = f.properties;
            layer.bindTooltip(`<b>${esc(p.n || "Unnamed")}</b><br>${esc((FACILITIES[p.f] || {}).label || p.f)}`, { sticky: true, className: "net-tip" });
          },
        });
      },
    },
    hillshade: {
      label: "Terrain shading", note: "Vaughan, Richmond Hill, Markham", on: false,
      make: async () => L.imageOverlay("/static/data/hillshade.png", state.meta.overlays.bounds, { pane: "terrain", opacity: 0.8 }),
      needs: () => state.meta && state.meta.overlays,
    },
    elevation: {
      label: "Elevation colour", note: "Height above sea level", on: false,
      make: async () => L.imageOverlay("/static/data/elevation.png", state.meta.overlays.bounds, { pane: "terrain", opacity: 0.5 }),
      needs: () => state.meta && state.meta.overlays,
    },
    steep: {
      label: "Steep hills", note: "Grades of 4% or more", on: false,
      make: async () => {
        const data = await getJSON("/static/data/steep_segments.geojson");
        return L.geoJSON(data, {
          pane: "network",
          style: (f) => ({ color: gradeColor(f.properties.g), weight: 3, opacity: 0.9 }),
          onEachFeature: (f, layer) => layer.bindTooltip(`${esc(f.properties.n || "Unnamed")}, up to ${f.properties.g}%`, { sticky: true, className: "net-tip" }),
        });
      },
    },
    wind: {
      label: "Wind arrows", on: false,
      make: async () => L.layerGroup(),
    },
    boundary: {
      label: "York Region boundary", on: true,
      make: async () => {
        const data = await getJSON("/static/data/york_boundary.geojson");
        return L.geoJSON(data, { pane: "network", style: { color: "#17302a", weight: 1.5, opacity: 0.35, dashArray: "1 5", lineCap: "round" }, interactive: false });
      },
    },
    coverage: {
      label: "Where hills are measured", on: false,
      make: async () => {
        const data = await getJSON("/static/data/dem_coverage.geojson");
        return L.geoJSON(data, { pane: "network", style: { color: "#2f64ad", weight: 1.5, dashArray: "6 4", fillOpacity: 0.04 }, interactive: false });
      },
    },
  };

  async function setOverlay(key, on) {
    const ov = overlays[key];
    ov.on = on;
    if (on) {
      try {
        if (!ov.layer) ov.layer = await ov.make();
        if (ov.on) ov.layer.addTo(map);
        if (key === "wind") drawWindField();
      } catch (err) {
        ov.on = false;
        const box = document.querySelector(`input[data-overlay="${key}"]`);
        if (box) box.checked = false;
        toast(`${ov.label} isn't available. Run make layers.`);
      }
    } else if (ov.layer) {
      map.removeLayer(ov.layer);
    }
    renderLegend();
  }

  function buildLayerPanel() {
    $("base-options").innerHTML = Object.entries(BASES).map(([k, b]) =>
      `<label><input type="radio" name="base" value="${k}"><span class="base-thumb thumb-${k}"></span>${b.label}</label>`).join("");
    $("base-options").addEventListener("change", (e) => setBase(e.target.value));

    $("overlay-options").innerHTML = Object.entries(overlays).map(([k, ov]) => {
      const available = !ov.needs || ov.needs();
      return `<label class="overlay-row${available ? "" : " unavailable"}">
        <span class="ov-text">${ov.label}${ov.note ? `<small>${ov.note}</small>` : ""}</span>
        <span class="switch"><input type="checkbox" data-overlay="${k}" ${ov.on && available ? "checked" : ""} ${available ? "" : "disabled"}><span></span></span>
      </label>`;
    }).join("");
    $("overlay-options").addEventListener("change", (e) => {
      if (e.target.dataset.overlay) setOverlay(e.target.dataset.overlay, e.target.checked);
    });
    $("layers-toggle").addEventListener("click", () => toggleLayers());
  }

  function toggleLayers(force) {
    const body = $("layers-body");
    body.hidden = force === undefined ? !body.hidden : !force;
    $("layers-toggle").setAttribute("aria-expanded", String(!body.hidden));
  }

  function renderLegend() {
    const parts = [];
    if (overlays.bike_network.on || state.colorMode === "facility") {
      parts.push(`<div><h3>Bike facilities</h3><ul>${Object.entries(FACILITIES).filter(([k]) => k !== "none" || state.colorMode === "facility")
        .map(([, f]) => `<li><i style="background:${f.color}"></i>${f.label}</li>`).join("")}</ul></div>`);
    }
    if (overlays.steep.on || state.colorMode === "grade") {
      parts.push(`<div><h3>Grade</h3><ul>${GRADE_STOPS.map((s) => `<li><i style="background:${s.color}"></i>${s.label}</li>`).join("")}</ul></div>`);
    }
    if (overlays.elevation.on && state.meta && state.meta.overlays) {
      const o = state.meta.overlays;
      parts.push(`<div><h3>Elevation</h3><div class="ramp" style="background:linear-gradient(90deg,#2e7d50,#84a854,#dec778,#ba6e40,#f5f0e8)"></div>
        <div class="ramp-labels"><span>${Math.round(o.min_elev)} m</span><span>${Math.round(o.max_elev)} m</span></div></div>`);
    }
    $("legend").innerHTML = parts.join("");
  }

  // Background layers step back while routes are on screen.
  function setNetworkEmphasis() {
    map.getPane("network").style.opacity = state.routes.length ? "0.35" : "1";
  }
  map.getPane("network").style.transition = "opacity .25s ease";

  // ---- wind arrows on the map -------------------------------------------------------

  function effectiveWind() {
    if (state.wind.mode === "manual") return { speed: state.wind.speed, dir: state.wind.dir };
    if (state.wind.mode === "live" && state.live) return { speed: state.live.speed_ms, dir: state.live.from_deg };
    return { speed: 0, dir: 0 };
  }

  function drawWindField() {
    const ov = overlays.wind;
    if (!ov.on || !ov.layer) return;
    ov.layer.clearLayers();
    const w = effectiveWind();
    if (w.speed < 0.3) return;
    const b = map.getBounds();
    const size = map.getSize();
    const cols = Math.max(3, Math.round(size.x / 180));
    const rows = Math.max(2, Math.round(size.y / 180));
    const len = 16 + Math.min(1, w.speed / 12) * 22;
    const toward = (w.dir + 180) % 360;
    const opacity = 0.35 + Math.min(1, w.speed / 10) * 0.4;
    const color = cssVar("--wind");
    for (let r = 0; r < rows; r++) {
      for (let c = 0; c < cols; c++) {
        const lat = b.getSouth() + ((r + 0.5) / rows) * (b.getNorth() - b.getSouth());
        const lng = b.getWest() + ((c + 0.5 + (r % 2 ? 0.25 : -0.25)) / cols) * (b.getEast() - b.getWest());
        const html = `<svg width="40" height="40" viewBox="-20 -20 40 40" style="transform:rotate(${toward}deg);opacity:${opacity}">
          <line x1="0" y1="${len / 2}" x2="0" y2="${-len / 2 + 5}" stroke="${color}" stroke-width="2.5" stroke-linecap="round"/>
          <path d="M0 ${-len / 2} l-5 8 h10z" fill="${color}"/></svg>`;
        L.marker([lat, lng], { icon: L.divIcon({ className: "", html, iconSize: [40, 40] }), interactive: false, keyboard: false, pane: "network" }).addTo(ov.layer);
      }
    }
  }
  map.on("moveend", debounce(drawWindField, 120));

  // ---- start / end: explicit picking ----------------------------------------------------

  const pinIcon = (which) => L.divIcon({
    className: "", html: `<div class="map-pin ${which === "start" ? "a" : "b"}"><span>${which === "start" ? "A" : "B"}</span></div>`,
    iconSize: [30, 30], iconAnchor: [15, 34],
  });
  const markers = { start: null, end: null };
  const labelSeq = { start: 0, end: 0 };
  const inputFor = (which) => $(which === "start" ? "from-input" : "to-input");
  const nameOf = (which) => (which === "start" ? "start" : "destination");

  function startPicking(which) {
    if (state.picking === which) { stopPicking(); return; }
    state.picking = which;
    document.querySelector(".map-wrap").classList.add("picking");
    $("pick-banner").hidden = false;
    $("pick-pin").className = `pin pin-${which === "start" ? "a" : "b"}`;
    $("pick-pin").innerHTML = `<span>${which === "start" ? "A" : "B"}</span>`;
    $("pick-text").textContent = `${touch ? "Tap" : "Click"} the map to set your ${nameOf(which)}`;
    document.querySelectorAll(".pick-btn").forEach((b) => b.setAttribute("aria-pressed", String(b.dataset.pick === which)));
    map.closePopup();
  }

  function stopPicking() {
    state.picking = null;
    document.querySelector(".map-wrap").classList.remove("picking");
    $("pick-banner").hidden = true;
    document.querySelectorAll(".pick-btn").forEach((b) => b.setAttribute("aria-pressed", "false"));
  }

  document.querySelectorAll(".pick-btn").forEach((b) => b.addEventListener("click", () => startPicking(b.dataset.pick)));
  $("pick-cancel").addEventListener("click", stopPicking);

  // Map clicks only place points while picking. Otherwise they do nothing,
  // so panning and exploring never moves your trip by accident.
  map.on("click", (e) => {
    if (!state.picking) return;
    const which = state.picking;
    stopPicking();
    placePoint(which, e.latlng);
  });

  // Right-click (or long-press on touch) offers the same choice explicitly.
  map.on("contextmenu", (e) => {
    stopPicking();
    const el = document.createElement("div");
    el.className = "ctx";
    el.innerHTML = `<button type="button" data-w="start">${pinHtml("start")}Start here</button>
      <button type="button" data-w="end">${pinHtml("end")}End here</button>`;
    const popup = L.popup({ closeButton: false, className: "ctx-popup", offset: [0, -2], autoPan: false })
      .setLatLng(e.latlng).setContent(el).openOn(map);
    el.addEventListener("click", (ev) => {
      const btn = ev.target.closest("button[data-w]");
      if (!btn) return;
      map.closePopup(popup);
      placePoint(btn.dataset.w, e.latlng);
    });
    el.querySelector("button").focus();
  });

  function placePoint(which, latlng, label) {
    setPoint(which, latlng, label);
    if (!label) labelPoint(which, latlng);
    updateTripButtons();
    if (state.start && state.end) requestRoutes();
    else renderRoutes();
  }

  function setPoint(which, latlng, label) {
    state[which] = { lat: latlng.lat, lng: latlng.lng };
    if (!markers[which]) {
      markers[which] = L.marker(latlng, { icon: pinIcon(which), draggable: true, autoPan: true, zIndexOffset: 1000, title: `Drag to move the ${nameOf(which)}` })
        .addTo(map)
        .on("dragend", (e) => {
          const p = e.target.getLatLng();
          state[which] = { lat: p.lat, lng: p.lng };
          labelPoint(which, p);
          requestRoutes();
        });
    } else {
      markers[which].setLatLng(latlng);
    }
    if (label) inputFor(which).value = label;
  }

  async function labelPoint(which, latlng) {
    const input = inputFor(which);
    const mine = ++labelSeq[which];
    input.value = "Dropped pin";
    try {
      const body = await getJSON(`/api/reverse?lat=${latlng.lat.toFixed(6)}&lon=${latlng.lng.toFixed(6)}`);
      if (mine === labelSeq[which]) input.value = body.label || "Dropped pin";
    } catch (_) {
      if (mine === labelSeq[which]) input.value = `${latlng.lat.toFixed(5)}, ${latlng.lng.toFixed(5)}`;
    }
  }

  function updateTripButtons() {
    $("swap-btn").disabled = !(state.start || state.end);
    $("clear-btn").disabled = !(state.start || state.end);
  }

  function clearTrip() {
    for (const which of ["start", "end"]) {
      if (markers[which]) map.removeLayer(markers[which]);
      markers[which] = null;
      state[which] = null;
      inputFor(which).value = "";
      labelSeq[which]++;
    }
    if (routeAbort) routeAbort.abort();
    setLoading(false);
    state.routes = [];
    state.selected = null;
    state.lastResult = null;
    clearRouteLayers();
    setNetworkEmphasis();
    renderRoutes();
    updateTripButtons();
    writeHash();
  }

  $("swap-btn").addEventListener("click", () => {
    const a = state.start, b = state.end;
    const la = inputFor("start").value, lb = inputFor("end").value;
    for (const which of ["start", "end"]) {
      if (markers[which]) map.removeLayer(markers[which]);
      markers[which] = null;
      state[which] = null;
      inputFor(which).value = "";
    }
    if (b) setPoint("start", L.latLng(b.lat, b.lng), lb || "Dropped pin");
    if (a) setPoint("end", L.latLng(a.lat, a.lng), la || "Dropped pin");
    updateTripButtons();
    if (state.start && state.end) requestRoutes(); else renderRoutes();
  });
  $("clear-btn").addEventListener("click", clearTrip);

  // ---- place search --------------------------------------------------------------------

  function setupSearch(which) {
    const input = inputFor(which);
    const list = $(which === "start" ? "from-suggest" : "to-suggest");
    let results = [], active = -1, seq = 0;

    const close = () => { list.hidden = true; active = -1; };
    const choose = (r) => {
      labelSeq[which]++;
      setPoint(which, L.latLng(r.lat, r.lon), r.label || r.name.split(",")[0]);
      close();
      input.blur();
      updateTripButtons();
      if (state.start && state.end) requestRoutes();
      else { map.setView([r.lat, r.lon], 14); renderRoutes(); }
    };
    const render = () => {
      list.innerHTML = results.length
        ? results.map((r, i) => {
          const rest = r.name.split(",").slice(1, 4).join(",").trim();
          return `<li role="option" data-i="${i}" aria-selected="${i === active}"><b>${esc(r.label || r.name.split(",")[0])}</b><small>${esc(rest)}</small></li>`;
        }).join("")
        : `<li class="none">No matches in York Region</li>`;
      list.hidden = false;
    };
    const search = debounce(async () => {
      const q = input.value.trim();
      if (q.length < 3 || /^-?\d+(\.\d+)?,\s*-?\d+(\.\d+)?$/.test(q)) { close(); return; }
      const mine = ++seq;
      try {
        const body = await getJSON(`/api/geocode?q=${encodeURIComponent(q)}`);
        if (mine !== seq || document.activeElement !== input) return;
        results = body.results; active = -1; render();
      } catch (err) { close(); toast(err.message); }
    }, 350);

    input.addEventListener("input", search);
    input.addEventListener("focus", () => input.select());
    input.addEventListener("keydown", (e) => {
      if (list.hidden) {
        if (e.key === "Enter") {
          const m = input.value.match(/^\s*(-?\d+(?:\.\d+)?),\s*(-?\d+(?:\.\d+)?)\s*$/);
          if (m) placePoint(which, L.latLng(+m[1], +m[2]), input.value.trim());
        }
        return;
      }
      if (e.key === "ArrowDown") { active = Math.min(results.length - 1, active + 1); render(); e.preventDefault(); }
      else if (e.key === "ArrowUp") { active = Math.max(0, active - 1); render(); e.preventDefault(); }
      else if (e.key === "Enter" && results[Math.max(0, active)]) { choose(results[Math.max(0, active)]); e.preventDefault(); }
      else if (e.key === "Escape") close();
    });
    list.addEventListener("mousedown", (e) => {
      const li = e.target.closest("li[data-i]");
      if (li) { e.preventDefault(); choose(results[+li.dataset.i]); }
    });
    input.addEventListener("blur", () => setTimeout(close, 150));
  }

  // ---- ride settings (folded) -------------------------------------------------------------

  const settingPanels = { rider: $("rider-panel"), wind: $("wind-panel") };
  function toggleSetting(which) {
    for (const [k, panel] of Object.entries(settingPanels)) {
      const open = k === which && panel.hidden;
      panel.hidden = !open;
      $(`${k}-toggle`).setAttribute("aria-expanded", String(open));
    }
  }
  $("rider-toggle").addEventListener("click", () => toggleSetting("rider"));
  $("wind-toggle").addEventListener("click", () => toggleSetting("wind"));

  function currentPreset() {
    return Object.entries(PRESETS).find(([, p]) => p.mass === state.rider.mass && p.power === state.rider.power && p.cda === state.rider.cda);
  }
  function syncRiderUI() {
    $("mass").value = state.rider.mass;
    $("power").value = state.rider.power;
    $("cda").value = state.rider.cda;
    $("mass-out").textContent = `${state.rider.mass} kg`;
    $("power-out").textContent = `${state.rider.power} W`;
    const preset = currentPreset();
    document.querySelectorAll("#presets button").forEach((b) => b.setAttribute("aria-checked", String(!!preset && b.dataset.preset === preset[0])));
    $("rider-main").textContent = preset ? preset[1].label : "Custom rider";
    $("rider-sub").textContent = `${state.rider.mass} kg, ${state.rider.power} W`;
  }
  const reroute = debounce(() => requestRoutes(), 400);

  document.querySelectorAll("#presets button").forEach((b) => b.addEventListener("click", () => {
    const { mass, power, cda } = PRESETS[b.dataset.preset];
    state.rider = { mass, power, cda };
    syncRiderUI(); reroute();
  }));
  $("mass").addEventListener("input", (e) => { state.rider.mass = +e.target.value; syncRiderUI(); reroute(); });
  $("power").addEventListener("input", (e) => { state.rider.power = +e.target.value; syncRiderUI(); reroute(); });
  $("cda").addEventListener("change", (e) => { state.rider.cda = e.target.value; syncRiderUI(); reroute(); });

  // ---- wind -----------------------------------------------------------------------------

  function buildDialTicks() {
    let html = "";
    for (let d = 0; d < 360; d += 15) {
      const r1 = d % 90 === 0 ? 44 : 47, a = (d * Math.PI) / 180;
      html += `<line x1="${Math.sin(a) * r1}" y1="${-Math.cos(a) * r1}" x2="${Math.sin(a) * 50}" y2="${-Math.cos(a) * 50}"/>`;
    }
    $("dial-ticks").innerHTML = html;
  }

  function renderWind() {
    const w = effectiveWind();
    const dial = $("dial");
    $("dial-arrow").setAttribute("transform", `rotate(${w.dir})`);
    dial.setAttribute("aria-valuenow", String(Math.round(w.dir)));
    dial.classList.toggle("editable", state.wind.mode === "manual");
    dial.classList.toggle("calm", w.speed < 0.3);
    $("wind-speed-wrap").hidden = state.wind.mode !== "manual";
    $("wind-speed").value = state.wind.speed;
    $("wind-speed-out").textContent = `${kmh(state.wind.speed)} km/h`;
    $("wind-icon").style.transform = `rotate(${w.dir}deg)`;
    $("wind-icon").style.opacity = w.speed < 0.3 ? "0.3" : "1";

    let big, note, main, sub;
    if (state.wind.mode === "none") {
      big = "Wind ignored"; note = "Routes are scored as if the air were still.";
      main = "No wind"; sub = "Ignored";
    } else if (state.wind.mode === "live" && !state.live) {
      big = "Fetching wind…"; note = ""; main = "Wind"; sub = "Fetching…";
    } else if (w.speed < 0.3) {
      big = "Calm"; note = state.wind.mode === "live" ? liveStamp() : "Drag the dial to set a direction.";
      main = "Calm"; sub = state.wind.mode === "live" ? "Live" : "Your setting";
    } else {
      big = `${kmh(w.speed)} km/h from the ${compass(w.dir)}`;
      note = state.wind.mode === "live" ? liveStamp() : "Drag the dial to where the wind comes from.";
      main = `${kmh(w.speed)} km/h ${compass(w.dir)}`;
      sub = state.wind.mode === "live" ? "Live wind" : "Your wind";
    }
    $("wind-big").textContent = big;
    $("wind-note").textContent = note;
    $("wind-main").textContent = main;
    $("wind-sub").textContent = sub;
    drawWindField();
  }

  function liveStamp() {
    const l = state.live;
    if (!l) return "";
    const t = l.observed_at ? new Date(l.observed_at).toLocaleTimeString([], { hour: "numeric", minute: "2-digit" }) : "";
    const gust = l.gusts_ms > l.speed_ms + 1 ? ` Gusts to ${kmh(l.gusts_ms)} km/h.` : "";
    return `Open-Meteo, ${t}.${gust}`;
  }

  async function fetchLiveWind() {
    const c = state.start && state.end
      ? { lat: (state.start.lat + state.end.lat) / 2, lng: (state.start.lng + state.end.lng) / 2 }
      : map.getCenter();
    try {
      state.live = await getJSON(`/api/wind?lat=${c.lat.toFixed(3)}&lon=${c.lng.toFixed(3)}`);
    } catch (_) {
      state.live = null;
      $("wind-big").textContent = "Live wind unavailable";
      $("wind-note").textContent = "Set your own wind, or ignore it.";
      $("wind-sub").textContent = "Unavailable";
      return;
    }
    renderWind();
  }

  document.querySelectorAll("[data-wind]").forEach((b) => b.addEventListener("click", () => {
    state.wind.mode = b.dataset.wind;
    document.querySelectorAll("[data-wind]").forEach((x) => x.setAttribute("aria-checked", String(x === b)));
    if (state.wind.mode === "manual" && state.live) { state.wind.speed = Math.round(state.live.speed_ms * 2) / 2; state.wind.dir = Math.round(state.live.from_deg); }
    if (state.wind.mode === "live" && !state.live) fetchLiveWind();
    renderWind(); reroute();
  }));

  function dialAngle(evt) {
    const r = $("dial").getBoundingClientRect();
    const x = evt.clientX - (r.left + r.width / 2), y = evt.clientY - (r.top + r.height / 2);
    return ((Math.atan2(x, -y) * 180) / Math.PI + 360) % 360;
  }
  let dragging = false;
  $("dial").addEventListener("pointerdown", (e) => {
    if (state.wind.mode !== "manual") { toast("Choose Set my own to change the wind."); return; }
    dragging = true;
    $("dial").setPointerCapture(e.pointerId);
    state.wind.dir = Math.round(dialAngle(e));
    renderWind();
  });
  $("dial").addEventListener("pointermove", (e) => {
    if (!dragging) return;
    state.wind.dir = Math.round(dialAngle(e));
    renderWind();
  });
  $("dial").addEventListener("pointerup", () => { if (dragging) { dragging = false; reroute(); } });
  $("dial").addEventListener("keydown", (e) => {
    if (state.wind.mode !== "manual") return;
    const step = e.shiftKey ? 45 : 15;
    if (e.key === "ArrowRight" || e.key === "ArrowUp") state.wind.dir = (state.wind.dir + step) % 360;
    else if (e.key === "ArrowLeft" || e.key === "ArrowDown") state.wind.dir = (state.wind.dir - step + 360) % 360;
    else return;
    e.preventDefault(); renderWind(); reroute();
  });
  $("wind-speed").addEventListener("input", (e) => { state.wind.speed = +e.target.value; renderWind(); reroute(); });

  // ---- routing ------------------------------------------------------------------------------

  let routeAbort = null;
  const routeLayers = new Map();  // profile -> { casing, line, hit }
  let selectedOverlay = null, selectedOverlayKey = null;
  let connectors = null;
  let hoverMarker = null;

  function writeHash() {
    if (!state.start || !state.end) { history.replaceState(null, "", location.pathname); return; }
    const f = (p) => `${p.lat.toFixed(5)},${p.lng.toFixed(5)}`;
    history.replaceState(null, "", `#from=${f(state.start)}&to=${f(state.end)}`);
  }
  function readHash() {
    const h = new URLSearchParams(location.hash.slice(1));
    const parse = (v) => { const m = (v || "").split(",").map(Number); return m.length === 2 && m.every(Number.isFinite) ? L.latLng(m[0], m[1]) : null; };
    const a = parse(h.get("from")), b = parse(h.get("to"));
    if (!a || !b) return false;
    setPoint("start", a); labelPoint("start", a);
    setPoint("end", b); labelPoint("end", b);
    updateTripButtons();
    return true;
  }

  async function requestRoutes() {
    if (!state.start || !state.end) return;
    writeHash();
    if (routeAbort) routeAbort.abort();
    const controller = routeAbort = new AbortController();
    const p = new URLSearchParams({
      from: `${state.start.lat.toFixed(6)},${state.start.lng.toFixed(6)}`,
      to: `${state.end.lat.toFixed(6)},${state.end.lng.toFixed(6)}`,
      mass: state.rider.mass, power: state.rider.power, cda: state.rider.cda,
      max_power: Math.round(state.rider.power * 1.7),
      wind: state.wind.mode,
    });
    if (state.wind.mode === "manual") { p.set("wind_speed", state.wind.speed); p.set("wind_dir", state.wind.dir); }

    setLoading(true);
    try {
      const body = await getJSON(`/api/route?${p}`, { signal: controller.signal });
      state.lastResult = body;
      if (body.wind && body.wind.source === "live") { state.live = { ...state.live, ...body.wind }; renderWind(); }
      const firstTime = !state.routes.length;
      state.routes = body.routes.slice().sort((a, b) => PROFILE_ORDER.indexOf(a.profile) - PROFILE_ORDER.indexOf(b.profile));
      const keep = state.routes.find((r) => r.profile === state.selected);
      const rec = state.routes.find((r) => r.profile === RECOMMENDED || r.also.includes(RECOMMENDED));
      state.selected = keep ? keep.profile : (rec || state.routes[0] || {}).profile || null;
      drawRoutes();
      renderRoutes(body.warnings || []);
      if (firstTime || !keep) fitRoutes();
    } catch (err) {
      if (err.name === "AbortError") return;
      state.routes = [];
      clearRouteLayers();
      renderRoutes([], err.message);
    } finally {
      if (controller === routeAbort) setLoading(false);
    }
    setNetworkEmphasis();
  }

  function setLoading(on) {
    state.loading = on;
    $("progress").hidden = !on;
    if (on) {
      $("routes-meta").textContent = "Finding routes…";
      if (!state.routes.length) $("routes").innerHTML = '<div class="skeleton"></div><div class="skeleton"></div><div class="skeleton"></div>';
    }
  }

  function clearRouteLayers() {
    for (const l of routeLayers.values()) { map.removeLayer(l.casing); map.removeLayer(l.line); map.removeLayer(l.hit); }
    routeLayers.clear();
    if (selectedOverlay) map.removeLayer(selectedOverlay);
    selectedOverlay = null;
    selectedOverlayKey = null;
    if (connectors) map.removeLayer(connectors);
    connectors = null;
    if (hoverMarker) map.removeLayer(hoverMarker);
    hoverMarker = null;
  }

  const latlngsOf = (r) => r.geometry.coordinates.map(([lng, lat]) => [lat, lng]);

  function drawRoutes() {
    clearRouteLayers();
    const casingColor = cssVar("--casing");
    for (const r of state.routes) {
      const pts = latlngsOf(r);
      const base = { renderer: routeRenderer, lineCap: "round", lineJoin: "round", interactive: false };
      const casing = L.polyline(pts, { ...base, color: casingColor }).addTo(map);
      const line = L.polyline(pts, { ...base, color: PROFILES[r.profile].color }).addTo(map);
      // A wide, invisible line gives a forgiving target for hover and click.
      const hit = L.polyline(pts, { renderer: routeRenderer, color: "#000", opacity: 0, weight: touch ? 34 : 24, lineCap: "round", lineJoin: "round", interactive: true, bubblingMouseEvents: false }).addTo(map);
      hit.on("click", (e) => {
        if (state.picking) { const which = state.picking; stopPicking(); placePoint(which, e.latlng); }
        else selectRoute(r.profile);
      });
      hit.on("contextmenu", (e) => map.fire("contextmenu", e));
      hit.on("mouseover", () => setHover(r.profile));
      hit.on("mouseout", () => setHover(null));
      hit.bindTooltip(() => routeTip(r), { sticky: true, direction: "top", offset: [0, -10], className: "route-tip", opacity: 1 });
      routeLayers.set(r.profile, { casing, line, hit });
    }
    // Dashed connectors when a pin is off the network (e.g. in a park or a lot).
    const res = state.lastResult;
    if (res) {
      const dash = L.featureGroup();
      for (const key of ["start", "end"]) {
        const s = res[key];
        if (s && s.distance_m > 15) {
          L.polyline([[s.requested[1], s.requested[0]], [s.snapped[1], s.snapped[0]]],
            { renderer: routeRenderer, color: cssVar("--ink-3"), weight: 2.5, dashArray: "2 6", lineCap: "round", interactive: false }).addTo(dash);
        }
      }
      connectors = dash.addTo(map);
    }
    styleRoutes();
  }

  function routeTip(r) {
    const prof = PROFILES[r.profile];
    if (r.profile === state.selected) return `<b>${prof.label}</b>, ${fmtTime(r.time_s)}, ${fmtKm(r.distance_m)}`;
    const sel = state.routes.find((x) => x.profile === state.selected);
    let delta = "";
    if (sel) {
      const dt = Math.round((r.time_s - sel.time_s) / 60);
      delta = dt === 0 ? ", same time" : `, ${Math.abs(dt)} min ${dt > 0 ? "slower" : "faster"}`;
    }
    return `<b>${prof.label}</b>, ${fmtTime(r.time_s)}${delta}<br><small>Click to choose this route</small>`;
  }

  function styleRoutes() {
    for (const [profile, l] of routeLayers) {
      const sel = profile === state.selected;
      const hov = profile === state.hovered && !sel;
      if (sel) {
        l.casing.setStyle({ weight: 12, opacity: 1 });
        l.line.setStyle({ weight: 7, opacity: state.colorMode === "profile" ? 1 : 0 });
      } else {
        l.casing.setStyle({ weight: hov ? 11 : 7, opacity: hov ? 1 : 0.9 });
        l.line.setStyle({ weight: hov ? 6 : 4, opacity: hov ? 1 : (isDark() ? 0.85 : 0.6) });
      }
    }
    updateSelectedOverlay();
    // Stack: alternatives, then the selected route, then a hovered alternative on top.
    const order = [...routeLayers.keys()].sort((a, b) => rank(a) - rank(b));
    for (const p of order) {
      const l = routeLayers.get(p);
      l.casing.bringToFront();
      l.line.bringToFront();
      if (p === state.selected && selectedOverlay) selectedOverlay.eachLayer((x) => x.bringToFront());
    }
    if (connectors) connectors.eachLayer((x) => x.bringToFront());
    // Hit lines above everything; the selected one lowest so alternatives stay easy to grab.
    const hitOrder = [...routeLayers.keys()].sort((a, b) => (a === state.selected ? -1 : b === state.selected ? 1 : 0));
    for (const p of hitOrder) routeLayers.get(p).hit.bringToFront();
  }
  const rank = (p) => (p === state.hovered && p !== state.selected ? 2 : p === state.selected ? 1 : 0);

  function updateSelectedOverlay() {
    const key = `${state.selected}|${state.colorMode}|${routeLayers.size}`;
    if (selectedOverlayKey === key) return;
    selectedOverlayKey = key;
    if (selectedOverlay) { map.removeLayer(selectedOverlay); selectedOverlay = null; }
    const r = state.routes.find((x) => x.profile === state.selected);
    if (r && state.colorMode !== "profile") selectedOverlay = colouredRoute(r, state.colorMode).addTo(map);
    renderLegend();
  }

  // Splits the selected route into runs of equal colour by grade or facility.
  function colouredRoute(r, mode) {
    const pts = latlngsOf(r);
    const colorAt = [];
    if (mode === "grade") {
      for (let i = 0; i < pts.length; i++) colorAt.push(gradeColor(r.grades_pct[i] || 0));
    } else {
      for (const s of r.steps) {
        const c = (FACILITIES[s.facility] || FACILITIES.none).color;
        for (let i = s.start; i <= s.end; i++) colorAt[i] = c;
      }
    }
    const group = L.featureGroup();
    let runStart = 0;
    for (let i = 1; i <= pts.length; i++) {
      if (i === pts.length || colorAt[i] !== colorAt[runStart]) {
        const seg = pts.slice(runStart, Math.min(i + 1, pts.length));
        if (seg.length > 1) L.polyline(seg, { renderer: routeRenderer, color: colorAt[runStart] || "#999", weight: 7, opacity: 1, lineCap: "round", lineJoin: "round", interactive: false }).addTo(group);
        runStart = i;
      }
    }
    return group;
  }

  function setHover(profile) {
    if (state.hovered === profile) return;
    state.hovered = profile;
    styleRoutes();
    document.querySelectorAll(".route").forEach((row) => row.classList.toggle("hover", row.dataset.profile === profile));
  }

  function fitRoutes() {
    const layers = [...routeLayers.values()].map((l) => l.line);
    if (!layers.length) return;
    const b = L.featureGroup(layers).getBounds();
    const mobile = window.innerWidth <= 820;
    const drawerH = mobile || $("drawer").hidden ? 60 : $("drawer").offsetHeight + 36;
    map.fitBounds(b, { paddingTopLeft: [50, 70], paddingBottomRight: [mobile ? 50 : 80, drawerH] });
  }

  function selectRoute(profile) {
    if (state.selected === profile) return;
    state.selected = profile;
    styleRoutes();
    renderRoutes(state.lastResult ? state.lastResult.warnings : []);
    const row = document.querySelector(`.route[data-profile="${profile}"]`);
    if (row) row.scrollIntoView({ block: "nearest", behavior: "smooth" });
  }

  // ---- route list ---------------------------------------------------------------------------

  function facilityBar(r) {
    const order = ["trail", "cycle_track", "buffered_lane", "bike_lane", "paved_shoulder", "sharrow", "none"];
    return `<div class="bar" aria-hidden="true">${order.filter((k) => r.facility_share[k])
      .map((k) => `<span style="width:${(r.facility_share[k] * 100).toFixed(1)}%;background:${FACILITIES[k].color}"></span>`).join("")}</div>`;
  }

  function routeRow(r) {
    const prof = PROFILES[r.profile];
    const selected = r.profile === state.selected;
    const recommended = r.profile === RECOMMENDED || r.also.includes(RECOMMENDED);
    const others = r.also.filter((a) => a !== RECOMMENDED).map((a) => PROFILES[a].label.toLowerCase());
    const head = r.avg_headwind_ms;
    const windText = Math.abs(head) < 0.3 ? "Neutral" : head > 0 ? `${kmh(head)} km/h against` : `${kmh(-head)} km/h behind`;
    const coverage = r.elevation_coverage < 0.95
      ? `<span class="badge">Hills measured on ${Math.round(r.elevation_coverage * 100)}% of this route</span>` : "";
    const facLegend = Object.entries(r.facility_share).sort((a, b) => b[1] - a[1])
      .map(([k, v]) => `<span><i style="background:${FACILITIES[k].color}"></i>${FACILITIES[k].label} ${Math.round(v * 100)}%</span>`).join("");
    const steps = r.steps.filter((s) => s.length_m >= 60);
    const stepList = steps.map((s) => `<li><span class="st-name">${esc(s.name || "Unnamed " + s.road.replace("_", " "))}</span>
        <span class="st-fac">${s.facility !== "none" ? FACILITIES[s.facility].label : ""}</span>
        <span class="st-len">${fmtKm(s.length_m)}</span></li>`).join("");

    return `<article class="route" role="option" tabindex="0" data-profile="${r.profile}" aria-selected="${selected}">
      <div class="route-top">
        <span class="swatch" style="background:${prof.color}"></span>
        <span class="route-name">${prof.label}</span>
        ${recommended ? '<span class="tag">Recommended</span>' : ""}
        <span class="route-time">${fmtTime(r.time_s)}</span>
      </div>
      <div class="route-stats">
        <span>${fmtKm(r.distance_m)}</span>
        <span>${Math.round(r.gain_m)} m climb</span>
        <span>${Math.round(r.bike_share * 100)}% bike lanes</span>
      </div>
      ${others.length ? `<p class="route-also">Also the ${others.join(" and ")} route</p>` : ""}
      ${selected ? facilityBar(r) : ""}
      <div class="route-detail">
        <dl class="detail-grid">
          <div class="metric"><dt>Effort</dt><dd>${Math.round(r.energy_kj)} kJ</dd></div>
          <div class="metric"><dt>Food energy</dt><dd>${r.food_kcal} kcal</dd></div>
          <div class="metric"><dt>Avg speed</dt><dd>${r.avg_speed_kmh} km/h</dd></div>
          <div class="metric"><dt>Climb / descent</dt><dd>${Math.round(r.gain_m)} / ${Math.round(r.loss_m)} m</dd></div>
          <div class="metric"><dt>Steepest</dt><dd>${r.max_grade_pct.toFixed(1)}%</dd></div>
          <div class="metric"><dt>Wind</dt><dd>${windText}</dd></div>
        </dl>
        ${coverage}
        <div class="fac-legend">${facLegend}</div>
        ${steps.length ? `<details class="turns"><summary>Streets on this route (${steps.length})<svg class="chev" viewBox="0 0 24 24" aria-hidden="true"><path d="m7 10 5 5 5-5"/></svg></summary><ul class="steps">${stepList}</ul></details>` : ""}
      </div>
    </article>`;
  }

  function emptyState() {
    const btn = (which, text) => `<button type="button" class="btn" data-pick-empty="${which}">${pinHtml(which)}${text}</button>`;
    if (!state.start && !state.end) {
      return `<div class="empty"><p>Where are you riding?</p>
        <div class="empty-actions">${btn("start", "Pick start on map")}</div>
        <p class="hint">Or search above. You can also ${touch ? "long-press" : "right-click"} the map.</p></div>`;
    }
    const missing = state.start ? "end" : "start";
    return `<div class="empty"><p>Now choose your ${nameOf(missing)}.</p>
      <div class="empty-actions">${btn(missing, `Pick ${nameOf(missing)} on map`)}</div></div>`;
  }

  function renderRoutes(warnings = [], error = null) {
    $("warnings").innerHTML = (error ? `<div class="notice error">${esc(error)}</div>` : "") +
      warnings.map((x) => `<div class="notice">${esc(x)}</div>`).join("");
    if (!state.routes.length) {
      $("routes-meta").textContent = "";
      $("routes").innerHTML = error ? "" : emptyState();
      $("drawer").hidden = true;
      return;
    }
    $("routes-meta").textContent = `${state.routes.length} option${state.routes.length > 1 ? "s" : ""}`;
    $("routes").innerHTML = `<div class="route-list" role="listbox" aria-label="Route options">${state.routes.map(routeRow).join("")}</div>`;
    renderProfile();
  }

  $("routes").addEventListener("click", (e) => {
    const pickBtn = e.target.closest("[data-pick-empty]");
    if (pickBtn) { startPicking(pickBtn.dataset.pickEmpty); return; }
    if (e.target.closest("details")) return;
    const row = e.target.closest(".route");
    if (row) selectRoute(row.dataset.profile);
  });
  $("routes").addEventListener("keydown", (e) => {
    const row = e.target.closest(".route");
    if (row && e.target === row && (e.key === "Enter" || e.key === " ")) { e.preventDefault(); selectRoute(row.dataset.profile); }
  });
  $("routes").addEventListener("mouseover", (e) => {
    const row = e.target.closest(".route");
    setHover(row ? row.dataset.profile : null);
  });
  $("routes").addEventListener("mouseleave", () => setHover(null));

  // ---- elevation profile ----------------------------------------------------------------------

  let chart = null;

  function renderProfile() {
    const r = state.routes.find((x) => x.profile === state.selected);
    if (!r) { $("drawer").hidden = true; return; }
    $("drawer").hidden = false;
    $("drawer-swatch").style.background = PROFILES[r.profile].color;
    $("drawer-title").textContent = `${PROFILES[r.profile].label} elevation`;
    $("drawer-sub").textContent = `${Math.round(r.gain_m)} m up, ${Math.round(r.loss_m)} m down`;
    $("chart-box").hidden = !state.drawerOpen;
    $("drawer-toggle").setAttribute("aria-expanded", String(state.drawerOpen));
    if (!state.drawerOpen) return;

    const pts = r.elevation_profile.map(([d, z, g, i]) => ({ x: d / 1000, y: z, g, i }));
    const coords = latlngsOf(r);
    const ink = cssVar("--ink-3"), rule = cssVar("--rule");
    const lineColor = PROFILES[r.profile].color;

    const data = {
      datasets: [{
        data: pts, parsing: false, pointRadius: 0, pointHoverRadius: 4, borderWidth: 2, tension: 0.15, spanGaps: false,
        borderColor: lineColor, pointHoverBackgroundColor: lineColor,
        segment: { borderColor: (ctx) => state.colorMode === "grade" ? gradeColor(pts[ctx.p1DataIndex].g) : lineColor },
        fill: "start",
        backgroundColor: (ctx) => {
          const { chartArea, ctx: c } = ctx.chart;
          if (!chartArea) return "transparent";
          const grad = c.createLinearGradient(0, chartArea.top, 0, chartArea.bottom);
          grad.addColorStop(0, lineColor + "44");
          grad.addColorStop(1, lineColor + "04");
          return grad;
        },
      }],
    };
    const options = {
      responsive: true, maintainAspectRatio: false, animation: false,
      interaction: { mode: "index", intersect: false },
      layout: { padding: { right: 4 } },
      scales: {
        x: { type: "linear", min: 0, max: r.distance_m / 1000, border: { display: false },
          ticks: { color: ink, callback: (v) => `${v} km`, maxTicksLimit: 7 }, grid: { display: false } },
        y: { border: { display: false }, ticks: { color: ink, callback: (v) => `${v} m`, maxTicksLimit: 3 }, grid: { color: rule }, grace: "15%" },
      },
      plugins: {
        legend: { display: false },
        tooltip: {
          displayColors: false,
          callbacks: {
            title: (items) => `${items[0].parsed.x.toFixed(1)} km`,
            label: (item) => {
              const p = pts[item.dataIndex];
              return p.y == null ? "Hills not measured here" : `${Math.round(p.y)} m, ${p.g > 0 ? "+" : ""}${p.g}% grade`;
            },
          },
        },
      },
      onHover: (_, active) => {
        if (!active.length) return;
        const p = pts[active[0].index];
        const ll = coords[Math.min(coords.length - 1, p.i)];
        if (!hoverMarker) hoverMarker = L.marker(ll, { icon: L.divIcon({ className: "", html: '<div class="hover-dot"></div>', iconSize: [14, 14] }), interactive: false, keyboard: false }).addTo(map);
        else hoverMarker.setLatLng(ll);
      },
    };
    if (chart) { chart.data = data; chart.options = options; chart.update(); }
    else chart = new Chart($("profile-chart"), { type: "line", data, options });
  }

  $("profile-chart").addEventListener("mouseleave", () => { if (hoverMarker) { map.removeLayer(hoverMarker); hoverMarker = null; } });
  $("drawer-toggle").addEventListener("click", () => { state.drawerOpen = !state.drawerOpen; renderProfile(); });

  document.querySelectorAll("[data-color]").forEach((b) => b.addEventListener("click", () => {
    state.colorMode = b.dataset.color;
    document.querySelectorAll("[data-color]").forEach((x) => x.setAttribute("aria-checked", String(x === b)));
    styleRoutes();
    renderProfile();
  }));

  // ---- keyboard ----------------------------------------------------------------------------------

  document.addEventListener("keydown", (e) => {
    if (e.key !== "Escape") return;
    if (state.picking) stopPicking();
    else if (!$("layers-body").hidden) toggleLayers(false);
    map.closePopup();
  });
  map.on("click", () => { if (!$("layers-body").hidden) toggleLayers(false); });

  // ---- boot ---------------------------------------------------------------------------------------

  async function init() {
    try { state.meta = await getJSON("/api/meta"); } catch (_) { state.meta = null; }
    buildLayerPanel();
    setBase(prefersDark.matches ? "dark" : "light");
    prefersDark.addEventListener("change", (e) => {
      if (currentBase === "light" || currentBase === "dark") setBase(e.matches ? "dark" : "light");
      if (state.routes.length) drawRoutes();
    });
    if (state.meta && state.meta.network && state.meta.network.bounds) map.fitBounds(state.meta.network.bounds, { padding: [20, 20] });
    if (state.meta && !state.meta.graph_ready) {
      $("warnings").innerHTML = '<div class="notice error">The routing graph hasn\'t been built. Run <code>make all-data</code>, then reload.</div>';
    }
    for (const [k, ov] of Object.entries(overlays)) if (ov.on && (!ov.needs || ov.needs())) setOverlay(k, true);
    setupSearch("start");
    setupSearch("end");
    buildDialTicks();
    syncRiderUI();
    renderWind();
    renderRoutes();
    renderLegend();
    updateTripButtons();
    await fetchLiveWind();
    if (readHash()) requestRoutes();
  }

  init();
})();
