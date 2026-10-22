#pragma once
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace bedrock {

struct GameIdentity;

struct Resolution {
    std::uintptr_t address = 0;
    std::string method;
    bool exact_build = false;
    bool verified = false;
};

class Resolver {
public:
    // Versioned data describes RVAs in the *mapped PE image* (not file offsets).
    bool loadJson(std::string_view json, std::string& error);
    // moduleBase must point at the full mapped PE image; text region describes the live image.
    bool attachImage(const std::byte* moduleBase, std::size_t mappedSize,
                     std::string actualFileSha256, std::uint32_t textRva,
                     std::size_t textSize, std::string& error);
    // Fail-closed default: no returned candidate without an explicit opt-in.
    // Alternative identity check for GDK/Proton environments where on-disk
    // Minecraft.Windows.exe is encrypted but the loaded .text mapping is readable.
    // The hash must be computed from mapped bytes matching target .text_size.
    bool attachImageByTextHash(const std::byte* moduleBase, std::size_t mappedSize,
                               std::string actualTextSha256, std::uint32_t textRva,
                               std::size_t textSize, std::string& error);
    std::optional<Resolution> resolve(std::string_view name,
                                      bool allowCandidates = false) const;
    [[nodiscard]] std::size_t count() const noexcept { return entries_.size(); }
    [[nodiscard]] bool exactBuild() const noexcept { return exactBuild_; }
#ifdef _WIN32
    // Uses Windows system APIs only; no PreLoader dependency.
    bool attachWindowsMainModule(std::string& error);
    // Reuses the live-module fingerprint produced by inspectLoadedGame.
    bool attachWindowsMainModule(const GameIdentity& identity,
                                 std::string& error);
#endif

private:
    struct BytePattern {
        std::vector<std::uint8_t> bytes;
        std::vector<std::uint8_t> mask;
    };
    struct Entry {
        std::uint32_t rva{};
        bool verified{};
        BytePattern signature;
        BytePattern entryCheck;
    };
    std::unordered_map<std::string, Entry> entries_;
    std::string expectedHash_;
    std::string expectedTextHash_;
    std::size_t targetTextSize_{};
    std::uint32_t targetSize_{};
    std::uint32_t targetTextRva_{};
    const std::byte* image_{};
    std::size_t imageSize_{};
    std::uint32_t textRva_{};
    std::size_t textSize_{};
    bool exactBuild_{};
    static BytePattern parsePattern(std::string_view);
    static bool matches(const std::byte*, const BytePattern&);
    static std::optional<std::size_t> findUnique(std::span<const std::byte>, const BytePattern&);
};

} // namespace bedrock
