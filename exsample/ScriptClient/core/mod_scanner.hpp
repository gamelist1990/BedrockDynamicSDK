#pragma once

#include "manifest.hpp"

namespace stageb {

struct ModCandidate {
    fs::path dir;
    std::string folderName;
    std::string manifestPath;
    ValidationResult validation;
    ModStatus status = ModStatus::Scanned;
};

struct ScanResult {
    fs::path modsDir;
    std::vector<ModCandidate> candidates;
    std::vector<std::string> diagnostics;
};

// §3.1/§3.2: scan exactly one directory level, create the mods directory if
// absent, skip '.'/'_' and manifest-less folders, sort by directory name.
ScanResult scanMods(const fs::path& modsDir, const std::string& gameVersion,
                    bool createIfMissing = true);


} // namespace stageb
