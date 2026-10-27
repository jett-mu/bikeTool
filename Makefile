# York Region bike tool — single Makefile for the C++ backend and data pipeline.

ifeq ($(origin CXX),default)
CXX      := clang++
endif
GDAL_CFLAGS := $(shell gdal-config --cflags)
GDAL_LIBS   := $(shell gdal-config --libs)
CXXFLAGS ?= -O2
CXXFLAGS += -std=c++20 -Wall -Wextra -Wpedantic -Iinclude -Ithird_party $(GDAL_CFLAGS) -MMD -MP
LDLIBS   += $(GDAL_LIBS)

BIN      := bin/biketool
SRCS     := $(shell find src -name '*.cpp')
OBJS     := $(patsubst src/%.cpp,build/%.o,$(SRCS))
DEPS     := $(OBJS:.o=.d)

PYTHON   ?= python3
RAW      := data/raw
GRAPH    := data/build/graph.bin
DEM_DIR  := data/elevation
WEB_DATA := web/static/data
JSON_HPP := third_party/nlohmann/json.hpp

.PHONY: all deps data graph layers all-data serve selftest clean distclean help

all: $(BIN)

$(BIN): $(OBJS)
	@mkdir -p $(@D)
	$(CXX) $(CXXFLAGS) $^ -o $@ $(LDFLAGS) $(LDLIBS)

build/%.o: src/%.cpp | $(JSON_HPP)
	@mkdir -p $(@D)
	$(CXX) $(CXXFLAGS) -c $< -o $@

deps: $(JSON_HPP)
$(JSON_HPP):
	@mkdir -p $(@D)
	curl -fsSL -o $@ https://github.com/nlohmann/json/releases/download/v3.11.3/json.hpp

# ---- data pipeline ---------------------------------------------------------

data:
	$(PYTHON) -I scripts/fetch_data.py --out-dir $(RAW)

$(GRAPH): $(BIN) $(RAW)/york_osm.json $(RAW)/york_bike_facilities.geojson
	$(BIN) build --osm $(RAW)/york_osm.json --bikes $(RAW)/york_bike_facilities.geojson \
	             --dem-dir $(DEM_DIR) --out $@

graph: $(GRAPH)

layers: $(BIN) $(GRAPH)
	$(BIN) layers --graph $(GRAPH) --dem-dir $(DEM_DIR) --out-dir $(WEB_DATA)
	$(BIN) hillshade --dem-dir $(DEM_DIR) --out-dir $(WEB_DATA)
	cp $(RAW)/york_boundary.geojson $(WEB_DATA)/york_boundary.geojson

all-data: data graph layers

serve: $(BIN)
	$(PYTHON) server/app.py

selftest: $(BIN)
	$(BIN) selftest

clean:
	rm -rf build bin

distclean: clean
	rm -rf data/build $(WEB_DATA)/*

help:
	@echo "make            build bin/biketool"
	@echo "make data       download York Region roads, bike lanes, boundary"
	@echo "make graph      build routing graph (roads + bike lanes + elevation)"
	@echo "make layers     export map layers and terrain overlays for the web UI"
	@echo "make all-data   data + graph + layers"
	@echo "make serve      run the Flask server on http://127.0.0.1:5050"
	@echo "make selftest   run backend self-tests"

-include $(DEPS)
