#pragma once

#include <algorithm>
#include <cstddef>
#include <functional>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

namespace bedrock::sdk {

class Subscription {
public:
    Subscription() = default;
    Subscription(std::function<void()> cancel) : cancel_(std::move(cancel)) {}
    Subscription(const Subscription&) = delete;
    Subscription& operator=(const Subscription&) = delete;
    Subscription(Subscription&& other) noexcept : cancel_(std::move(other.cancel_)) {}
    Subscription& operator=(Subscription&& other) noexcept {
        if (this != &other) { reset(); cancel_ = std::move(other.cancel_); }
        return *this;
    }
    ~Subscription() { reset(); }
    void reset() noexcept { if (cancel_) { cancel_(); cancel_ = {}; } }
private:
    std::function<void()> cancel_;
};

template<class... Args>
class EventSignal {
public:
    using Handler = std::function<void(Args...)>;

    EventSignal() = default;
    EventSignal(const EventSignal&) = delete;
    EventSignal& operator=(const EventSignal&) = delete;

    [[nodiscard]] Subscription operator+=(Handler handler) {
        return subscribe(std::move(handler));
    }

    [[nodiscard]]
    Subscription subscribe(Handler handler) {
        auto entry = std::make_shared<Handler>(std::move(handler));
        Subscription subscription([weak = std::weak_ptr<State>(state_), entry] {
            if (const auto state = weak.lock()) {
                std::lock_guard lock(state->mutex);
                std::erase(state->handlers, entry);
            }
        });
        std::lock_guard lock(state_->mutex);
        state_->handlers.push_back(std::move(entry));
        return subscription;
    }

    // Unsubscription affects future publications, not a dispatch already in progress.
    void publish(Args... args) {
        std::vector<std::shared_ptr<Handler>> copy;
        {
            std::lock_guard lock(state_->mutex);
            copy = state_->handlers;
        }
        for (const auto& handler : copy) (*handler)(args...);
    }

private:
    struct State {
        std::mutex mutex;
        std::vector<std::shared_ptr<Handler>> handlers;
    };
    std::shared_ptr<State> state_ = std::make_shared<State>();
};

template<class Event>
using Events = EventSignal<const Event&>;

} // namespace bedrock::sdk
