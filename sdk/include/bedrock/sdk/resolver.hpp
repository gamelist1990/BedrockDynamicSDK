#pragma once

#include "profiles.hpp"
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace bedrock::sdk {

struct SymbolResolverRuntimeAccess;

struct SymbolResolution {
    std::uintptr_t rva{};
    std::uintptr_t address{};
    bool exactBuild{};
    bool verified{};
};

class SymbolResolver {
public:
    SymbolResolver();
    ~SymbolResolver();

    bool open(const Profile& profile, std::string& error);
    bool openFixture(std::string_view profileJson, std::string& error);

    [[nodiscard]] bool exactBuild() const noexcept;
    [[nodiscard]] std::optional<SymbolResolution> resolve(std::string_view name) const;
    [[nodiscard]] std::optional<SymbolResolution> resolveCandidate(std::string_view name) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    friend struct SymbolResolverRuntimeAccess;
};

} // namespace bedrock::sdk
