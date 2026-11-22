#pragma once
#include <bedrock/sdk/events.hpp>
#include <functional>

// Sample-owned input snapshot event. Not a BedrockDynamicSDK core dependency.
// A complete state every frame avoids lost KeyEvent edges.
struct InputFrameEvent {
    bool minecraftFocused{};
    bool gameplayCursorHidden{};
    bool forwardHeld{};
    bool shiftHeld{};
    bool leftCtrlHeld{};

    [[nodiscard]] bool shouldSprint() const noexcept {
        return minecraftFocused && gameplayCursorHidden && forwardHeld && !shiftHeld;
    }
};

// Public SDK event signal (EventSignal<const InputFrameEvent&>), replacing the
// former bedrock::EventBus/EventChannel dependency. Has subscribe()/publish()
// but NO listeners() query.
using AutoSprintEvents = bedrock::sdk::Events<InputFrameEvent>;

class AutoSprintFeature {
public:
    using SendSprint = std::function<bool(bool)>;
    explicit AutoSprintFeature(AutoSprintEvents& events, SendSprint send);
    ~AutoSprintFeature();

    AutoSprintFeature(const AutoSprintFeature&) = delete;
    AutoSprintFeature& operator=(const AutoSprintFeature&) = delete;

    [[nodiscard]] bool ownsSprint() const noexcept;
private:
    void onInputFrame(const InputFrameEvent& frame);
    void release();
    AutoSprintEvents& events_;
    bedrock::sdk::Subscription subscription_;
    SendSprint send_;
    bool ownSprint_{};
};