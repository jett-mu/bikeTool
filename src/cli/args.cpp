#include "biketool/cli/args.hpp"

#include <cmath>
#include <cstdlib>
#include <stdexcept>

namespace bt {

Args::Args(int argc, char** argv) {
    int i = 1;
    if (i < argc && argv[i][0] != '-') command_ = argv[i++];
    for (; i < argc; ++i) {
        std::string tok = argv[i];
        if (tok.rfind("--", 0) != 0) throw std::runtime_error("unexpected argument: " + tok);
        tok = tok.substr(2);
        const auto eq = tok.find('=');
        if (eq != std::string::npos) {
            options_[tok.substr(0, eq)] = tok.substr(eq + 1);
        } else if (i + 1 < argc && std::string(argv[i + 1]).rfind("--", 0) != 0) {
            options_[tok] = argv[++i];
        } else {
            options_[tok] = "true";
        }
    }
}

bool Args::has(const std::string& key) const { return options_.count(key) > 0; }

std::string Args::get(const std::string& key, const std::string& fallback) const {
    auto it = options_.find(key);
    return it == options_.end() ? fallback : it->second;
}

std::string Args::require(const std::string& key) const {
    auto it = options_.find(key);
    if (it == options_.end()) throw std::runtime_error("missing required option --" + key);
    return it->second;
}

double Args::getDouble(const std::string& key, double fallback) const {
    auto it = options_.find(key);
    if (it == options_.end()) return fallback;
    char* end = nullptr;
    const double v = std::strtod(it->second.c_str(), &end);
    if (end == it->second.c_str() || *end != '\0' || !std::isfinite(v))
        throw std::runtime_error("option --" + key + " expects a number, got '" + it->second + "'");
    return v;
}

}  // namespace bt
