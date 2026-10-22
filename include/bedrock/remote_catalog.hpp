#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace bedrock {
// Fingerprint of the executable actually loaded, never a reported version label.
// A GDK/Proton packaged EXE may be encrypted on disk; textSha256 is from mapped code.
struct GameIdentity {
    std::string fileSha256;
    std::string textSha256;
    std::uint32_t imageSize{};
    std::uint32_t textRva{};
    std::uint32_t textSize{};
};

enum class ProfileSource : std::uint8_t {
    Remote,
    Cache,
    Embedded,
};

struct ProfileChoice {
    std::string version;
    std::string path;
    std::string contentSha256;
    ProfileSource source{ProfileSource::Remote};
};

constexpr std::string_view profileSourceName(ProfileSource source) noexcept {
    switch (source) {
    case ProfileSource::Remote: return "remote";
    case ProfileSource::Cache: return "cache";
    case ProfileSource::Embedded: return "embedded";
    }
    return "unknown";
}

std::optional<ProfileChoice> chooseProfile(
    std::string_view manifest, const GameIdentity& identity,
    std::string& error);

// Returns whether a downloaded profile *really is* the same module identity and
// complete expected file. Matching external JSON alone is not enough to hook.
bool validateDownloadedProfile(std::string_view profile,
                               const ProfileChoice& choice,
                               const GameIdentity& identity,
                               std::string& error);

#ifdef _WIN32
bool inspectLoadedGame(GameIdentity& identity, std::string& error);
bool fetchSupportedProfile(std::string& profileJson,
                           ProfileChoice& choice, std::string& error);
#endif
} // namespace bedrock
