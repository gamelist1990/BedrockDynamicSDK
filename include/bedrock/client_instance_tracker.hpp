#pragma once
#include <atomic>
#include <cstddef>
#include <cstdint>

namespace bedrock {

// A passive, type-erased ClientInstance* recorder.
// A pointer is first observed as the 'this' parameter of confirmed client methods;
// this class never calls a Minecraft function or uses an SDK field offset.
class ClientInstanceTracker {
public:
    using Validator = bool (*)(const void* candidate, void* userdata) noexcept;
    explicit ClientInstanceTracker(Validator validator = nullptr, void* context = nullptr) noexcept
        : validate_(validator), context_(context) {}

    ClientInstanceTracker(const ClientInstanceTracker&) = delete;
    ClientInstanceTracker& operator=(const ClientInstanceTracker&) = delete;

    // Called by an installed detour when the game naturally invokes the function.
    bool observe(void* instance) noexcept {
        if (!instance || (validate_ && !validate_(instance, context_)))
            return false;
        instance_.store(instance, std::memory_order_release);
        captures_.fetch_add(1, std::memory_order_relaxed);
        return true;
    }

    // Non-owning pointer. Validity must be checked again by the caller before use.
    void* get() const noexcept {
        void* value = instance_.load(std::memory_order_acquire);
        if (!value) return nullptr;
        if (validate_ && !validate_(value, context_)) {
            void* expected = value;
            instance_.compare_exchange_strong(expected, nullptr, std::memory_order_acq_rel);
            return nullptr;
        }
        return value;
    }

    void clear() noexcept { instance_.store(nullptr, std::memory_order_release); }
    std::uint64_t captures() const noexcept { return captures_.load(std::memory_order_acquire); }

private:
    Validator validate_{};
    void* context_{};
    mutable std::atomic<void*> instance_{nullptr};
    std::atomic<std::uint64_t> captures_{0};
};

} // namespace bedrock
