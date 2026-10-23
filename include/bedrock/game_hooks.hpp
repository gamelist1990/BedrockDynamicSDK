#pragma once

#ifdef _WIN32
#include <bedrock/game_api.hpp>
#include <bedrock/hook_runtime.h>
#include <bedrock/native_keyboard.hpp>
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

    bool installFov(Resolver& resolver, std::string& error) {
        if (!api_) { error = "GameApi is unavailable"; return false; }
        if (tokens_[3]) { error.clear(); return true; }
        const auto fov = resolveSharedHook(resolver, "LevelRendererPlayer::getFov",
                                           BDS_HOOK_GET_FOV);
        if (!fov || !subscribe(BDS_HOOK_GET_FOV, fov->address, &onFov, &tokens_[3])) {
            error = "shared FOV hook registration failed";
            return false;
        }
        error.clear();
        return true;
    }

    bool installGameplay(Resolver& resolver, std::string& error) {
        if (!api_) { error = "GameApi is unavailable"; return false; }
        if (tokens_[4] && tokens_[5]) { error.clear(); return true; }
        const auto grab = resolveSharedHook(resolver, "ClientInstance::grabCursor",
                                            BDS_HOOK_GRAB_CURSOR);
        const auto release = resolveSharedHook(resolver, "ClientInstance::releaseCursor",
                                               BDS_HOOK_RELEASE_CURSOR);
        if (!grab || !release ||
            !subscribe(BDS_HOOK_GRAB_CURSOR, grab->address, &onGrab, &tokens_[4]) ||
            !subscribe(BDS_HOOK_RELEASE_CURSOR, release->address, &onRelease, &tokens_[5])) {
            error = "shared gameplay cursor hook registration failed";
            return false;
        }
        error.clear();
        return true;
    }

    bool installLevelTick(Resolver& resolver, std::string& error) {
        if (!api_) { error = "GameApi is unavailable"; return false; }
        if (tokens_[7]) { error.clear(); return true; }
        const auto level = resolveSharedHook(resolver, "MultiPlayerLevel::_subTick",
                                             BDS_HOOK_LEVEL_SUB_TICK);
        if (!level || !subscribe(BDS_HOOK_LEVEL_SUB_TICK, level->address,
                                 &onLevelTick, &tokens_[7])) {
            error = "shared Level tick hook registration failed";
            return false;
        }
        error.clear();
        return true;
    }

    bool installNativeKeyboard(Resolver& resolver, std::string_view profile,
                               std::string& error) {
        if (!api_) { error = "GameApi is unavailable"; return false; }
        if (tokens_[6]) { error.clear(); return true; }
        const auto layout = parseNativeKeyboardLayout(profile, error);
        if (!layout) return false;
        const auto target = resolveSharedHook(resolver, "GameCore::keyboardEvent",
                                              BDS_HOOK_GAMECORE_KEYBOARD_EVENT);
        keyboardLayout_ = *layout;
        if (!target || !isUsableSharedHookResolution(*target) ||
            !BDS_HookSubscribe(BDS_HOOK_GAMECORE_KEYBOARD_EVENT,
                reinterpret_cast<void*>(target->address), &onNativeKeyboard,
                this, &tokens_[6])) {
            error = "shared native KeyboardEvent hook registration failed";
            return false;
        }
        error.clear();
        return true;
    }

    void stop() noexcept {
        for (auto& token : tokens_) {
            if (token) BDS_HookUnsubscribe(std::exchange(token, nullptr));
        }
        if (api_) api_->clearNativeKeyboard();
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
            api.observeMouseDevice(args);
            MouseEvent event{args.action, args.state, args.x, args.y,
                             args.dx, args.dy, args.force_motionless_pointer,
                             api.isSyntheticMouseDispatch()};
            if (api.dispatchMouse(event)) next(frame, &args);
        } catch (...) {
            next(frame, &args);
        }
    }

    static void BDS_HOOK_CALL onFov(void* user, void* raw,
                                   BDS_HookNext next, void* frame) noexcept {
        auto& args = *static_cast<BDS_FovArgs*>(raw);
        next(frame, &args);
        auto& api = *static_cast<GameApi*>(user);
        FovEvent event{args.self, args.result, args.result};
        api.dispatchFov(event);
        args.result = event.resultFov;
    }
    static void BDS_HOOK_CALL onLevelTick(void* user, void* raw,
                                          BDS_HookNext next, void* frame) noexcept {
        auto& args = *static_cast<BDS_LevelArgs*>(raw);
        next(frame, &args);
        static std::atomic<std::uint64_t> sequence{0};
        static_cast<GameApi*>(user)->dispatchLevelTick(
            args.level, sequence.fetch_add(1, std::memory_order_relaxed) + 1);
    }
    static void BDS_HOOK_CALL onGrab(void* user, void* raw,
                                     BDS_HookNext next, void* frame) noexcept {
        auto& args = *static_cast<BDS_CursorArgs*>(raw);
        next(frame, &args);
        static_cast<GameApi*>(user)->dispatchGameplay(GameplayEvent{true, args.self});
    }
    static void BDS_HOOK_CALL onRelease(void* user, void* raw,
                                        BDS_HookNext next, void* frame) noexcept {
        auto& args = *static_cast<BDS_CursorArgs*>(raw);
        static_cast<GameApi*>(user)->dispatchGameplay(GameplayEvent{false, args.self});
        next(frame, &args);
    }
    static void BDS_HOOK_CALL onNativeKeyboard(void* user, void* raw,
                                               BDS_HookNext next, void* frame) noexcept {
        auto& args = *static_cast<BDS_GameCoreKeyboardEventArgs*>(raw);
        next(frame, &args);
        auto& hooks = *static_cast<GameHooks*>(user);
        const auto input = captureNativeKeyboardEvent(args.context, hooks.keyboardLayout_);
        if (input) hooks.api_->dispatchNativeKeyboard(*input);
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
    NativeKeyboardLayout keyboardLayout_{};
    std::array<BDS_HookToken, 8> tokens_{};
};

} // namespace bedrock
#endif // _WIN32
