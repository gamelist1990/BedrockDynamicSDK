#pragma once

#include <bedrock/detail/client_access.hpp>
#include <bedrock/detail/client_events.hpp>
#include <bedrock/native/keyboard.hpp>
#include <bedrock/runtime/hook_runtime.h>

#include <atomic>
#include <mutex>
#include <unordered_map>

namespace bedrock::detail {

// Per-subscriber delivery state, independent of native hook installation.
// The native adapter (when supplied) must outlive this dispatcher.
class HookDispatch final {
public:
    explicit HookDispatch(ClientAccess& access = inactiveClientAccess()) noexcept : access_(&access) {}

    [[nodiscard]] ClientAccess& nativeAccess() const noexcept { return *access_; }

    template<class Event, class Handler>
    auto on(Handler&& handler) {
        return events_.on<Event>(std::forward<Handler>(handler));
    }

    void observeActorNameTag(void* actor, std::string_view tag) {
        if (!actor || tag.empty() || !access_->isActorValid(actor)) return;
        {
            std::lock_guard lock(tagsMutex_);
            const auto it = tags_.find(actor);
            if (it != tags_.end() && it->second == tag) return;
            tags_.insert_or_assign(actor, std::string(tag));
        }
        events_.emit<ActorNameTagEvent>({actor, tag, NameTagSource::ActorGetter});
    }

    void clearObservedActors() {
        std::lock_guard lock(tagsMutex_);
        tags_.clear();
    }

