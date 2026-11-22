#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "block_types.hpp"
#include "profiles.hpp"
#include "world_access.hpp"

namespace bedrock::sdk {

// Why a targeted block action did not go through. Mirrors the native
// BlockActionFailure values one to one.
enum class BlockActionFailureValue {
    None,
    NotLoaded,
    InvalidFace,
    GameModeUnavailable,
    VirtualFunctionMismatch,
    HitContextUnavailable,
    NativeFault,
    GameRejected
};

// Outcome detail of one mine() call. Mirrors the native BlockActionReport.
struct BlockActionReportValue {
    BlockActionFailureValue failure{BlockActionFailureValue::None};
    unsigned virtualSlot{};
    std::uint32_t observedFunctionRva{};
    bool called{}, accepted{};
    float progressBefore{};
    std::uint64_t idleStopsSuppressed{};
};

// --------------------------------------------------------------------------
// BlockActions — public facade over the native targeted GameMode actions.
// The game's own survival break timer, selected tool, reach checks and
// transaction path remain authoritative; the facade only invokes the same
// virtuals a real targeted input would. Requires the WorldAccess it was
// loaded against (same instance the Mod reads the world through).
// --------------------------------------------------------------------------
class BlockActions final {
public:
    BlockActions();
    ~BlockActions();
    BlockActions(const BlockActions&) = delete;
    BlockActions& operator=(const BlockActions&) = delete;
    BlockActions(BlockActions&&) noexcept;
    BlockActions& operator=(BlockActions&&) noexcept;

    // Bind the profile's block-action functions and the world accessor.
    bool load(const Profile& profile, WorldAccess& world, std::string& error);

    // Begin or continue destroying `block` through the targeted hit
    // context. `destroyed` reports the engine's own completion flag; the
    // caller must still observe the block disappearing.
    bool mine(void* player, BlockPos block, unsigned face, Vec3Value eye, Vec3Value hit,
              bool begin, bool& destroyed, BlockActionReportValue* report = nullptr) const noexcept;

    // Read the engine-owned destroy target and real progress.
    bool destroyState(void* player, float& progress, BlockPos& target) const noexcept;

    // Stop destroying `block` and release the mining lease.
    bool stop(void* player, BlockPos block) const noexcept;

    // Use / release the held item, or attack an actor, through GameMode.
    bool useItem(void* player) const noexcept;
    bool releaseItem(void* player) const noexcept;
    bool attackActor(void* player, void* target) const noexcept;

    // Physical input and explicit cancellation relinquish the mining
    // lease immediately.
    void cancelMiningOwnership() const noexcept;

    // Human-readable one-line summary of a report (same text as native).
    static std::string diagnostic(const BlockActionReportValue& report);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace bedrock::sdk
