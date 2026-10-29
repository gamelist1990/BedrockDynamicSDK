#pragma once
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace bedrock {
// Versioned native callback context layout, loaded from an exact-build profile.
struct NativeKeyboardLayout {
    std::size_t ownerOffset{}, processorOffset{}, messageOffset{}, keyOffset{};
    std::size_t keyDownSlot{}, keyUpSlot{};
    int hardwareOrigin{};
};
std::optional<NativeKeyboardLayout> parseNativeKeyboardLayout(
    std::string_view profile, std::string& error);

// Calling the prefilter named Keyboard::feed alone does not reach gameplay.
// Capture this route from a real GameCore::keyboardEvent callback instead.
struct NativeKeyboardInput {
#ifdef _WIN32
    using KeyDown = void(__fastcall*)(void*, int, int);
    using KeyUp = void(__fastcall*)(void*, int);
#else
    using KeyDown = void(*)(void*, int, int);
    using KeyUp = void(*)(void*, int);
#endif
    void* processor{};
    KeyDown keyDown{};
    KeyUp keyUp{};
    int hardwareOrigin{};
    bool send(int key, bool down) const {
        if (!processor || !keyDown || !keyUp || key < 0 || key >= 256) return false;
        if (down) keyDown(processor, key, hardwareOrigin);
        else keyUp(processor, key);
        return true;
    }
};
struct NativeKeyboardEvent {
    NativeKeyboardInput input;
    int key{};
    bool down{};
};
std::optional<NativeKeyboardEvent> captureNativeKeyboardEvent(
    const void* context, const NativeKeyboardLayout& layout);
} // namespace bedrock
