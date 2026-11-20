#pragma once

#include "events.hpp"
#include "settings.hpp"

namespace bedrock::sdk {

enum class Key : unsigned {
    Tab = 9, Space = 32,
    Digit0 = '0', Digit1, Digit2, Digit3, Digit4, Digit5, Digit6, Digit7, Digit8, Digit9,
    A = 'A', B, C, D, E, F, G, H, I, J, K, L, M, N, O, P, Q, R, S, T, U, V, W, X, Y, Z,
    F1 = 112, F2, F3, F4, F5, F6, F7, F8, F9, F10, F11, F12,
};

struct KeybindEvent {
    std::string_view id; // Borrowed for the duration of the callback.
    bool pressed;
};

class KeyBindings {
public:
    explicit KeyBindings(std::string modId);
    ~KeyBindings();
    KeyBindings(const KeyBindings&) = delete;
    KeyBindings& operator=(const KeyBindings&) = delete;

    Result<void> add(std::string id, std::string labelKey, Key defaultKey);
    void locale(std::string id, std::vector<Translation> entries);
    [[nodiscard]] std::optional<Key> key(std::string_view id) const;
    Events<KeybindEvent> changed;

    // Runtime adapter entry point: deliver gameplay input transitions only.
    // Returns matching action IDs with repeated presses filtered out.
    [[nodiscard]] std::vector<std::string> dispatch(Key key, bool pressed);
    void clearInput();

private:
    friend class Mod;
    Result<void> connect();
    Result<void> disconnect();

    struct State;
    std::shared_ptr<State> state_;
    SettingsPage page_;
    bool connected_{};
};

} // namespace bedrock::sdk
