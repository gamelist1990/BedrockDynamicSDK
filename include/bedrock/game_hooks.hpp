#pragma once

#ifdef _WIN32
#include <bedrock/game_api.hpp>
#include <bedrock/hook_runtime.h>
#include <bedrock/resolver.hpp>

#include <array>
#include <string>
#include <utility>

namespace bedrock {

// Convenience subscriber for input and Actor::getNameTag events. Hooks are
// routed through the process-wide shared MinHook broker, so multiple mod DLLs
// can subscribe to the same native call without installing competing detours.
class GameHooks final {
public:
    explicit GameHooks(GameApi& api) noexcept : api_(&api) {}
    ~GameHooks() { stop(); }
    GameHooks(const GameHooks&) = delete;
    GameHooks& operator=(const GameHooks&) = delete;

    bool install(Resolver& resolver, std::string& error) {
        if (!api_) { error = "GameApi is unavailable"; return false; }
        if (active()) { error.clear(); return true; }
        if (!installInput(resolver, error) || !installActorNameTag(resolver, error)) {
            stop(); return false;
        }
        error.clear();
        return true;
    }

    bool installInput(Resolver& resolver, std::string& error) {
        if (!api_) { error = "GameApi is unavailable"; return false; }
        if (tokens_[0] && tokens_[1]) { error.clear(); return true; }
        const auto keyboard = resolveSharedHook(
            resolver, "Keyboard::feed", BDS_HOOK_KEYBOARD_FEED);
        const auto mouse = resolveSharedHook(
            resolver, "MouseDevice::feed", BDS_HOOK_MOUSE_FEED);
        if (!keyboard || !mouse) {
            error = "required keyboard or mouse symbol is unavailable for this exact build";
            return false;
        }
        if (!subscribe(BDS_HOOK_KEYBOARD_FEED, keyboard->address, &onKeyboard, &tokens_[0]) ||
            !subscribe(BDS_HOOK_MOUSE_FEED, mouse->address, &onMouse, &tokens_[1])) {
            stop(); error = "shared input hook registration failed"; return false;
        }
        error.clear();
        return true;
    }

    bool installActorNameTag(Resolver& resolver, std::string& error) {
        if (!api_) { error = "GameApi is unavailable"; return false; }
        if (tokens_[2]) { error.clear(); return true; }
        const auto nameTag = resolveSharedHook(
            resolver, "Actor::getNameTag", BDS_HOOK_ACTOR_GET_NAME_TAG);
        if (!nameTag) {
            error = "Actor::getNameTag is unavailable for this exact build";
            return false;
        }
        auto* target = reinterpret_cast<void*>(nameTag->address);
        const bool alreadyOwned =
            BDS_HookOwnsTarget(BDS_HOOK_ACTOR_GET_NAME_TAG, target) != 0;
        if (!subscribe(BDS_HOOK_ACTOR_GET_NAME_TAG, nameTag->address,
                       &onNameTag, &tokens_[2])) {
            error = alreadyOwned
                ? "shared runtime already owns Actor::getNameTag but rejected the subscriber"
                : "Actor::getNameTag target is not owned by the shared hook broker; "
                  "a legacy direct/third-party detour or conflicting hookpoint may be "
                  "installed. Fully restart Minecraft to clear stale in-process hooks, "
                  "then inject only one current TabList DLL";
            return false;
        }
        error.clear();
        return true;
    }

    void stop() noexcept {
        for (auto& token : tokens_) {
            if (token) BDS_HookUnsubscribe(std::exchange(token, nullptr));
        }
    }
    [[nodiscard]] bool active() const noexcept {
        return tokens_[0] && tokens_[1] && tokens_[2];
    }

private:
    bool subscribe(BDS_HookPoint point, std::uintptr_t address,
                   BDS_HookCallback callback, BDS_HookToken* token) noexcept {
        return BDS_HookSubscribe(point, reinterpret_cast<void*>(address), callback,
                                 api_, token) != 0;
    }
    static void BDS_HOOK_CALL onKeyboard(void* user, void* raw,
                                         BDS_HookNext next, void* frame) noexcept {
        auto& args = *static_cast<BDS_KeyboardArgs*>(raw);
        auto& api = *static_cast<GameApi*>(user);
        try {
            if (args.key < 0) { next(frame, &args); return; }
            KeyEvent event{static_cast<unsigned>(args.key), args.down != 0, false};
            if (api.dispatchKey(event)) next(frame, &args);
            else args.result = 0;
        } catch (...) {
            next(frame, &args);
        }
    }
    static void BDS_HOOK_CALL onMouse(void* user, void* raw,
                                      BDS_HookNext next, void* frame) noexcept {
        auto& args = *static_cast<BDS_MouseArgs*>(raw);
        auto& api = *static_cast<GameApi*>(user);
        try {
            MouseEvent event{args.action, args.state, args.x, args.y,
                             args.dx, args.dy, args.force_motionless_pointer};
            if (api.dispatchMouse(event)) next(frame, &args);
        } catch (...) {
            next(frame, &args);
        }
    }
    static void BDS_HOOK_CALL onNameTag(void* user, void* raw,
                                        BDS_HookNext next, void* frame) noexcept {
        auto& args = *static_cast<BDS_ActorNameTagArgs*>(raw);
        next(frame, &args);
        if (!args.actor || !args.name_tag) return;
        try {
            const auto& value = *static_cast<const std::string*>(args.name_tag);
            static_cast<GameApi*>(user)->observeActorNameTag(args.actor, value);
        } catch (...) {
            // Never let an SDK listener or cache failure escape into Minecraft.
        }
    }

    GameApi* api_{};
    std::array<BDS_HookToken, 3> tokens_{};
};

} // namespace bedrock
#endif // _WIN32
