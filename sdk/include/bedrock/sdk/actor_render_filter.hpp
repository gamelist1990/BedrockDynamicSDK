#pragma once

#include "profiles.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace bedrock::sdk {

// Thread-safe policy consulted on the render thread. For the per-actor queue
// surface, `object` is the actor key produced by ActorFacade capture.
//
// The callback must be lock-free, non-blocking and must not allocate: it runs
// once per object of the watched extraction passes, every frame.
using ActorRenderPolicy = bool (*)(void* object) noexcept;

struct ActorRenderFilterStatistics {
    bool ready{};
    unsigned calls{};      // predicate calls observed on the watched passes
    unsigned permitted{};  // calls the policy allowed
    unsigned excluded{};   // calls the policy excluded
    unsigned matches{};    // policy matches, including in logging-only mode
    unsigned long long variants{};  // bit i = variant i observed since the last call
};

// One distinct object seen by the diagnostic probe (no exclusion performed).
// `words` and `contextWords` are a bounded read-only copy of the object's and
// the render context's first quadwords, for identifying what the pass iterates.
struct ActorRenderProbeSample {
    static constexpr unsigned kSampleWords = 32;
    static constexpr unsigned kContextWords = 8;
    std::uintptr_t table{};
    std::uintptr_t object{};
    std::uintptr_t context{};
    std::uintptr_t words[kSampleWords]{};
    std::uintptr_t contextWords[kContextWords]{};
    unsigned calls{};
};

// Per-actor render skip through a profiled per-actor submission callback.
//
// The profile definition names the shared predicate and the callable tables of
// the watched surface. Install verifies the live
// table slots and the type getters before it detours anything; a mismatch
// leaves the game untouched and returns an error. The legacy RenderDragon
// extraction probe remains diagnostic-only when actor identity is unresolved.
class ActorRenderFilter {
public:
    ActorRenderFilter();
    ~ActorRenderFilter();
    ActorRenderFilter(const ActorRenderFilter&) = delete;
    ActorRenderFilter& operator=(const ActorRenderFilter&) = delete;
    ActorRenderFilter(ActorRenderFilter&&) noexcept;
    ActorRenderFilter& operator=(ActorRenderFilter&&) noexcept;

    // Diagnostics-only mode: no exclusion, records the objects each watched
    // pass asks about so the Mod can identify them. Must be called before
    // configure().
    void setProbe(bool enabled) noexcept;
    [[nodiscard]] bool probe() const noexcept;

    bool configure(const Profile& profile, ActorRenderPolicy policy, std::string& error,
                   std::vector<std::string>& warnings);
    [[nodiscard]] bool ready() const noexcept;
    ActorRenderFilterStatistics takeStatistics() noexcept;
    std::vector<ActorRenderProbeSample> takeProbeSamples();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace bedrock::sdk
