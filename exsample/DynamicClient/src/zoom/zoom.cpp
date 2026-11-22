#include "client.hpp"
#include "zoom/controller.hpp"
#include <bedrock/sdk/keybinds.hpp>
#include <bedrock/sdk/game_hooks.hpp>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <mutex>

namespace dynamic_client {
namespace {
using bedrock::sdk::Hooks;
using bedrock::sdk::GameplayEventValue;
using bedrock::sdk::FovEventValue;
using bedrock::sdk::MouseEventValue;

Hooks hooks;
zoom::Controller controller;
zoom::Config config;
float configuredMultiplier = config.zoomMultiplier;
std::mutex mutex;

} // namespace

void setZoomConfig(const zoom::Config& next) {
    std::lock_guard<std::mutex> lock(mutex);
    if (config.mode != next.mode || config.enabled != next.enabled) controller.resetInput();
    const float multiplier = configuredMultiplier == next.zoomMultiplier
        ? config.zoomMultiplier : next.zoomMultiplier;
    configuredMultiplier = next.zoomMultiplier;
    config = next;
    config.zoomMultiplier = multiplier;
}

void onZoomKeybind(bool pressed) {
    std::lock_guard<std::mutex> lock(mutex);
    if (config.enabled) controller.onMinecraftKey(config.key, pressed, config);
}

void startZoom(const bedrock::sdk::Profile& profile, const bedrock::sdk::SymbolResolver& resolver,
               bedrock::sdk::Logger& logger) {
    std::string error;
    
    hooks.onGameplay([&](const bedrock::sdk::GameplayEventValue& ev) {
        std::lock_guard<std::mutex> lock(mutex);
        controller.setGameplayActive(ev.active);
    });
    hooks.onMouse([&](bedrock::sdk::MouseEventValue& ev) {
        constexpr int wheel = 4;
        if (ev.synthetic || ev.action != wheel || ev.state == 0) return;
        std::lock_guard<std::mutex> lock(mutex);
        if (!config.scrollAdjustment || !controller.isZoomActive(config)) return;
        const auto step = ev.state > 0 ? config.scrollStep : -config.scrollStep;
        config.zoomMultiplier = std::clamp(config.zoomMultiplier + step, 1.f, 50.f);
        ev.cancelled = true;  // read back by Hooks::onMouse impl -> e.cancel()
    });
    hooks.onFov([&](bedrock::sdk::FovEventValue& ev) {
        std::lock_guard<std::mutex> lock(mutex);
        static auto last = std::chrono::steady_clock::now();
        const auto now = std::chrono::steady_clock::now();
        const float delta = std::chrono::duration<float>(now - last).count();
        last = now;
        if (config.enabled) {
            ev.resultFov = zoom::zoomFovDegrees(ev.resultFov, controller.tick(config, delta));
        } else {
            // Do not override another subscriber's FOV when disabled.
            (void)controller.tick(config, .25f);
        }
    });

    // Granular public installs — each composes with the monolithic install()
    if (!hooks.installInput(resolver, error) ||
        !hooks.installFov(resolver, error) ||
        !hooks.installGameplay(resolver, error)) {
        hooks.stop();
        throw std::runtime_error(error);
    }
    logger.info("Zoom ready");
}
} // namespace dynamic_client