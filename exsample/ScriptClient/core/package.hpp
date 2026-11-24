#pragma once

#include "common.hpp"

#include <functional>
#include <memory>

namespace stageb {

// All three directories live at the selected root. Probe with a real write;
// successfully reading the game EXE (or creating an existing directory) is
// not evidence that the installation is writable.
fs::path selectStorageRoot(const fs::path& exeDir, const fs::path& dllDir);
bool isScriptModName(const std::string& name);
bool isPackagePath(const fs::path& path);
std::vector<fs::path> listPackages(const fs::path& inbox);

fs::path stageDroppedPackage(const fs::path& source, const fs::path& inbox);
void archivePackage(const fs::path& archive);

// A validated, private copy of the package. The exact bytes the user previews
// are the bytes published, even if the inbox file is changed while confirming.
class PreparedPackage {
public:
    static PreparedPackage prepare(const fs::path& archive, const fs::path& modsDir,
                                   const std::string& gameVersion);
    PreparedPackage(PreparedPackage&&) noexcept;
    PreparedPackage& operator=(PreparedPackage&&) noexcept;
    ~PreparedPackage();

    const std::string& name() const;
    const std::string& version() const;
    const std::string& oldVersion() const;
    bool isUpdate() const;
    void publish(const std::function<void()>& unload);

private:
    struct State;
    explicit PreparedPackage(std::unique_ptr<State> state);
    std::unique_ptr<State> state_;
};

// Move code out of the startup scan before attempting cleanup. Returns a
// warning if the hidden files could not be removed.
std::string removeModFiles(const fs::path& modsDir, const std::string& name,
                           const std::function<void()>& unload);

} // namespace stageb
