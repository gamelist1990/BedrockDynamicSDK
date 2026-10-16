#pragma once
#include <atomic>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include "bedrock/resolver.hpp"

namespace bedrock {

// Ordinary keyboard bindings used by the minimal example, not game ABI offsets.
// Custom keybinds/controller require an independently validated input adapter.
enum class Key : unsigned { Forward = 'W', Back = 'S', Left = 'A', Right = 'D',
                            Sprint = 0xA2, Sneak = 0xA0 };

inline constexpr bool shouldSprint(bool playing, bool forward, bool sneaking) noexcept {
    return playing && forward && !sneaking;
}

// Convenient input facade. Sends *ordinary Windows keyboard events*; never writes
// to Minecraft Actor/ClientInstance memory and never assumes object field offsets.
class Input {
public:
    Input() = default;
    Input(const Input&) = delete;
    Input& operator=(const Input&) = delete;
    bool down(Key key) const noexcept;
    bool forward() const noexcept { return down(Key::Forward); }
    bool sneaking() const noexcept;
    void sprint(bool wantSprint) noexcept;
    void autoSprint() noexcept;
    void release() noexcept;
private:
    bool ownSprint_{};
};

class Client {
public:
    Client(Resolver& resolver, std::string version) :
        resolver_(resolver), version_(std::move(version)) {}
    [[nodiscard]] std::string_view version() const noexcept { return version_; }
    [[nodiscard]] bool inGame() const noexcept;
    [[nodiscard]] bool focused() const noexcept;
    [[nodiscard]] Input& input() noexcept { return input_; }
    [[nodiscard]] const Input& input() const noexcept { return input_; }

    // Advanced escape hatch for feature modules; candidate is never ABI verified.
    [[nodiscard]] std::optional<Resolution> symbol(
        std::string_view name, bool allowCandidate = false) const {
        return resolver_.resolve(name, allowCandidate);
    }
private:
    Resolver& resolver_;
    std::string version_;
    Input input_{};
};

enum class ModStatus : unsigned {
    NotStarted=0, Starting=1, Running=2, Unsupported=3, Stopped=4, Failed=5
};

// SDK-managed manifest fetch, version selection, 20ms input loop and cleanup.
// Tick handlers run on the worker thread, not Minecraft's render/game thread.
using TickHandler = void (*)(Client&);

class ModRuntime {
public:
    ModRuntime() = default;
    ModRuntime(const ModRuntime&) = delete;
    ModRuntime& operator=(const ModRuntime&) = delete;
    bool start(TickHandler handler) noexcept;
    void stop() noexcept { stopping_.store(true, std::memory_order_release); }
    [[nodiscard]] ModStatus status() const noexcept { return status_.load(); }
private:
    void run() noexcept;
#ifdef _WIN32
    static unsigned long __stdcall threadEntry(void* state);
#endif
    TickHandler callback_{};
    std::atomic<bool> stopping_{false};
    std::atomic<ModStatus> status_{ModStatus::NotStarted};
};

} // namespace bedrock
