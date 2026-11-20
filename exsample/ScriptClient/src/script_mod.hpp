#pragma once

#include "plugin_host.hpp"

#include <bedrock/sdk.hpp>

#include <functional>
#include <stdexcept>
#include <string>

namespace stageb {

// One JS plugin = one sdk::Mod instance (§7.2). The host drives its lifecycle;
// the JS side is owned by PluginHost and reached through tickMod(),
// notifyPlayerSpawn(), and notifyKeybind().
class ScriptMod final : public bedrock::sdk::Mod {
public:
    using Drain = std::function<void()>;

    ScriptMod(PluginHost& host, std::string name, Drain drain)
        : Mod(lowerCopy(name)), host_(host), name_(std::move(name)), drain_(std::move(drain)) {}

    const std::string& modName() const noexcept { return name_; }

private:
    void onEnable() override {
        // Keybinds declared during JS onEnable() must be added to this
        // sdk::Mod's KeyBindings before Mod::enable() invokes keybinds_.connect().
        if (const auto* decl = host_.keybindsFor(name_)) {
            for (const auto& entry : decl->bindings) {
                const auto res = keybinds().add(entry.id, entry.labelKey, entry.defaultKey);
                if (!res) throw std::runtime_error(res.error().message);
            }
            for (const auto& loc : decl->locales) {
                keybinds().locale(loc.id, loc.entries);
            }
        }
    }

    void onUpdate() override {
        host_.tickMod(name_);
        if (drain_) drain_(); // queued command lines run on the owning tick
    }

    // §8.1: event subscriptions are forwarded by the host on the owning tick.
    void onPlayerSpawn(bedrock::sdk::Player&) override {
        host_.notifyPlayerSpawn(name_, 0, !spawnedOnce_);
        spawnedOnce_ = true;
    }

    void onKeybind(std::string_view id, bool pressed) override {
        host_.notifyKeybind(name_, id, pressed);
    }

    PluginHost& host_;
    std::string name_;
    Drain drain_;
    bool spawnedOnce_ = false;
};

// The ClientHost must be started with a Mod. This anchor has no JS realm and
// exists so every ScriptMod can be attached with the D-2 API and so queued
// commands (/mods reload, etc.) still drain even when all plugins are faulted.
class HostAnchorMod final : public bedrock::sdk::Mod {
public:
    using Drain = std::function<void()>;
    HostAnchorMod() : Mod("script-client-host") {}
    HostAnchorMod(PluginHost& /*host*/, Drain drain)
        : Mod("script-client-host"), drain_(std::move(drain)) {}

private:
    void onUpdate() override {
        if (drain_) drain_();
    }
    Drain drain_;
};

} // namespace stageb
