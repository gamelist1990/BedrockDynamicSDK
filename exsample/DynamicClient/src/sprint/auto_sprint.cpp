#include "auto_sprint_feature.hpp"

AutoSprintFeature::AutoSprintFeature(bedrock::sdk::Events<InputFrameEvent>& events, SendSprint send)
    : events_{events}, send_{std::move(send)} {
    subscription_ = events_.subscribe([this](const InputFrameEvent& frame) { onInputFrame(frame); });
}

AutoSprintFeature::~AutoSprintFeature() {
    subscription_.reset();
    release();
}

[[nodiscard]] bool AutoSprintFeature::ownsSprint() const noexcept { return ownSprint_; }

void AutoSprintFeature::onInputFrame(const InputFrameEvent& frame) {
    // Input snapshots are emitted only when Minecraft's own key or cursor
    // state changes; this keeps the feature event-driven and avoids polling.
    if (frame.shouldSprint()) {
        if (frame.leftCtrlHeld) {
            // The physical key has taken ownership of Ctrl. Do not release it
            // when the sprint condition later becomes false.
            ownSprint_ = false;
        } else if (!ownSprint_ && send_ && send_(true)) {
            ownSprint_ = true;
        }
    } else {
        if (ownSprint_ && frame.leftCtrlHeld)
            ownSprint_ = false;
        else
            release();
    }
}

void AutoSprintFeature::release() {
    if (!ownSprint_) return;
    ownSprint_ = false;
    if (send_) send_(false);
}