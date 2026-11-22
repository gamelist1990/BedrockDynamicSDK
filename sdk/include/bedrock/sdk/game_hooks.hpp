#pragma once
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <cstdint>
#include "resolver.hpp"

namespace bedrock::sdk {

class RosterAccess;

struct LevelTickEventValue { void* level{}; std::uint64_t sequence{}; };
struct GameplayEventValue { void* clientInstance{}; bool active{}; };
enum class WorldChangeReasonValue : std::uint8_t { Joined, Left, JoinCancelled, DimensionChanged };
struct WorldChangeEventValue { WorldChangeReasonValue reason{}; void* clientInstance{}; void* level{}; void* player{}; };
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
    // The handler receives the keyboard-feed event by mutable reference:
    // setting `cancelled` on the value cancels the feed for the game,
    // mirroring onMouse/onFov write-back. Handlers that only read may
    // still take the value by const reference.
    HookSubscription onKey(std::function<void(KeyEventValue&)>);
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
    bool installWorldEvents(const SymbolResolver& resolver, std::string_view profileJson, std::string& error);
    bool installLevelTick(const SymbolResolver& resolver, std::string& error);

    bool sendNativeKey(unsigned key, bool pressed);
    bool sendKeyboardFeed(unsigned key, bool pressed);
    // Synthesize a mouse button event through the native input path
    // (freecam releases the buttons it swallowed when it toggles off).
    bool sendMouseInput(unsigned action, bool pressed);
    // Synthetic relative pointer motion through the mouse feed (the
    // motion counterpart of sendMouseInput).
    bool sendMouseMotion(int dx, int dy);
    // Whether the gameplay scope is currently active, as tracked by the
    // hook dispatcher (the native gameplayActive signal).
    [[nodiscard]] bool gameplayActive() const noexcept;
    // Pin the client instance the world-change events bind to
    // (split-screen disambiguation; freecam pins from its gameplay event).
    void pinWorldEventClient(void* client);

    // Bind this Hooks' event dispatch to a RosterAccess' native game
    // access, so the access' LevelTick scope opens inside this Hooks'
    // LevelTick handlers and its reads (equipmentItem, localInventoryItem,
    // actorIdentity, ...) see the world — the native pattern of a
    // HookDispatch constructed with the access. Must be called before any
    // subscription or install on this Hooks; returns false otherwise. The
    // RosterAccess must outlive this Hooks.
    bool useGameAccess(RosterAccess& access, std::string& error);
    // The access bound by useGameAccess, or nullptr.
    RosterAccess* gameAccess() noexcept { return gameAccess_; }

    // Uninstall all native hooks while preserving subscriptions/impl state.
    void stop() noexcept;

    // Clear any cached actor name-tag state maintained by the hook
    // dispatcher. TabList uses this when the world identity changes so
    // actors are re-queried for their name tags. Lives in Hooks (not
    // RosterAccess) because it clears HookDispatch's own observed-actor
    // cache, which is shared by the Hooks ActorNameTagEvent subscribers.
    void clearObservedActors() noexcept;

private:
    // Binding step used by useGameAccess (defined in roster.cpp, where the
    // native game access type is complete): recreates the dispatch around
    // the given native ClientAccess. Kept free of any RosterAccess /
    // NativeGameAccess reference so Hooks never drags the roster TU (and
    // its native dependencies) into SDK-only links.
    bool bindGameAccess(void* nativeAccess, std::string& error);
    struct Impl;
    std::unique_ptr<Impl> impl_;
    RosterAccess* gameAccess_{};
    void* boundAccess_{};
    void ensureNative() noexcept;  // Lazily creates the native hook broker before any install touches it.
};

std::string hookRuntimeDiagnostic();

} // namespace bedrock::sdk