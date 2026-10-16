#pragma once
// Lightweight, header-only, typed event dispatcher.
// No Minecraft binary offsets, Win32 headers, hooks, JSON, downloads, or threads.
// The event source belongs to the client (or an independently validated adapter).
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <vector>

namespace bedrock {

struct UpdateEvent {
    // Emitted by a client-owned update source. NOT necessarily a Minecraft game tick.
    std::uint64_t sequence{};
};
struct KeyEvent {
    // Virtual-key code; the client decides the input source.
    unsigned virtualKey{};
    bool pressed{};
    bool repeat{};
};
struct FocusEvent { bool focused{}; };
// Emitted by a platform adapter when Minecraft is actively accepting gameplay
// input (e.g. its window is focused and the gameplay cursor is captured).
struct GameplayEvent { bool active{}; };

template <class Event>
class EventChannel {
public:
    using Callback = std::function<void(const Event&)>;
    using ListenerId = std::uint64_t;

    ListenerId subscribe(Callback callback) {
        if (!callback) throw std::invalid_argument("event handler must be callable");
        const ListenerId id = nextId_++;
        auto slot = Slot{id, std::move(callback), true};
        if (dispatchDepth_) pending_.push_back(std::move(slot));
        else slots_.push_back(std::move(slot));
        return id;
    }

    bool unsubscribe(ListenerId id) {
        for (auto& slot : slots_) {
            if (slot.id == id && slot.active) {
                slot.active = false;
                if (!dispatchDepth_) compact();
                return true;
            }
        }
        for (auto& slot : pending_) {
            if (slot.id == id && slot.active) {
                slot.active = false;
                return true;
            }
        }
        return false;
    }

    void emit(const Event& event) {
        // Registration/removal during a callback is deferred, avoiding iterators
        // and callback ownership invalidation; nested emits are supported.
        struct DispatchGuard {
            EventChannel& channel;
            explicit DispatchGuard(EventChannel& c) : channel(c) { ++channel.dispatchDepth_; }
            ~DispatchGuard() {
                if (!--channel.dispatchDepth_) channel.compact();
            }
        } guard(*this);
        for (auto& slot : slots_)
            if (slot.active) slot.callback(event);
    }

    [[nodiscard]] std::size_t listeners() const noexcept {
        std::size_t count = 0;
        for (const auto& slot : slots_) if (slot.active) ++count;
        for (const auto& slot : pending_) if (slot.active) ++count;
        return count;
    }
private:
    struct Slot {
        ListenerId id{};
        Callback callback;
        bool active{};
    };
    void compact() {
        std::erase_if(slots_, [](const Slot& s) { return !s.active; });
        for (auto& slot : pending_)
            if (slot.active) slots_.push_back(std::move(slot));
        pending_.clear();
    }
    std::vector<Slot> slots_;
    std::vector<Slot> pending_;
    ListenerId nextId_ = 1;
    unsigned dispatchDepth_ = 0;
};

// Compile-time event types = no runtime RTTI, string-name lookup or global state.
// Subscribe and dispatch on ONE owner thread. Any cross-thread event source should
// marshal events to the owner thread; there are no hidden worker threads.
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

using ClientEvents = EventBus<UpdateEvent, KeyEvent, FocusEvent, GameplayEvent>;

} // namespace bedrock
