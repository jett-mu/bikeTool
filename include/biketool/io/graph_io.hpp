#pragma once
// Versioned binary (de)serialisation of a Graph. The format is a raw dump of
// the POD arrays, so loading is a handful of freads.

#include <string>

#include "biketool/graph/graph.hpp"

namespace bt {

void saveGraph(const Graph& g, const std::string& path);
Graph loadGraph(const std::string& path);  // throws std::runtime_error

}  // namespace bt
