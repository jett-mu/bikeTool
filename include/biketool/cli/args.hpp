#pragma once
// Minimal argv parser: `biketool <command> [--key value | --flag]...`.

#include <map>
#include <string>
#include <vector>

namespace bt {

class Args {
public:
    Args(int argc, char** argv);

    const std::string& command() const { return command_; }
    bool has(const std::string& key) const;
    std::string get(const std::string& key, const std::string& fallback = "") const;
    // Throws std::runtime_error when the option is missing.
    std::string require(const std::string& key) const;
    double getDouble(const std::string& key, double fallback) const;

private:
    std::string command_;
    std::map<std::string, std::string> options_;
};

}  // namespace bt
