#include "bedrock/remote_catalog.hpp"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cctype>
#include <exception>
#include <regex>

namespace bedrock {
namespace {
std::string lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
        [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return value;
}
bool validHash(const std::string& value) {
    if (value.size() != 64) return false;
    return std::all_of(value.begin(), value.end(),
        [](unsigned char ch) { return std::isxdigit(ch) != 0; });
}
bool validVersion(const std::string& v) {
    static const std::regex version(R"(^[0-9]{1,5}(\.[0-9]{1,5}){2,4}$)");
    return std::regex_match(v, version);
}
bool matchingIdentity(const nlohmann::json& data,
                      const GameIdentity& identity, std::string& error) {
    const std::string file = lower(data.at("module_sha256").get<std::string>());
    const std::string text = lower(data.at("module_text_sha256").get<std::string>());
    const auto image = data.at("size_of_image").get<std::uint32_t>();
    const auto textRva = data.at("text_rva").get<std::string>();
    const std::uint32_t rv = static_cast<std::uint32_t>(std::stoul(textRva,nullptr,0));
    const auto textSize = data.at("text_size").get<std::uint32_t>();
    if (!validHash(file) || !validHash(text)) return false;
    if (image != identity.imageSize || rv != identity.textRva ||
        textSize != identity.textSize) return false;
    const bool exactFile = validHash(identity.fileSha256) &&
                           file == lower(identity.fileSha256);
    const bool exactText = validHash(identity.textSha256) &&
                           text == lower(identity.textSha256);
    if (!(exactText || exactFile)) {
        error = "unsupported executable hash";
        return false;
    }
    return true;
}
}

std::optional<ProfileChoice> chooseProfile(
    std::string_view manifest, const GameIdentity& identity, std::string& error) {
    try {
        const auto root = nlohmann::json::parse(manifest.begin(), manifest.end());
        if (root.at("schema").get<int>() != 1 || !root.at("versions").is_object())
            throw std::invalid_argument("unsupported manifest schema");
        for (auto it = root.at("versions").begin(); it != root.at("versions").end(); ++it) {
            const std::string version = it.key();
            if (!validVersion(version) || !it.value().is_object() ||
                !it.value().contains("windows-x64")) continue;
            const auto& row = it.value().at("windows-x64");
            const auto expectedPath = "data/" + version + "/windows-x64.json";
            const std::string path = row.at("path").get<std::string>();
            const std::string checksum = lower(row.at("sha256").get<std::string>());
            if (path != expectedPath || !validHash(checksum)) continue;
            std::string reject;
            if (matchingIdentity(row, identity, reject)) {
                error.clear();
                return ProfileChoice{version, path, checksum};
            }
        }
        error = "unsupported game build: no exact matching version in manifest";
    } catch (const std::exception& e) {
        error = std::string("invalid manifest: ") + e.what();
    }
    return std::nullopt;
}

bool validateDownloadedProfile(std::string_view profile,
                               const ProfileChoice& choice,
                               const GameIdentity& identity, std::string& error) {
    try {
        const auto doc = nlohmann::json::parse(profile.begin(), profile.end());
        const auto& target = doc.at("target");
        if (doc.at("schema") != 1 || target.at("product") != "minecraft-bedrock" ||
            target.at("kind") != "client" || target.at("arch") != "x64" ||
            target.at("platform") != "windows" ||
            target.at("version") != choice.version) {
            error = "profile target is not this Windows client version";
            return false;
        }
        const auto imageSize = target.at("size_of_image").get<std::uint32_t>();
        const std::uint32_t rva = static_cast<std::uint32_t>(
            std::stoul(target.at("text_rva").get<std::string>(), nullptr, 0));
        const auto textSize = target.at("text_size").get<std::uint32_t>();
        if (imageSize != identity.imageSize || rva != identity.textRva ||
            textSize != identity.textSize) {
            error = "profile PE geometry mismatch";
            return false;
        }
        const auto file = lower(target.at("sha256").get<std::string>());
        const auto text = lower(target.at("text_sha256").get<std::string>());
        if (!(validHash(file) && validHash(text) &&
              ((file == lower(identity.fileSha256) && validHash(identity.fileSha256)) ||
               (text == lower(identity.textSha256) && validHash(identity.textSha256))))) {
            error = "profile executable hash mismatch";
            return false;
        }
        if (!doc.at("symbols").is_object()) throw std::invalid_argument("symbols not object");
        error.clear();
        return true;
    } catch (const std::exception& e) {
        error = std::string("invalid profile: ") + e.what();
        return false;
    }
}
} // namespace bedrock