    [[nodiscard]] bool dispatchKey(KeyEvent& event) noexcept {
        try { events_.emit<KeyEvent>(event); }
        catch (...) { return true; } // callback failures must not swallow game input
        return !event.cancelled();
    }
    void dispatchLevelTick(void* level, std::uint64_t sequence) noexcept {
        if (!access_->beginWorldTick(level)) return;
        struct TickScope { ClientAccess* access; ~TickScope() { access->endWorldTick(); } } scope{access_};
        worldEventLevel_.store(level, std::memory_order_release);
        try { events_.emit<LevelTickEvent>(LevelTickEvent{level, sequence}); }
        catch (...) { }
    }
    [[nodiscard]] std::uint64_t worldEpoch() const noexcept {
        return worldEventEpoch_.load(std::memory_order_acquire);
    }
    // Select the intended client for split-screen. Otherwise the first native
    // notification binds each session, and exit/cancel releases that binding.
    void setWorldEventClient(void* client) noexcept {
        worldEventClientPinned_.store(client != nullptr, std::memory_order_release);
        worldEventClient_.store(client, std::memory_order_release);
    }
    // Adapter entry point, also usable by an external event source. Each real
    // notification advances the generation, even when Minecraft reuses pointers.
    bool dispatchWorldChange(WorldChangeReason reason, void* client,
                             void* player = nullptr) noexcept {
        if (!client) return false;
        auto* expected = static_cast<void*>(nullptr);
        worldEventClient_.compare_exchange_strong(expected, client,
            std::memory_order_acq_rel);
        if (worldEventClient_.load(std::memory_order_acquire) != client) return false;
        WorldChangeEvent event{reason, client,
            worldEventLevel_.exchange(nullptr, std::memory_order_acq_rel), player,
            worldEventEpoch_.fetch_add(1, std::memory_order_acq_rel) + 1};
        if (reason == WorldChangeReason::Joined) {
            event.level = nullptr;
            try {
                if (player && access_->isActorValid(player)) event.level = access_->worldOf(player);
            } catch (...) { }
            worldEventLevel_.store(event.level, std::memory_order_release);
        }
        // Invalidate before callbacks so consumers cannot see the old roster.
        try { clearObservedActors(); } catch (...) { }
        if ((reason == WorldChangeReason::Left || reason == WorldChangeReason::JoinCancelled) &&
            !worldEventClientPinned_.load(std::memory_order_acquire)) {
            expected = client;
            worldEventClient_.compare_exchange_strong(expected, nullptr, std::memory_order_acq_rel);
        }
        try { events_.emit<WorldChangeEvent>(event); } catch (...) { }
        // A throwing general listener must not suppress the specific event.
        try {
            switch (reason) {
            case WorldChangeReason::Joined:
                events_.emit<ClientJoinLevelEvent>(ClientJoinLevelEvent{event}); break;
            case WorldChangeReason::Left:
                events_.emit<ClientExitLevelEvent>(ClientExitLevelEvent{event}); break;
            case WorldChangeReason::JoinCancelled:
                events_.emit<ClientCancelJoinLevelEvent>(ClientCancelJoinLevelEvent{event}); break;
            case WorldChangeReason::DimensionChanged:
                events_.emit<DimensionChangeEvent>(DimensionChangeEvent{event}); break;
            }
        } catch (...) { }
        return true;
    }
    [[nodiscard]] bool dispatchMouse(MouseEvent& event) noexcept {
        try { events_.emit<MouseEvent>(event); }
        catch (...) { return true; }
        return !event.cancelled();
    }
    void dispatchGameplay(const GameplayEvent& event) noexcept {
        access_->observeClient(event.clientInstance);
        gameplayActive_.store(event.active, std::memory_order_release);
        try { events_.emit<GameplayEvent>(event); } catch (...) {}
    }
    void dispatchFov(FovEvent& event) noexcept {
        try { events_.emit<FovEvent>(event); } catch (...) {}
    }
    [[nodiscard]] bool gameplayActive() const noexcept {
        return gameplayActive_.load(std::memory_order_acquire);
    }
    void dispatchNativeKeyboard(const NativeKeyboardEvent& captured) noexcept {
        {
            std::lock_guard lock(inputMutex_);
            nativeKeyboard_ = captured.input;
        }
        try { events_.emit<NativeKeyEvent>(
            NativeKeyEvent{static_cast<unsigned>(captured.key), captured.down}); }
        catch (...) {}
    }
    void clearNativeKeyboard() noexcept {
        std::lock_guard lock(inputMutex_);
        nativeKeyboard_.reset();
    }
    [[nodiscard]] bool sendNativeKey(unsigned key, bool pressed) noexcept {
        std::optional<NativeKeyboardInput> route;
        {
            std::lock_guard lock(inputMutex_);
            route = nativeKeyboard_;
        }
        return route && route->send(static_cast<int>(key), pressed);
    }
    void observeMouseDevice(const BDS_MouseArgs& args) noexcept {
        std::lock_guard lock(inputMutex_);
        mouseDevice_ = args.self;
        lastMouseArgs_ = args;
    }
    [[nodiscard]] bool sendMouseInput(unsigned action, bool pressed) noexcept {
#ifdef _WIN32
        BDS_MouseArgs args{};
        {
            std::lock_guard lock(inputMutex_);
            if (!mouseDevice_) return false;
            args = lastMouseArgs_;
        }
        args.action = static_cast<std::int8_t>(action);
        args.state = pressed ? 1 : 0;
        args.force_motionless_pointer =
            (args.force_motionless_pointer & BDS_MOUSE_FLAG_FORCE_MOTIONLESS) |
            BDS_MOUSE_FLAG_SYNTHETIC;
        const auto* previousDispatch = syntheticMouseDispatch_;
        syntheticMouseDispatch_ = this;
        BDS_HookDispatch(BDS_HOOK_MOUSE_FEED, &args);
        syntheticMouseDispatch_ = previousDispatch;
        return true;
#else
        (void)action; (void)pressed;
        return false;
#endif
    }
    [[nodiscard]] bool isSyntheticMouseDispatch() const noexcept {
        return syntheticMouseDispatch_ == this;
    }

private:
    EventBus<KeyEvent, NativeKeyEvent, MouseEvent, LevelTickEvent, GameplayEvent,
             FovEvent, ActorNameTagEvent, WorldChangeEvent, ClientJoinLevelEvent,
             ClientExitLevelEvent, ClientCancelJoinLevelEvent, DimensionChangeEvent> events_;
    ClientAccess* access_;
    std::atomic<void*> worldEventClient_{nullptr};
    std::atomic<bool> worldEventClientPinned_{false};
    std::atomic<void*> worldEventLevel_{nullptr};
    std::atomic<std::uint64_t> worldEventEpoch_{0};
    std::mutex inputMutex_;
    std::optional<NativeKeyboardInput> nativeKeyboard_;
    void* mouseDevice_{};
    BDS_MouseArgs lastMouseArgs_{};
    std::atomic<bool> gameplayActive_{false};
    inline static thread_local const HookDispatch* syntheticMouseDispatch_{};
    std::mutex tagsMutex_;
    std::unordered_map<void*, std::string> tags_;
};

} // namespace bedrock::detail
