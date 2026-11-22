#pragma once
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <cstdint>
#include "resolver.hpp"

namespace bedrock::sdk {

struct LevelTickEventValue { void* level{}; std::uint64_t sequence{}; };
struct GameplayEventValue { void* clientInstance{}; bool active{}; };
struct WorldChangeEventValue { void* clientInstance{}; void* level{}; void* player{}; };
struct KeyEventValue { int key{}; bool pressed{}; bool cancelled{}; };

// Mirror of internal MouseEvent: action, state, synthetic, cancellation
struct MouseEventValue {
    std::int8_t action{};
    std::int8_t state{};
    bool synthetic{};
    bool cancelled{};
};

// Mirror of internal FovEvent: resultFov (mutable read-back)
struct FovEventValue {
    float resultFov{};
};

// Mirror of internal NativeKeyEvent: inputKey, pressed
struct NativeKeyEventValue {
    unsigned inputKey{};
    bool pressed{};
};

class HookSubscription {
public:
    HookSubscription();
    ~HookSubscription();
    HookSubscription(HookSubscription&&) noexcept;
    HookSubscription& operator=(HookSubscription&&) noexcept;
    HookSubscription(const HookSubscription&) = delete;
    HookSubscription& operator=(const HookSubscription&) = delete;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    friend class Hooks;
};

class Hooks {
public:
    struct ActorNameTagEventValue { void* actor{}; std::string nameTag; };
    Hooks();
    ~Hooks();
    Hooks(const Hooks&) = delete;
    Hooks& operator=(const Hooks&) = delete;

    HookSubscription onLevelTick(std::function<void(const LevelTickEventValue&)>);
    HookSubscription onGameplay(std::function<void(const GameplayEventValue&)>);
    HookSubscription onWorldChange(std::function<void(const WorldChangeEventValue&)>);
    HookSubscription onKey(std::function<void(const KeyEventValue&)>);
    HookSubscription onMouse(std::function<void(MouseEventValue&)>);
    HookSubscription onFov(std::function<void(FovEventValue&)>);
    HookSubscription onNativeKey(std::function<void(const NativeKeyEventValue&)>);

    HookSubscription onActorNameTag(std::function<void(const ActorNameTagEventValue&)>);


    bool install(const SymbolResolver& resolver, std::string_view profileJson, std::string& error);
    bool install(std::string_view selectedProfileJson, std::string_view embeddedProfileJson, std::string& error);

    // Granular installs — each composes with the monolithic install()
    bool installInput(const SymbolResolver& resolver, std::string& error);
    bool installFov(const SymbolResolver& resolver, std::string& error);
    bool installNativeKeyboard(const SymbolResolver& resolver, std::string_view profileJson, std::string& error);
    bool installGameplay(const SymbolResolver& resolver, std::string& error);
    bool installLevelTick(const SymbolResolver& resolver, std::string& error);

    bool sendNativeKey(unsigned key, bool pressed);
    bool sendKeyboardFeed(unsigned key, bool pressed);

    // Uninstall all native hooks while preserving subscriptions/impl state.
    void stop() noexcept;

    // Clear any cached actor name-tag state maintained by the hook
    // dispatcher. TabList uses this when the world identity changes so
    // actors are re-queried for their name tags. Lives in Hooks (not
    // RosterAccess) because it clears HookDispatch's own observed-actor
    // cache, which is shared by the Hooks ActorNameTagEvent subscribers.
    void clearObservedActors() noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    void ensureNative() noexcept;  // Lazily creates the native hook broker before any install touches it.
};

std::string hookRuntimeDiagnostic();

} // namespace bedrock::sdk