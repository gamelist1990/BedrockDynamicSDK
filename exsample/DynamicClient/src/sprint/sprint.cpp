#include "client.hpp"
#include "auto_sprint_feature.hpp"
#include <bedrock/runtime/game_hooks.hpp>
#include <algorithm>
#include <array>
#include <atomic>
#include <mutex>

namespace dynamic_client {
namespace {
bedrock::detail::HookDispatch api;
bedrock::GameHooks hooks{api};
AutoSprintEvents events;
constexpr unsigned ctrl = 0x11;
constexpr std::array<unsigned, 5> shiftKeys{0x10, 0xA0, 0xA1, 340, 344};
constexpr std::array<unsigned, 5> ctrlKeys{ctrl, 0xA2, 0xA3, 341, 345};
std::atomic<bool> enabled{true};
std::recursive_mutex mutex;
std::array<bool, shiftKeys.size()> shiftHeld{}, ctrlHeld{};
bool forward{}, gameplay{};
AutoSprintFeature feature{events, [](bool down) { return api.sendNativeKey(ctrl, down); }};

void dispatch() {
    events.emit(InputFrameEvent{gameplay, gameplay && enabled.load(), forward,
                                std::ranges::any_of(shiftHeld, std::identity{}),
                                std::ranges::any_of(ctrlHeld, std::identity{})});
}
}

void setSprintEnabled(bool value) noexcept { enabled.store(value); }

void startSprint(const bedrock::RuntimeProfile& profile, bedrock::Logger& logger) {
    std::string error;
    if (!profile.supports({bedrock::Feature::Keyboard, bedrock::Feature::Gameplay,
                          bedrock::Feature::LevelTick}, error))
        throw std::runtime_error(error);
    api.on<bedrock::NativeKeyEvent>([](const auto& event) {
        std::lock_guard lock(mutex);
        if (event.inputKey == 'W') forward = event.pressed;
        for (std::size_t i = 0; i < shiftKeys.size(); ++i)
            if (event.inputKey == shiftKeys[i]) shiftHeld[i] = event.pressed;
        for (std::size_t i = 0; i < ctrlKeys.size(); ++i)
            if (event.inputKey == ctrlKeys[i]) ctrlHeld[i] = event.pressed;
        dispatch();
    });
    api.on<bedrock::GameplayEvent>([](const auto& event) {
        std::lock_guard lock(mutex);
        gameplay = event.active;
        dispatch();
        if (!gameplay) {
            forward = false;
            shiftHeld.fill(false);
            ctrlHeld.fill(false);
        }
    });
    api.on<bedrock::LevelTickEvent>([](const auto&) {
        std::lock_guard lock(mutex);
        if (feature.ownsSprint() && !enabled.load()) dispatch();
    });
    if (!hooks.installNativeKeyboard(profile.resolver(), profile.json(), error) ||
        !hooks.installGameplay(profile.resolver(), error) ||
        !hooks.installLevelTick(profile.resolver(), error)) {
        hooks.stop();
        throw std::runtime_error(error);
    }
    logger.info("AutoSprint ready");
}
}
