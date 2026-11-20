#include "client.hpp"
#include "zoom/controller.hpp"
#include <bedrock/sdk/keybinds.hpp>
#include <bedrock/runtime/game_hooks.hpp>
#include <algorithm>
#include <chrono>
#include <mutex>

namespace dynamic_client {
namespace {
bedrock::detail::HookDispatch api;
bedrock::GameHooks hooks{api};
zoom::Controller controller;
std::mutex mutex;
zoom::Config config;
float configuredMultiplier = config.zoomMultiplier;
}

void setZoomConfig(const zoom::Config& next) {
    std::lock_guard lock(mutex);
    if (config.mode != next.mode || config.enabled != next.enabled) controller.resetInput();
    const float multiplier = configuredMultiplier == next.zoomMultiplier
        ? config.zoomMultiplier : next.zoomMultiplier;
    configuredMultiplier = next.zoomMultiplier;
    config = next;
    config.zoomMultiplier = multiplier;
}

void onZoomKeybind(bool pressed) {
    std::lock_guard lock(mutex);
    if (config.enabled) controller.onMinecraftKey(config.key, pressed, config);
}

void startZoom(const bedrock::RuntimeProfile& profile, bedrock::Logger& logger) {
    std::string error;
    if (!profile.supports({bedrock::Feature::Input, bedrock::Feature::Fov, bedrock::Feature::Gameplay},
                          error)) throw std::runtime_error(error);
    api.on<bedrock::GameplayEvent>([](const auto& event) {
        std::lock_guard lock(mutex);
        controller.setGameplayActive(event.active);
    });
    api.on<bedrock::MouseEvent>([](const auto& event) {
        constexpr int wheel = 4;
        if (event.synthetic || event.action != wheel || event.state == 0) return;
        std::lock_guard lock(mutex);
        if (!config.scrollAdjustment || !controller.isZoomActive(config)) return;
        const auto step = event.state > 0 ? config.scrollStep : -config.scrollStep;
        config.zoomMultiplier = std::clamp(config.zoomMultiplier + step, 1.f, 50.f);
        event.cancel();
    });
    api.on<bedrock::FovEvent>([](const auto& event) {
        std::lock_guard lock(mutex);
        static auto last = std::chrono::steady_clock::now();
        const auto now = std::chrono::steady_clock::now();
        const float delta = std::chrono::duration<float>(now - last).count();
        last = now;
        if (config.enabled) {
            event.setResultFov(zoom::zoomFovDegrees(event.resultFov, controller.tick(config, delta)));
        } else {
            // Do not override another subscriber's FOV when disabled.
            (void)controller.tick(config, .25f);
        }
    });
    if (!hooks.installInput(profile.resolver(), error) ||
        !hooks.installGameplay(profile.resolver(), error) ||
        !hooks.installFov(profile.resolver(), error)) {
        hooks.stop();
        throw std::runtime_error(error);
    }
    logger.info("Zoom ready");
}
} // namespace dynamic_client
