#pragma once
// Portable Win32 message observer for Latite-like keyboard input.
// Feed actual HWND/WndProc messages here from the application's GUI thread.
// This class DOES NOT hook an OS window or intercept messages by itself.
#include <array>
#include <atomic>
#include <cstdint>

namespace bedrock {

class KeyboardObserver {
public:
    static constexpr std::uint32_t KeyDown = 0x0100;   // WM_KEYDOWN
    static constexpr std::uint32_t KeyUp = 0x0101;     // WM_KEYUP
    static constexpr std::uint32_t Char = 0x0102;      // WM_CHAR
    static constexpr std::uint32_t SysKeyDown = 0x0104;// WM_SYSKEYDOWN (Alt)
    static constexpr std::uint32_t SysKeyUp = 0x0105;  // WM_SYSKEYUP
    static constexpr std::uint32_t KillFocus = 0x0008; // WM_KILLFOCUS

    using KeyHandler = void (*)(int virtualKey, bool down, bool repeated, void* userdata);

    KeyboardObserver() noexcept {
        clear();
    }
    KeyboardObserver(const KeyboardObserver&) = delete;
    KeyboardObserver& operator=(const KeyboardObserver&) = delete;

    void setKeyHandler(KeyHandler handler, void* userdata = nullptr) noexcept {
        handler_.store(handler, std::memory_order_release);
        userdata_.store(userdata, std::memory_order_release);
    }
    void onWindowMessage(std::uint32_t message, std::uintptr_t wParam,
                         std::intptr_t lParam = 0) noexcept {
        if (message == KillFocus) {
            clear();
            return;
        }
        const bool down = message == KeyDown || message == SysKeyDown;
        const bool up = message == KeyUp || message == SysKeyUp;
        if (!down && !up) return;
        // wParam uses Windows VK enum. Latite keyboard records 0x100 key states.
        if (wParam >= 256) return;
        const auto key = static_cast<std::size_t>(wParam);
        const bool old = states_[key].exchange(down, std::memory_order_acq_rel);
        const bool repeated = down && (old || ((static_cast<std::uintptr_t>(lParam) >> 30) & 1));
        // Deliver only state transitions to avoid duplicate repeat-key toggles.
        if (old == down) return;
        auto handler = handler_.load(std::memory_order_acquire);
        if (handler) handler(static_cast<int>(key), down, repeated,
                             userdata_.load(std::memory_order_acquire));
    }
    bool isDown(int virtualKey) const noexcept {
        return virtualKey >= 0 && virtualKey < 256 &&
            states_[static_cast<std::size_t>(virtualKey)].load(std::memory_order_acquire);
    }
    void clear() noexcept {
        for (auto& key : states_) key.store(false, std::memory_order_release);
    }

private:
    std::array<std::atomic<bool>, 256> states_{};
    std::atomic<KeyHandler> handler_{nullptr};
    std::atomic<void*> userdata_{nullptr};
};

} // namespace bedrock
