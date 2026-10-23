#pragma once
// Lightweight, header-only, typed event dispatcher.
// No Minecraft binary offsets, Win32 headers, hooks, JSON, downloads, or threads.
// The event source belongs to the client (or an independently validated adapter).
#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace bedrock {

struct UpdateEvent {
    // Emitted by a client-owned update source. NOT necessarily a Minecraft game tick.
    std::uint64_t sequence{};
};
struct LevelTickEvent {
    void* level{};
    std::uint64_t sequence{};
};
struct KeyEvent {
    // Kept as `virtualKey` for source compatibility. On Minecraft hooks this
    // is Minecraft's InputKey value, not a promise that it is a Win32 VK.
    unsigned virtualKey{};
    bool pressed{};
    bool repeat{};
    KeyEvent() = default;
    KeyEvent(unsigned key, bool isPressed, bool isRepeat = false) noexcept
        : virtualKey(key), pressed(isPressed), repeat(isRepeat) {}
    [[nodiscard]] unsigned inputKey() const noexcept { return virtualKey; }
    void cancel() const noexcept { cancelled_ = true; }
    [[nodiscard]] bool cancelled() const noexcept { return cancelled_; }
private:
    mutable bool cancelled_{};
};
// A post-dispatch key transition captured from Minecraft's native HID route.
// Unlike KeyEvent (which is cancellable before Keyboard::feed), this event has
// a usable native send route attached to GameApi for synchronous callbacks.
struct NativeKeyEvent {
    unsigned inputKey{};
    bool pressed{};
};
struct MouseEvent {
    std::int8_t action{}, state{}; // native Minecraft MouseDevice::feed values
    int x{}, y{}, deltaX{}, deltaY{};
    int forceMotionlessPointer{};
    bool synthetic{};
    MouseEvent() = default;
    MouseEvent(std::int8_t nativeAction, std::int8_t nativeState,
               int cursorX = 0, int cursorY = 0, int dx = 0, int dy = 0,
               int force = 0, bool isSynthetic = false) noexcept
        : action(nativeAction), state(nativeState), x(cursorX), y(cursorY),
          deltaX(dx), deltaY(dy), forceMotionlessPointer(force),
          synthetic(isSynthetic) {}
    void cancel() const noexcept { cancelled_ = true; }
    [[nodiscard]] bool cancelled() const noexcept { return cancelled_; }
private:
    mutable bool cancelled_{};
};
struct FocusEvent { bool focused{}; };
// Emitted by a platform adapter when Minecraft is actively accepting gameplay
// input (e.g. its window is focused and the gameplay cursor is captured).
struct GameplayEvent {
    bool active{};
    void* clientInstance{};
};
// Emitted after Minecraft calculates the baseline FOV. Listeners may adjust
// the final value with setResultFov(); the shared hook runtime returns it to
// the game after all listeners have run.
struct FovEvent {
    void* renderer{};
    float inputFov{};
    mutable float resultFov{};
    void setResultFov(float value) const noexcept { resultFov = value; }
};
enum class NameTagSource : std::uint8_t { ActorGetter, View };
struct ActorNameTagEvent {
    void* actor{};
    std::string_view nameTag; // valid only for the duration of dispatch
    NameTagSource source{NameTagSource::ActorGetter};
};
struct ViewNameTagEvent {
    void* actor{};
    std::string_view nameTag; // valid only for the duration of dispatch
};
// An Actor observed in the current LevelRendererCamera render/culling pass.
// This is a visible-render candidate, not a complete world Actor list.
struct RenderViewActorEvent {
    const void* viewObject{}; // borrowed; valid only during dispatch
    void* actor{};            // borrowed; validate before retaining
    std::uint64_t frame{};
};

template <class Event>
class EventChannel {
public:
    using Callback = std::function<void(const Event&)>;
    using ListenerId = std::uint64_t;

    ListenerId subscribe(Callback callback) {
        if (!callback) throw std::invalid_argument("event handler must be callable");
        std::lock_guard lock(mutex_);
        const ListenerId id = nextId_++;
        auto slot = std::make_shared<Slot>(id, std::move(callback));
        slots_.push_back(std::move(slot));
        return id;
    }

    ListenerId then(Callback callback) { return subscribe(std::move(callback)); }

    bool unsubscribe(ListenerId id) {
        std::lock_guard lock(mutex_);
        const auto it = std::find_if(slots_.begin(), slots_.end(),
            [id](const auto& slot) { return slot->id == id; });
        if (it == slots_.end() || !(*it)->active.exchange(false, std::memory_order_acq_rel))
            return false;
        slots_.erase(it);
        return true;
    }
    bool off(ListenerId id) { return unsubscribe(id); }

    void emit(const Event& event) {
        // Take a stable snapshot under lock, then invoke user code lock-free.
        // This supports nested emits and registration/removal from callbacks.
        std::vector<std::shared_ptr<Slot>> snapshot;
        {
            std::lock_guard lock(mutex_);
            snapshot = slots_;
        }
        for (const auto& slot : snapshot)
            if (slot->active.load(std::memory_order_acquire)) slot->callback(event);
    }

    [[nodiscard]] std::size_t listeners() const {
        std::lock_guard lock(mutex_);
        std::size_t count = 0;
        for (const auto& slot : slots_)
            if (slot->active.load(std::memory_order_acquire)) ++count;
        return count;
    }
private:
    struct Slot {
        ListenerId id{};
        Callback callback;
        std::atomic<bool> active{true};
        Slot(ListenerId listenerId, Callback listener)
            : id(listenerId), callback(std::move(listener)) {}
    };
    mutable std::mutex mutex_;
    std::vector<std::shared_ptr<Slot>> slots_;
    ListenerId nextId_ = 1;
};

// Compile-time event types = no runtime RTTI, string-name lookup or global state.
// Registration and removal are thread-safe. Callbacks execute synchronously on
// the thread which emits the event; adapters that need UI/game-thread affinity
// must explicitly marshal their callbacks. The dispatcher creates no workers.
template <class... EventTypes>
class EventBus : private EventChannel<EventTypes>... {
public:
    template <class Event, class Handler>
    auto on(Handler&& handler) -> typename EventChannel<Event>::ListenerId {
        return EventChannel<Event>::subscribe(
            typename EventChannel<Event>::Callback(std::forward<Handler>(handler)));
    }
    template <class Event>
    bool off(typename EventChannel<Event>::ListenerId id) {
        return EventChannel<Event>::unsubscribe(id);
    }
    template <class Event>
    void emit(const Event& event) {
        EventChannel<Event>::emit(event);
    }
    template <class Event>
    [[nodiscard]] std::size_t listeners() const {
        return EventChannel<Event>::listeners();
    }
};

using ClientEvents = EventBus<UpdateEvent, KeyEvent, NativeKeyEvent, MouseEvent,
                               FocusEvent, GameplayEvent, FovEvent,
                               ActorNameTagEvent, ViewNameTagEvent, LevelTickEvent>;

} // namespace bedrock
