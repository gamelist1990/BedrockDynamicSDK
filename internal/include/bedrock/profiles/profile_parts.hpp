#pragma once
#include <nlohmann/json.hpp>
#include <functional>
#include <regex>
#include <stdexcept>
#include <string>
#include <string_view>

namespace bedrock {
// Transport authenticates each returned part with the expected SHA256.
// Assembly does not overwrite duplicates and admits only manifest-local paths.
inline std::string assembleProfileParts(
    std::string_view descriptor, std::string_view manifestPath,
    const std::function<std::string(const std::string &, const std::string &)> &fetch) {
    using Json = nlohmann::json;
    const auto manifest = Json::parse(descriptor);
    if (manifest.value("format", std::string{}) != "bedrock-profile-parts")
        return std::string(descriptor);
    const auto slash = manifestPath.find_last_of('/');
    if (slash == std::string_view::npos)
        throw std::runtime_error("profile manifest parent missing");
    const std::string parent(manifestPath.substr(0, slash + 1));
    const auto &parts = manifest.at("parts");
    if (!parts.is_array() || parts.size() > 256)
        throw std::runtime_error("profile part count limit");
    Json root = {{"schema", manifest.at("schema")}, {"target", manifest.at("target")}};
    if (manifest.contains("protocol"))
        root["protocol"] = manifest.at("protocol");
    const std::regex pathRule(R"(^(classes|analysis|metadata)/[A-Za-z0-9_.-]+\.json$)");
    const std::regex hashRule("^[a-f0-9]{64}$");
    std::size_t total{};
    for (const auto &entry : parts) {
        const auto path = entry.at("path").get<std::string>();
        const auto hash = entry.at("sha256").get<std::string>();
        if (!std::regex_match(path, pathRule) || !std::regex_match(hash, hashRule))
            throw std::runtime_error("invalid profile part path/hash");
        // Historical REA observations are available in data; they are not needed
        // for runtime resolution and do not delay the class definition fetches.
        if (path.starts_with("analysis/"))
            continue;
        const auto raw = fetch(parent + path, hash);
        total += raw.size();
        if (raw.size() > 4 * 1024 * 1024 || total > 16 * 1024 * 1024)
            throw std::runtime_error("profile part byte limit");
        const auto fragment = Json::parse(raw);
        if (!fragment.is_object())
            throw std::runtime_error("profile part must be an object");
        for (auto it = fragment.begin(); it != fragment.end(); ++it) {
            const auto key = it.key();
            if (key == "symbols" || key == "layouts" || key == "virtual_methods") {
                if (!it.value().is_object())
                    throw std::runtime_error("invalid profile section");
                if (!root.contains(key))
                    root[key] = Json::object();
                for (auto item = it.value().begin(); item != it.value().end(); ++item) {
                    if (root[key].contains(item.key()))
                        throw std::runtime_error("duplicate profile definition: " + key + "/" +
                                                 item.key());
                    root[key][item.key()] = item.value();
                }
            } else {
                if (root.contains(key) || key == "parts" || key == "format" || key == "analysis")
                    throw std::runtime_error("duplicate/invalid profile metadata: " + key);
                root[key] = it.value();
            }
        }
    }
    if (!root.contains("symbols"))
        root["symbols"] = Json::object();
    return root.dump();
}
} // namespace bedrock
