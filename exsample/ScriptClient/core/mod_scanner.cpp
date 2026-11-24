#include "mod_scanner.hpp"

#include <algorithm>
#include <fstream>

namespace stageb {

ScanResult scanMods(const fs::path& modsDir, const std::string& gameVersion,
                    bool createIfMissing) {
    ScanResult result;
    result.modsDir = fs::absolute(modsDir);
    std::error_code ec;
    if (createIfMissing) fs::create_directories(result.modsDir, ec);
    if (!fs::is_directory(result.modsDir, ec)) {
        result.diagnostics.push_back("mods directory unavailable: " +
                                     result.modsDir.generic_string());
        return result;
    }

    std::vector<fs::path> folders;
    for (fs::directory_iterator it(result.modsDir, ec), end; !ec && it != end; it.increment(ec)) {
        const auto& entry = *it;
        if (!entry.is_directory(ec)) continue;
        const auto name = entry.path().filename().string();
        if (name.empty() || name.front() == '.' || name.front() == '_') continue;
        folders.push_back(entry.path());
    }
    std::sort(folders.begin(), folders.end(), [](const fs::path& a, const fs::path& b) {
        return a.filename().string() < b.filename().string();
    });

    std::vector<std::string> seenLower;
    for (const auto& dir : folders) {
        const auto folderName = dir.filename().string();
        const auto lower = lowerCopy(folderName);
        if (std::find(seenLower.begin(), seenLower.end(), lower) != seenLower.end()) {
            ModCandidate c;
            c.dir = dir;
            c.folderName = folderName;
            c.status = ModStatus::Error;
            c.validation.error = "duplicate mod name (case-insensitive): " + folderName;
            result.diagnostics.push_back(c.validation.error);
            result.candidates.push_back(std::move(c));
            continue;
        }
        seenLower.push_back(lower);

        const auto manifestPath = dir / "manifest.json";
        if (!fs::is_regular_file(manifestPath, ec)) continue; // silent ignore (§3.2)

        ModCandidate c;
        c.dir = dir;
        c.folderName = folderName;
        c.manifestPath = manifestPath.string();

        std::ifstream in(manifestPath, std::ios::binary);
        if (!in) {
            c.status = ModStatus::Error;
            c.validation.error = "cannot open manifest.json";
            result.candidates.push_back(std::move(c));
            continue;
        }
        try {
            nlohmann::json doc;
            in >> doc;
            c.validation = validateManifest(doc, folderName, dir, gameVersion);
            if (!c.validation.ok)
                c.status = c.validation.incompatible ? ModStatus::Incompatible
                                                     : ModStatus::Error;
            else if (!c.validation.warning.empty())
                result.diagnostics.push_back(folderName + ": " + c.validation.warning);
        } catch (const std::exception& e) {
            c.status = ModStatus::Error;
            c.validation.error = std::string("manifest.json parse error: ") + e.what();
        }
        result.candidates.push_back(std::move(c));
    }
    return result;
}

} // namespace stageb
