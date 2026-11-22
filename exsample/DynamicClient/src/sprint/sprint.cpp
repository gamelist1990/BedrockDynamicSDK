#include "client.hpp"
#include "auto_sprint_feature.hpp"
#include <bedrock/sdk/game_hooks.hpp>
#include <algorithm>
#include <array>
#include <atomic>
#include <functional>
#include <mutex>

namespace dynamic_client {
namespace {
using bedrock::sdk::Hooks;
using bedrock::sdk::GameplayEventValue;
using bedrock::sdk::LevelTickEventValue;
using bedrock::sdk::NativeKeyEventValue;

Hooks hooks;
AutoSprintEvents events;
constexpr unsigned ctrl = 0x11;
constexpr std::array<unsigned, 5> shiftKeys{0x10, 0xA0, 0xA1, 340, 344};
constexpr std::array<unsigned, 5> ctrlKeys{ctrl, 0xA2, 0xA3, 341, 345};
std::atomic<bool> enabled{true};
std::recursive_mutex mutex;
std::array<bool, shiftKeys.size()> shiftHeld{}, ctrlHeld{};
bool forward{}, gameplay{};

bool sendSprint(bool down) { return hooks.sendNativeKey(ctrl, down); }

AutoSprintFeature feature{events, sendSprint};

void dispatch() {
    events.publish(InputFrameEvent{gameplay, gameplay && enabled.load(), forward,
                                   std::ranges::any_of(shiftHeld, std::identity{}),
                                   std::ranges::any_of(ctrlHeld, std::identity{})});
}

void onNativeKeyHandler(const NativeKeyEventValue& ev) {
    std::lock_guard<std::recursive_mutex> lock(mutex);
    if (ev.inputKey == 'W') forward = ev.pressed;
    for (std::size_t i = 0; i < shiftKeys.size(); ++i)
        if (ev.inputKey == shiftKeys[i]) shiftHeld[i] = ev.pressed;
    for (std::size_t i = 0; i < ctrlKeys.size(); ++i)
        if (ev.inputKey == ctrlKeys[i]) ctrlHeld[i] = ev.pressed;
    dispatch();
}

void onGameplayHandler(const GameplayEventValue& ev) {
    std::lock_guard<std::recursive_mutex> lock(mutex);
    gameplay = ev.active;
    dispatch();
    if (!gameplay) {
        forward = false;
        shiftHeld.fill(false);
        ctrlHeld.fill(false);
    }
}

void onLevelTickHandler(const LevelTickEventValue&) {
    std::lock_guard<std::recursive_mutex> lock(mutex);
    if (feature.ownsSprint() && !enabled.load()) dispatch();
}
} // namespace

void setSprintEnabled(bool value) noexcept { enabled.store(value); }

void startSprint(const bedrock::sdk::Profile& profile, const bedrock::sdk::SymbolResolver& resolver,
                 bedrock::sdk::Logger& logger) {
    std::string error;
    
    hooks.onNativeKey([](const bedrock::sdk::NativeKeyEventValue& ev) { onNativeKeyHandler(ev); });
    hooks.onGameplay([](const bedrock::sdk::GameplayEventValue& ev) { onGameplayHandler(ev); });
    hooks.onLevelTick([](const bedrock::sdk::LevelTickEventValue& ev) { onLevelTickHandler(ev); });
    
    if (!hooks.installNativeKeyboard(resolver, profile.json(), error) ||
        !hooks.installGameplay(resolver, error) ||
        !hooks.installLevelTick(resolver, error)) {
        hooks.stop();
        throw std::runtime_error(error);
    }
    logger.info("AutoSprint ready");
}
} // namespace dynamic_client