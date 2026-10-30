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
        std::uint64_t settingsExited{}, applyEvents{}, stackCompositions{},
            globalStackIncluded{};
        std::uint64_t applyNavigations{}, liveApplications{}, inWorldApplyEvents{}, gameGateOverrides{};
        std::uint64_t terrainAtlasCompleted{}, refreshCancelled{}, resourceWaitFrames{};
        std::uint64_t platformCycles{}, blockGraphicsRebuilt{}, graphicsCacheResets{}, camerasRefreshed{}, remoteReloads{}, suspendGateOverrides{};
        std::uint64_t geometryReloaded{}, clientDefinitionsReloaded{};
        std::uint64_t customMaterialsRemapped{}, customUvSetsUpdated{}, customUvMismatched{}, customUvUnmatched{}, customUvUnchanged{};
        std::uint64_t nativeScreenWaitFrames{}, previousReloadWaitFrames{};
    };
    NativeResourcePacks();
    ~NativeResourcePacks();
    NativeResourcePacks(const NativeResourcePacks &) = delete;
    NativeResourcePacks &operator=(const NativeResourcePacks &) = delete;
    bool load(const RuntimeProfile &profile, std::string &error);
    bool install(std::string &error);
    using TraceCallback = void (*)(const char *) noexcept;
    void setTraceCallback(TraceCallback callback) noexcept;
    Status status() const noexcept;

  private:
    struct Binding;
    Binding *binding_{};
};
} // namespace bedrock
