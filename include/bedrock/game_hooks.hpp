#pragma once

#ifdef _WIN32
#include <bedrock/game_api.hpp>
#include <bedrock/hook_runtime.h>
#include <bedrock/native_keyboard.hpp>
#include <bedrock/native_world_events.hpp>
#include <bedrock/resolver.hpp>

#include <array>
#include <string>
#include <utility>

namespace bedrock {

// Convenience subscriber for input, world lifecycle and Actor events. Hooks are
// routed through the process-wide shared MinHook broker, so multiple mod DLLs
// can subscribe to the same native call without installing competing detours.
class GameHooks final {
public:
    explicit GameHooks(GameApi& api) noexcept : api_(&api) {}
    ~GameHooks() { stop(); }
    GameHooks(const GameHooks&) = delete;
    GameHooks& operator=(const GameHooks&) = delete;

    bool install(const Resolver& resolver, std::string& error) {
        if (!api_) { error = "GameApi is unavailable"; return false; }
        if (active()) { error.clear(); return true; }
        if (!installInput(resolver, error) || !installActorNameTag(resolver, error)) {
            stop(); return false;
        }
        error.clear();
        return true;
    }

    bool installInput(const Resolver& resolver, std::string& error) {
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

    bool installActorNameTag(const Resolver& resolver, std::string& error) {
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

    bool installFov(const Resolver& resolver, std::string& error) {
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

    bool installGameplay(const Resolver& resolver, std::string& error) {
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

    bool installLevelTick(const Resolver& resolver, std::string& error) {
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

    // Opt-in native lifecycle source. Resolve all four guarded exact-build
    // entries before subscribing, and roll back only this group on failure.
    bool installWorldEvents(const Resolver& resolver, std::string_view profile,
                            std::string& error) {
        if (!api_) { error = "GameApi is unavailable"; return false; }
        if (worldEventsActive()) { error.clear(); return true; }
        const auto layout = parseNativeWorldEventsLayout(profile, error);
        if (!layout) return false;
        constexpr std::array<const char*, 4> symbols{
            "ClientInstance::onPlayerLoaded", "ClientInstance::onLevelExit",
            "ClientInstance::onDimensionChangedEvent", "ClientInstance::onCancelJoinGame"};
        constexpr std::array<BDS_HookPoint, 4> points{
            BDS_HOOK_CLIENT_PLAYER_LOADED, BDS_HOOK_CLIENT_LEVEL_EXIT,
            BDS_HOOK_CLIENT_DIMENSION_CHANGED, BDS_HOOK_CLIENT_JOIN_CANCELLED};
        constexpr std::array<BDS_HookCallback, 4> callbacks{
            &onPlayerLoaded, &onLevelExit, &onDimensionChanged, &onJoinCancelled};
        std::array<std::uintptr_t, 4> addresses{};
        for (std::size_t i = 0; i != symbols.size(); ++i) {
            const auto target = resolveSharedHook(resolver, symbols[i], points[i]);
            if (!target || !isUsableSharedHookResolution(*target)) {
                error = std::string("world event target unavailable for this exact build: ") + symbols[i];
                return false;
            }
            addresses[i] = target->address;
        }
        worldLayout_ = *layout;
        for (std::size_t i = 0; i != points.size(); ++i) {
            if (!BDS_HookSubscribe(points[i], reinterpret_cast<void*>(addresses[i]),
                                   callbacks[i], this, &tokens_[8 + i])) {
                for (std::size_t j = 8; j != tokens_.size(); ++j)
                    if (tokens_[j]) BDS_HookUnsubscribe(std::exchange(tokens_[j], nullptr));
                error = std::string("shared world event hook registration failed: ") + symbols[i];
                return false;
            }
        }
        error.clear();
        return true;
    }
    [[nodiscard]] bool worldEventsActive() const noexcept {
        return tokens_[8] && tokens_[9] && tokens_[10] && tokens_[11];
    }

    bool installNativeKeyboard(const Resolver& resolver, std::string_view profile,
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
    static void BDS_HOOK_CALL onPlayerLoaded(void* user, void* raw,
                                            BDS_HookNext next, void* frame) noexcept {
        auto& args = *static_cast<BDS_ClientPlayerLoadedArgs*>(raw);
        next(frame, raw);
        static_cast<GameHooks*>(user)->api_->dispatchWorldChange(
            WorldChangeReason::Joined, args.self, args.player);
    }
    static void BDS_HOOK_CALL onLevelExit(void* user, void* raw,
                                         BDS_HookNext next, void* frame) noexcept {
        auto& hooks = *static_cast<GameHooks*>(user);
        const auto self = reinterpret_cast<std::uintptr_t>(
            static_cast<BDS_ClientWorldArgs*>(raw)->self);
        bool notified = false;
        if (self >= hooks.worldLayout_.exitClientOffset && self != 0)
            notified = hooks.api_->dispatchWorldChange(WorldChangeReason::Left,
                reinterpret_cast<void*>(self - hooks.worldLayout_.exitClientOffset));
        const auto epoch = hooks.api_->worldEpoch();
        next(frame, raw); // keep original GameCallbacks*, never pass normalized this
        if (notified && hooks.api_->worldEpoch() == epoch)
            try { hooks.api_->clearObservedActors(); } catch (...) { }
    }
    static void BDS_HOOK_CALL onDimensionChanged(void* user, void* raw,
                                                BDS_HookNext next, void* frame) noexcept {
        next(frame, raw);
        static_cast<GameHooks*>(user)->api_->dispatchWorldChange(
            WorldChangeReason::DimensionChanged,
            static_cast<BDS_ClientWorldArgs*>(raw)->self);
    }
    static void BDS_HOOK_CALL onJoinCancelled(void* user, void* raw,
                                             BDS_HookNext next, void* frame) noexcept {
        auto& api = *static_cast<GameHooks*>(user)->api_;
        const bool notified = api.dispatchWorldChange(
            WorldChangeReason::JoinCancelled,
            static_cast<BDS_ClientWorldArgs*>(raw)->self);
        const auto epoch = api.worldEpoch();
        next(frame, raw);
        if (notified && api.worldEpoch() == epoch)
            try { api.clearObservedActors(); } catch (...) { }
    }
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
    NativeWorldEventsLayout worldLayout_{};
    std::array<BDS_HookToken, 12> tokens_{};
};

} // namespace bedrock
#endif // _WIN32
