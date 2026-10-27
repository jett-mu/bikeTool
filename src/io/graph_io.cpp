#include "biketool/io/graph_io.hpp"

#include <cstdint>
#include <cstdio>
#include <memory>
#include <stdexcept>

namespace bt {
namespace {

constexpr char kMagic[4] = {'B', 'K', 'T', 'G'};
constexpr uint32_t kVersion = 1;

using FilePtr = std::unique_ptr<FILE, int (*)(FILE*)>;

FilePtr open(const std::string& path, const char* mode) {
    FilePtr f(std::fopen(path.c_str(), mode), &std::fclose);
    if (!f) throw std::runtime_error("cannot open " + path);
    return f;
}

template <class T>
void writeVec(FILE* f, const std::vector<T>& v) {
    const uint64_t n = v.size();
    std::fwrite(&n, sizeof n, 1, f);
    if (n) std::fwrite(v.data(), sizeof(T), n, f);
}

template <class T>
void readVec(FILE* f, std::vector<T>& v) {
    uint64_t n = 0;
    if (std::fread(&n, sizeof n, 1, f) != 1) throw std::runtime_error("truncated graph file");
    v.resize(n);
    if (n && std::fread(v.data(), sizeof(T), n, f) != n)
        throw std::runtime_error("truncated graph file");
}

}  // namespace

void saveGraph(const Graph& g, const std::string& path) {
    auto f = open(path, "wb");
    std::fwrite(kMagic, 1, 4, f.get());
    std::fwrite(&kVersion, sizeof kVersion, 1, f.get());
    writeVec(f.get(), g.nodes);
    writeVec(f.get(), g.edges);
    writeVec(f.get(), g.firstOut);
    writeVec(f.get(), g.points);
    const uint64_t nNames = g.names.size();
    std::fwrite(&nNames, sizeof nNames, 1, f.get());
    for (const auto& s : g.names) {
        const uint32_t len = static_cast<uint32_t>(s.size());
        std::fwrite(&len, sizeof len, 1, f.get());
        std::fwrite(s.data(), 1, len, f.get());
    }
    if (std::ferror(f.get())) throw std::runtime_error("write failed: " + path);
}

Graph loadGraph(const std::string& path) {
    auto f = open(path, "rb");
    char magic[4];
    uint32_t version = 0;
    if (std::fread(magic, 1, 4, f.get()) != 4 || std::fread(&version, sizeof version, 1, f.get()) != 1 ||
        std::string(magic, 4) != std::string(kMagic, 4))
        throw std::runtime_error(path + " is not a biketool graph");
    if (version != kVersion)
        throw std::runtime_error("graph version " + std::to_string(version) +
                                 " unsupported; rebuild with `make graph`");
    Graph g;
    readVec(f.get(), g.nodes);
    readVec(f.get(), g.edges);
    readVec(f.get(), g.firstOut);
    readVec(f.get(), g.points);
    uint64_t nNames = 0;
    if (std::fread(&nNames, sizeof nNames, 1, f.get()) != 1) throw std::runtime_error("truncated graph file");
    g.names.resize(nNames);
    for (auto& s : g.names) {
        uint32_t len = 0;
        if (std::fread(&len, sizeof len, 1, f.get()) != 1) throw std::runtime_error("truncated graph file");
        s.resize(len);
        if (len && std::fread(s.data(), 1, len, f.get()) != len) throw std::runtime_error("truncated graph file");
    }
    return g;
}

}  // namespace bt
