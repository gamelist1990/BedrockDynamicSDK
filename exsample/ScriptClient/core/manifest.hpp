#pragma once

#include "common.hpp"
#include <nlohmann/json.hpp>

namespace stageb {

struct Manifest {
    std::string name;
    std::string version;
    std::string apiVersion;      // normalized semver, e.g. "1.0.0"
    std::string entry;           // as written in the manifest
    std::string description;
    std::string displayName;
    std::vector<std::string> gameVersions;
    unsigned memoryMiB = 64;
};

struct ValidationResult {
    bool ok = false;
    bool incompatible = false;   // apiVersion / gameVersions mismatch (§3.3)
    std::string error;
    std::string warning;
    Manifest manifest;
};

// Validate one manifest against the rules in DESIGN §3.3.
// `folderName` is the immediate subdirectory name; `modDir` is its absolute
// path; `gameVersion` is the running executable's version from the profile.
ValidationResult validateManifest(const nlohmann::json& doc,
                                  const std::string& folderName,
                                  const fs::path& modDir,
                                  const std::string& gameVersion);

// Parse a semantic version (accepting the lenient "1" / "1.0" forms).
// Returns false for a malformed or non-numeric major.
bool parseSemver(const std::string& text, int& major, int& minor, int& patch);

} // namespace stageb
