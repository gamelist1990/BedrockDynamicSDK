#include <bedrock/sdk/mod.hpp>
#include "game_frame.hpp"

#include <exception>

namespace bedrock::sdk {
namespace {
template<class Callback>
Result<void> invoke(Callback&& callback) {
    try {
        callback();
        return Result<void>::ok();
    } catch (const std::exception& error) {
        return Result<void>::error(ErrorCode::Internal, error.what());
    } catch (...) {
        return Result<void>::error(ErrorCode::Internal, "unknown Mod callback failure");
    }
}
}

Result<void> Mod::enable() {
    if (state_ == State::Enabled) return Result<void>::ok();
    if (state_ != State::Disabled)
        return Result<void>::error(ErrorCode::Invalid, "cannot enable during a Mod callback");
    state_ = State::Enabling;
    auto result = invoke([this] { onEnable(); });
    if (result) result = keybinds_.connect();
    state_ = result ? State::Enabled : State::Disabled;
    if (!result) system_.clear();
    return result;
}

Result<void> Mod::disable() {
    if (state_ == State::Disabled) return Result<void>::ok();
    if (state_ != State::Enabled)
        return Result<void>::error(ErrorCode::Invalid, "cannot disable during a Mod callback");
    state_ = State::Disabling;
    auto result = invoke([this] { onDisable(); });
    auto disconnected = keybinds_.disconnect();
    system_.clear();
    lastSpawnSession_.reset();
    lastLocalPlayer_.reset();
    state_ = State::Disabled;
    return result ? disconnected : result;
}

Result<void> Mod::update() {
    if (state_ != State::Enabled)
        return Result<void>::error(ErrorCode::Unavailable, "Mod is not ready to update");
    state_ = State::Updating;
    auto spawn = invoke([this] {
        if (auto* frame = detail::frameFor(&world_)) {
            const auto id = frame->localPlayerId();
            const bool changed = lastSpawnSession_.lock() != frame->session || lastLocalPlayer_ != id;
            lastSpawnSession_ = frame->session;
            lastLocalPlayer_ = id;
            if (changed)
                if (auto player = world_.localPlayer()) onPlayerSpawn(*player);
        }
    });
    auto tasks = invoke([this] { system_.advance(); });
    auto frame = invoke([this] { onUpdate(); });
    state_ = State::Enabled;
    return !spawn ? spawn : (tasks ? frame : tasks);
}

Result<void> Mod::dispatchKey(Key key, bool pressed) {
    if (state_ != State::Enabled)
        return Result<void>::error(ErrorCode::Unavailable, "Mod is not ready for input");
    return invoke([&] {
        for (const auto& id : keybinds_.dispatch(key, pressed)) onKeybind(id, pressed);
    });
}

} // namespace bedrock::sdk
