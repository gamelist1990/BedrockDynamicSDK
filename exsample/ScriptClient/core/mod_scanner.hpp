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

// §3.1: DLL-adjacent by default, overridable by BEDROCK_SDK_MODS_DIR.
fs::path resolveModsDir(const fs::path& hostDllDirectory);

} // namespace stageb
