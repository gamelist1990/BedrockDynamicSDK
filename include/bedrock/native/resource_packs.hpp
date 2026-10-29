#pragma once
#include <bedrock/profiles/profile_manager.hpp>
#include <cstdint>
#include <string>

namespace bedrock {
// Windows x64 Release MSVC adapter. Native entries, flags and string ABI are
// supplied by the exact executable profile, never by the consuming mod.
class NativeResourcePacks final {
  public:
    struct Status {
        bool installed{};
        std::uint64_t settingsOpened{}, preGameOverrides{}, failures{};
        std::uint64_t settingsExited{}, applyEvents{}, reloadCalls{}, stackCompositions{},
            reloadFinalized{}, globalStackIncluded{};
        std::uint64_t applyNavigations{}, liveApplications{}, inWorldApplyEvents{}, gameGateOverrides{};
    };
    NativeResourcePacks();
    ~NativeResourcePacks();
    NativeResourcePacks(const NativeResourcePacks &) = delete;
    NativeResourcePacks &operator=(const NativeResourcePacks &) = delete;
    bool load(const RuntimeProfile &profile, std::string &error);
    bool install(std::string &error);
    Status status() const noexcept;

  private:
    struct Binding;
    Binding *binding_{};
};
} // namespace bedrock
