#pragma once

#include "common.hpp"

namespace stageb {

// All three directories live at the selected root. Probe with a real write;
// successfully reading the game EXE (or creating an existing directory) is
// not evidence that the installation is writable.
fs::path selectStorageRoot(const fs::path& exeDir, const fs::path& dllDir);
bool isScriptModName(const std::string& name);
bool isPackagePath(const fs::path& path);
std::vector<fs::path> listPackages(const fs::path& inbox);

// Extract one explicitly marked ZIP into a private staging directory. Publish
// the validated mod by renaming it into modsDir, never overwriting a mod.
std::string installPackage(const fs::path& archive, const fs::path& modsDir,
                           const std::string& gameVersion);

} // namespace stageb
