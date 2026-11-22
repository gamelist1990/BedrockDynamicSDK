#include "bedrock/sdk/automation_controls.hpp"

#include "profile_facade.hpp"

#ifdef _WIN32
#include <bedrock/detail/automation_motion.hpp>
#include <bedrock/native/automation_controls.hpp>
#endif

namespace bedrock::sdk {

struct AutomationControls::Impl {
#ifdef _WIN32
    bedrock::NativeAutomationControls native;
#endif
};

AutomationControls::AutomationControls() : impl_(std::make_unique<Impl>()) {}
AutomationControls::~AutomationControls() = default;
AutomationControls::AutomationControls(AutomationControls&&) noexcept = default;
AutomationControls& AutomationControls::operator=(AutomationControls&&) noexcept = default;

bool AutomationControls::load(const Profile& profile, std::string& error) {
#ifdef _WIN32
    const auto runtime = ProfileRuntimeAccess::runtime(profile);
    if (!runtime) {
        error = "automation profile runtime snapshot unavailable";
        return false;
    }
    return impl_->native.load(*runtime, error);
#else
    (void)profile;
    error = "automation controls unavailable on this host";
    return false;
#endif
}

void AutomationControls::stop() noexcept {
#ifdef _WIN32
    impl_->native.stop();
#endif
}

void AutomationControls::update(void* client, std::uint64_t epoch,
                                const AutomationIntentValue& intent, float cameraYaw,
                                bool manualOverride) {
#ifdef _WIN32
    bedrock::AutomationIntent native;
    native.forward = intent.forward;
    native.left = intent.left;
    native.yaw = intent.yaw;
    native.pitch = intent.pitch;
    native.jump = intent.jump;
    native.sneak = intent.sneak;
    native.sprint = intent.sprint;
    native.active = intent.active;
    native.look = intent.look;
    native.sprintJump = intent.sprintJump;
    impl_->native.update(client, epoch, native, cameraYaw, manualOverride);
#else
    (void)client;
    (void)epoch;
    (void)intent;
    (void)cameraYaw;
    (void)manualOverride;
#endif
}

void AutomationControls::clear() noexcept {
#ifdef _WIN32
    impl_->native.clear();
#endif
}

void AutomationControls::manualInput(unsigned channel, bool down) noexcept {
#ifdef _WIN32
    impl_->native.manualInput(channel, down);
#else
    (void)channel;
    (void)down;
#endif
}

void AutomationControls::clearManualInput() noexcept {
#ifdef _WIN32
    impl_->native.clearManualInput();
#endif
}

bool AutomationControls::manualActive() const noexcept {
#ifdef _WIN32
    return impl_->native.manualActive();
#else
    return false;
#endif
}

bool AutomationControls::applied() const noexcept {
#ifdef _WIN32
    return impl_->native.applied();
#else
    return false;
#endif
}

AutomationInputObservationValue AutomationControls::observation() const {
#ifdef _WIN32
    const auto native = impl_->native.observation();
    AutomationInputObservationValue result;
    result.sequence = native.sequence;
    result.sprintCalls = native.sprintCalls;
    result.applied = native.applied;
    result.sprintJump = native.sprintJump;
    result.forward = native.forward;
    result.left = native.left;
    result.yaw = native.yaw;
    result.input = native.input;
    result.held = native.held;
    result.sprintMask = native.sprintMask;
    return result;
#else
    return {};
#endif
}

} // namespace bedrock::sdk
