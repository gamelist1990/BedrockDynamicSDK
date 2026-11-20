#pragma once

#include <chrono>
#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace stageb {

namespace fs = std::filesystem;

// §8.4 Mod state machine. String values are the ones shown by /mods list.
enum class ModStatus {
    Scanned,
    Loading,
    Active,
    Faulted,
    Incompatible,
    Error,
};

inline const char* statusName(ModStatus s) noexcept {
    switch (s) {
        case ModStatus::Scanned:      return "scanned";
        case ModStatus::Loading:      return "loading";
        case ModStatus::Active:       return "ok";
        case ModStatus::Faulted:      return "faulted";
        case ModStatus::Incompatible: return "incompatible";
        case ModStatus::Error:        return "error";
    }
    return "error";
}

struct StageError : std::runtime_error {
    std::string code;
    StageError(std::string c, std::string message)
        : std::runtime_error(std::move(message)), code(std::move(c)) {}
};

// §5.1 frozen ErrorCode string set used as JS exception `code`.
inline const char* errorCodeName(int code) noexcept {
    static const char* names[] = {"Invalid", "AlreadyRegistered", "NotFound",
                                  "WrongType", "Storage", "Unavailable",
                                  "BufferTooSmall", "Internal"};
    return (code >= 0 && code < 8) ? names[code] : "Internal";
}

// Resolve a path against a base directory, rejecting any attempt to escape
// the base via `..`. Returns nullopt on escape. Used by require resolution and
// manifest entry checks (§3.3, §4.2, §12).
inline std::optional<fs::path> confinedResolve(const fs::path& base,
                                                const fs::path& relative) {
    std::error_code ec;
    auto root = fs::weakly_canonical(base, ec);
    if (ec) root = fs::absolute(base);
    auto target = fs::weakly_canonical(root / relative, ec);
    if (ec) target = fs::absolute(root / relative);
    auto rootStr = root.generic_string();
    auto targetStr = target.generic_string();
    if (targetStr != rootStr &&
        targetStr.rfind(rootStr + "/", 0) != 0)
        return std::nullopt;
    return target;
}

inline std::string lowerCopy(std::string s) {
    for (char& c : s)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return s;
}

using Clock = std::chrono::steady_clock;

} // namespace stageb
