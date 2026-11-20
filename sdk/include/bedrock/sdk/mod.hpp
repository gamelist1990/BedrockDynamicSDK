#pragma once

#include "events.hpp"
#include "keybinds.hpp"
#include "settings.hpp"
#include "system.hpp"
#include "world.hpp"
#include <string>

namespace bedrock::sdk {

class Mod {
public:
    explicit Mod(std::string id) : id_(std::move(id)), keybinds_(id_) {}
    virtual ~Mod() = default;
    Mod(const Mod&) = delete;
    Mod& operator=(const Mod&) = delete;

    [[nodiscard]] const std::string& id() const noexcept { return id_; }
    SettingsPage settings(std::string titleKey) const { return SettingsPage{id_, std::move(titleKey)}; }
    System& system() noexcept { return system_; }
    World& world() noexcept { return world_; }
    KeyBindings& keybinds() noexcept { return keybinds_; }

    // Called by the Mod host. Lifecycle and ticks must be serialized.
    [[nodiscard]] Result<void> enable();
    [[nodiscard]] Result<void> disable();
    [[nodiscard]] Result<void> update();
    [[nodiscard]] Result<void> dispatchKey(Key key, bool pressed);

protected:
    virtual void onEnable() {}
    virtual void onDisable() {}
    virtual void onUpdate() {}
    // First owning tick with a new local-player runtime identity.
    virtual void onPlayerSpawn(Player&) {}
    virtual void onKeybind(std::string_view, bool) {}

private:
    enum class State { Disabled, Enabling, Enabled, Updating, Disabling };
    std::string id_;
    System system_;
    World world_;
    std::weak_ptr<detail::WorldSession> lastSpawnSession_;
    std::optional<std::uint64_t> lastLocalPlayer_;
    KeyBindings keybinds_;
    State state_{State::Disabled};
};

} // namespace bedrock::sdk
