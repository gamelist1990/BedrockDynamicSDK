#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "profiles.hpp"

namespace bedrock::sdk {

// One automation intent for the independent (non-interfering) movement
// override. Mirrors the native AutomationIntent field for field; the
// override borrows the player's input component around a single engine
// simulation call — no keyboard feed, cursor capture, velocity write or
// position write is involved.
struct AutomationIntentValue {
    float forward{}, left{}, yaw{}, pitch{};
    bool jump{}, sneak{}, sprint{}, active{}, look{};
    // Sprint-jump locomotion needs engine-forward input and a boost along
    // yaw, not camera-relative strafing; the native adapter borrows actor
    // yaw for simulation only.
    bool sprintJump{};
};

// What the last simulation override actually applied, for diagnostics and
// for counting real sprint-jump takeoffs.
struct AutomationInputObservationValue {
    unsigned long long sequence{}, sprintCalls{};
    bool applied{}, sprintJump{};
    float forward{}, left{}, yaw{};
    unsigned input{}, held{}, sprintMask{};
};

// --------------------------------------------------------------------------
// AutomationControls — public facade over the native independent-input
// automation. The Mod publishes an intent (update) and the facade applies
// it inside the engine's simulation tick; physical (manual) input always
// wins while it is active, so the user can grab control back at any time.
// --------------------------------------------------------------------------
class AutomationControls final {
public:
    AutomationControls();
    ~AutomationControls();
    AutomationControls(const AutomationControls&) = delete;
    AutomationControls& operator=(const AutomationControls&) = delete;
    AutomationControls(AutomationControls&&) noexcept;
    AutomationControls& operator=(AutomationControls&&) noexcept;

    // Parse the automation layout from the selected profile and install
    // the simulation + rotation-wire hooks.
    bool load(const Profile& profile, std::string& error);

    // Unsubscribe and drop any published intent.
    void stop() noexcept;

    // Publish the intent for the current world epoch. Intents go stale
    // after ~500 ms, so callers re-publish every tick.
    void update(void* client, std::uint64_t epoch, const AutomationIntentValue& intent,
                float cameraYaw, bool manualOverride = false);

    // Drop the published intent and the player context.
    void clear() noexcept;

    // Record physical input on a channel (0-31) so automation yields.
    void manualInput(unsigned channel, bool down) noexcept;
    // UI typing/clicks are not gameplay input, including key-up events.
    void clearManualInput() noexcept;
    bool manualActive() const noexcept;

    bool applied() const noexcept;
    AutomationInputObservationValue observation() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace bedrock::sdk
