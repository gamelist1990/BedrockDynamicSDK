#include "manifest.hpp"

#include <cctype>
#include <regex>

namespace stageb {
namespace {

bool isName(const std::string& s) {
    static const std::regex re(R"(^[A-Za-z0-9][A-Za-z0-9._-]{0,63}$)");
    return std::regex_match(s, re);
}

std::optional<std::string> asString(const nlohmann::json& doc, const char* key) {
    const auto it = doc.find(key);
    if (it == doc.end() || !it->is_string()) return std::nullopt;
    return it->get<std::string>();
}

} // namespace

bool parseSemver(const std::string& text, int& major, int& minor, int& patch) {
    if (text.empty()) return false;
    // Trim an optional leading 'v' for lenient input.
    std::string s = text;
    if (s.size() > 1 && (s[0] == 'v' || s[0] == 'V')) s.erase(s.begin());
    // Ignore prerelease / build metadata for the numeric comparison.
    const auto cut = s.find_first_of("-+");
    if (cut != std::string::npos) s = s.substr(0, cut);
    std::vector<std::string> parts;
    std::size_t start = 0;
    while (true) {
        const auto dot = s.find('.', start);
        parts.push_back(s.substr(start, dot == std::string::npos ? std::string::npos
                                                                 : dot - start));
        if (dot == std::string::npos) break;
        start = dot + 1;
    }
    if (parts.empty() || parts.size() > 3) return false;
    auto number = [](const std::string& p, int& out) {
        if (p.empty() || p.size() > 9) return false;
        for (char c : p) if (!std::isdigit(static_cast<unsigned char>(c))) return false;
        try { out = std::stoi(p); } catch (...) { return false; }
        return true;
    };
    major = minor = patch = 0;
    if (!number(parts[0], major)) return false;
    if (parts.size() > 1 && !number(parts[1], minor)) return false;
    if (parts.size() > 2 && !number(parts[2], patch)) return false;
    return true;
}

ValidationResult validateManifest(const nlohmann::json& doc,
                                  const std::string& folderName,
                                  const fs::path& modDir,
                                  const std::string& gameVersion) {
    ValidationResult r;
    auto fail = [&](std::string message) {
        r.ok = false;
        r.incompatible = false;
        r.error = std::move(message);
        return r;
    };
    auto incompatible = [&](std::string message) {
        r.ok = false;
        r.incompatible = true;
        r.error = std::move(message);
        return r;
    };

    if (!doc.is_object()) return fail("manifest.json must be a JSON object");

    Manifest m;
    auto name = asString(doc, "name");
    if (!name || name->empty()) return fail("missing/invalid field: name");
    if (!isName(*name)) return fail("field name must match ^[A-Za-z0-9][A-Za-z0-9._-]{0,63}$");
    if (*name != folderName)
        return fail("manifest name '" + *name + "' does not match folder '" + folderName + "'");
    m.name = *name;

    auto version = asString(doc, "version");
    if (!version || version->empty()) return fail("missing/invalid field: version");
    m.version = *version;

    // apiVersion: string semver or integer 1 -> "1.0.0" (lenient input).
    const auto apiIt = doc.find("apiVersion");
    if (apiIt == doc.end()) return fail("missing field: apiVersion");
    std::string apiText;
    if (apiIt->is_string()) apiText = apiIt->get<std::string>();
    else if (apiIt->is_number_integer()) apiText = std::to_string(apiIt->get<int>());
    else return fail("field apiVersion must be a semver string or integer");
    int major = 0, minor = 0, patch = 0;
    if (!parseSemver(apiText, major, minor, patch))
        return incompatible("invalid apiVersion: " + apiText);
    if (major != 1)
        return incompatible("apiVersion " + apiText + " is not supported (requires ^1.0.0)");
    m.apiVersion = std::to_string(major) + "." + std::to_string(minor) + "." +
                   std::to_string(patch);

    auto gameIt = doc.find("gameVersions");
    if (gameIt == doc.end() || !gameIt->is_array() || gameIt->empty())
        return fail("missing/invalid field: gameVersions");
    bool gameMatch = false;
    for (const auto& value : *gameIt) {
        if (!value.is_string()) return fail("gameVersions must be an array of strings");
        const auto v = value.get<std::string>();
        m.gameVersions.push_back(v);
        if (v == "*" || v == gameVersion) gameMatch = true;
    }
    if (!gameMatch)
        return incompatible("gameVersions does not include running game " + gameVersion);

    auto entry = asString(doc, "entry");
    if (!entry || entry->empty()) return fail("missing/invalid field: entry");
    const auto ext = fs::path(*entry).extension().generic_string();
    if (ext != ".ts" && ext != ".js" && ext != ".mjs")
        return fail("entry must end with .ts, .js or .mjs");
    auto resolved = confinedResolve(modDir, *entry);
    if (!resolved) return fail("entry escapes the mod directory");
    std::error_code ec;
    if (!fs::is_regular_file(*resolved, ec))
        return fail("entry file not found: " + *entry);
    m.entry = resolved->generic_string();

    if (auto description = asString(doc, "description")) m.description = *description;
    if (auto displayName = asString(doc, "displayName")) m.displayName = *displayName;
    if (m.displayName.empty()) m.displayName = m.name;

    if (const auto memIt = doc.find("memoryMiB"); memIt != doc.end()) {
        if (!memIt->is_number_integer())
            r.warning = "memoryMiB is not an integer; using default 64";
        else {
            const auto mem = memIt->get<long long>();
            if (mem < 8 || mem > 1024)
                r.warning = "memoryMiB out of range 8-1024; using default 64";
            else
                m.memoryMiB = static_cast<unsigned>(mem);
        }
    }

    r.ok = true;
    r.manifest = std::move(m);
    return r;
}

} // namespace stageb
