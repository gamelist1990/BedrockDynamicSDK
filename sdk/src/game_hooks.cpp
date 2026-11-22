#include <bedrock/sdk/game_hooks.hpp>
#include "resolver_access.hpp"
#include "roster_bridge.hpp"
#ifdef _WIN32
#include <bedrock/detail/hook_dispatch.hpp>
#include <bedrock/runtime/game_hooks.hpp>
#endif

namespace bedrock::sdk {

struct HookSubscription::Impl {};
HookSubscription::HookSubscription() = default;
HookSubscription::~HookSubscription() = default;
HookSubscription::HookSubscription(HookSubscription&&) noexcept = default;
HookSubscription& HookSubscription::operator=(HookSubscription&&) noexcept = default;

struct Hooks::Impl {
#ifdef _WIN32
    bedrock::detail::HookDispatch dispatch;
    std::unique_ptr<bedrock::GameHooks> native;
    std::string actorNameTagDiagnostic{"Actor::getNameTag not installed"};
    bool engaged{};
    Impl() = default;
    explicit Impl(bedrock::ClientAccess& access) : dispatch(access) {}
#endif
};

Hooks::Hooks() : impl_(std::make_unique<Impl>()) {}
Hooks::~Hooks() = default;

#ifdef _WIN32
void Hooks::ensureNative() noexcept {
    if (!impl_->native) impl_->native = std::make_unique<bedrock::GameHooks>(impl_->dispatch);
}

HookSubscription Hooks::onLevelTick(std::function<void(const LevelTickEventValue&)> h) {
    impl_->engaged = true;
    impl_->dispatch.on<bedrock::LevelTickEvent>([h=std::move(h)](const bedrock::LevelTickEvent& e) { h({e.level,e.sequence}); });
    return {};
}
HookSubscription Hooks::onGameplay(std::function<void(const GameplayEventValue&)> h) {
    impl_->engaged = true;
    impl_->dispatch.on<bedrock::GameplayEvent>([h=std::move(h)](const bedrock::GameplayEvent& e) { h({e.clientInstance,e.active}); });
    return {};
}
HookSubscription Hooks::onWorldChange(std::function<void(const WorldChangeEventValue&)> h) {
    impl_->engaged = true;
    impl_->dispatch.on<bedrock::WorldChangeEvent>([h=std::move(h)](const bedrock::WorldChangeEvent& e) {
        WorldChangeReasonValue reason{};
        switch (e.reason) {
        case bedrock::WorldChangeReason::Joined: reason = WorldChangeReasonValue::Joined; break;
        case bedrock::WorldChangeReason::Left: reason = WorldChangeReasonValue::Left; break;
        case bedrock::WorldChangeReason::JoinCancelled: reason = WorldChangeReasonValue::JoinCancelled; break;
        case bedrock::WorldChangeReason::DimensionChanged: reason = WorldChangeReasonValue::DimensionChanged; break;
        }
        h({reason, e.clientInstance, e.level, e.player});
    });
    return {};
}
HookSubscription Hooks::onKey(std::function<void(KeyEventValue&)> h) {
    impl_->engaged = true;
    impl_->dispatch.on<bedrock::KeyEvent>([h=std::move(h)](const bedrock::KeyEvent& e) {
        KeyEventValue v{static_cast<int>(e.inputKey()), e.pressed, e.cancelled()};
        h(v);
        if (v.cancelled) e.cancel();
    });
    return {};
}
HookSubscription Hooks::onMouse(std::function<void(MouseEventValue&)> h) {
    impl_->engaged = true;
    impl_->dispatch.on<bedrock::MouseEvent>([h=std::move(h)](const bedrock::MouseEvent& e) {
        MouseEventValue v{ e.action, e.state, e.synthetic, e.cancelled() };
        h(v);
        if (v.cancelled) e.cancel();
    });
    return {};
}
HookSubscription Hooks::onFov(std::function<void(FovEventValue&)> h) {
    impl_->engaged = true;
    impl_->dispatch.on<bedrock::FovEvent>([h=std::move(h)](const bedrock::FovEvent& e) {
        FovEventValue v{ e.resultFov };
        h(v);
        e.setResultFov(v.resultFov);
    });
    return {};
}
HookSubscription Hooks::onNativeKey(std::function<void(const NativeKeyEventValue&)> h) {
    impl_->engaged = true;
    impl_->dispatch.on<bedrock::NativeKeyEvent>([h=std::move(h)](const bedrock::NativeKeyEvent& e) {
        h({ e.inputKey, e.pressed });
    });
    return {};
}
HookSubscription Hooks::onActorNameTag(std::function<void(const ActorNameTagEventValue&)> h) {
    impl_->engaged = true;
    impl_->dispatch.on<bedrock::ActorNameTagEvent>([h=std::move(h)](const bedrock::ActorNameTagEvent& e) {
        h({ e.actor, std::string(e.nameTag) });
    });
    return {};
}


bool Hooks::install(std::string_view, std::string_view, std::string& error) {
    impl_->engaged = true;
    impl_->native = std::make_unique<bedrock::GameHooks>(impl_->dispatch);
    return true;
}

bool Hooks::install(const SymbolResolver& resolver, std::string_view profileJson, std::string& error) {
    impl_->engaged = true;
    impl_->native = std::make_unique<bedrock::GameHooks>(impl_->dispatch);
    const auto& nativeResolver = SymbolResolverRuntimeAccess::resolver(resolver);
    if (!impl_->native->installGameplay(nativeResolver, error)
        || !impl_->native->installLevelTick(nativeResolver, error)
        || !impl_->native->installWorldEvents(nativeResolver, profileJson, error)) return false;

    std::string nameError;
    if (!impl_->native->installActorNameTag(nativeResolver, nameError)) {
        impl_->actorNameTagDiagnostic = "Actor::getNameTag unavailable: " + nameError;
    } else {
        impl_->actorNameTagDiagnostic = "Actor::getNameTag shared hook installed";
    }
    error.clear();
    return true;
}

bool Hooks::bindGameAccess(void* nativeAccess, std::string& error) {
    if (boundAccess_) { error = "game access already bound"; return false; }
    if (impl_->engaged) {
        error = "hooks already engaged; bind the game access before subscribing or installing";
        return false;
    }
    if (!nativeAccess) { error = "game access native backend unavailable"; return false; }
    impl_ = std::make_unique<Impl>(*static_cast<bedrock::ClientAccess*>(nativeAccess));
    boundAccess_ = nativeAccess;
    return true;
}

void Hooks::stop() noexcept {
    if (impl_->native) impl_->native->stop();
}

void Hooks::clearObservedActors() noexcept {
    impl_->dispatch.clearObservedActors();
}

bool Hooks::installInput(const SymbolResolver& resolver, std::string& error) {
    impl_->engaged = true;
    ensureNative();
    const auto& nativeResolver = SymbolResolverRuntimeAccess::resolver(resolver);
    return impl_->native->installInput(nativeResolver, error);
}

bool Hooks::installFov(const SymbolResolver& resolver, std::string& error) {
    impl_->engaged = true;
    ensureNative();
    const auto& nativeResolver = SymbolResolverRuntimeAccess::resolver(resolver);
    return impl_->native->installFov(nativeResolver, error);
}

bool Hooks::installNativeKeyboard(const SymbolResolver& resolver, std::string_view profileJson, std::string& error) {
    impl_->engaged = true;
    ensureNative();
    const auto& nativeResolver = SymbolResolverRuntimeAccess::resolver(resolver);
    return impl_->native->installNativeKeyboard(nativeResolver, profileJson, error);
}

bool Hooks::installGameplay(const SymbolResolver& resolver, std::string& error) {
    impl_->engaged = true;
    ensureNative();
    const auto& nativeResolver = SymbolResolverRuntimeAccess::resolver(resolver);
    return impl_->native->installGameplay(nativeResolver, error);
}

bool Hooks::installLevelTick(const SymbolResolver& resolver, std::string& error) {
    impl_->engaged = true;
    ensureNative();
    const auto& nativeResolver = SymbolResolverRuntimeAccess::resolver(resolver);
    return impl_->native->installLevelTick(nativeResolver, error);
}

bool Hooks::sendNativeKey(unsigned key, bool pressed) {
    impl_->engaged = true;
    return impl_->dispatch.sendNativeKey(key, pressed);
}

bool Hooks::sendMouseInput(unsigned action, bool pressed) {
    impl_->engaged = true;
    return impl_->dispatch.sendMouseInput(action, pressed);
}

bool Hooks::sendMouseMotion(int dx, int dy) {
    impl_->engaged = true;
    return impl_->dispatch.sendMouseMotion(
        static_cast<std::int16_t>(std::clamp(dx, -32768, 32767)),
        static_cast<std::int16_t>(std::clamp(dy, -32768, 32767)));
}

bool Hooks::gameplayActive() const noexcept {
    return impl_->dispatch.gameplayActive();
}

bool Hooks::installWorldEvents(const SymbolResolver& resolver, std::string_view profileJson, std::string& error) {
    impl_->engaged = true;
    ensureNative();
    const auto& nativeResolver = SymbolResolverRuntimeAccess::resolver(resolver);
    return impl_->native->installWorldEvents(nativeResolver, profileJson, error);
}

void Hooks::pinWorldEventClient(void* client) {
    impl_->dispatch.setWorldEventClient(client);
}

bool Hooks::sendKeyboardFeed(unsigned key, bool pressed) {
    impl_->engaged = true;
    // Wraps BDS_HookCallOriginal(BDS_HOOK_KEYBOARD_FEED, &args) for the
    // specific capability freecam needs when releasing movement keys on toggle.
    // This is the narrowest possible surface for the keyboard feed hook; it
    // preserves the exact semantics of calling the original keyboard feed
    // handler rather than going through the event bus.
#ifdef _WIN32
    BDS_KeyboardArgs args{static_cast<int>(key), pressed ? 1 : 0, 0};
    return BDS_HookCallOriginal(BDS_HOOK_KEYBOARD_FEED, &args) != 0;
#else
    (void)key; (void)pressed; return false;
#endif
}

std::string hookRuntimeDiagnostic() {
    if (const char* diagnostic = BDS_HookRuntimeDiagnostic()) return diagnostic;
    return {};
}
#else
HookSubscription Hooks::onLevelTick(std::function<void(const LevelTickEventValue&)>) { return {}; }
HookSubscription Hooks::onGameplay(std::function<void(const GameplayEventValue&)>) { return {}; }
HookSubscription Hooks::onWorldChange(std::function<void(const WorldChangeEventValue&)>) { return {}; }
HookSubscription Hooks::onKey(std::function<void(KeyEventValue&)>) { return {}; }
HookSubscription Hooks::onMouse(std::function<void(MouseEventValue&)>) { return {}; }
HookSubscription Hooks::onFov(std::function<void(FovEventValue&)>) { return {}; }
HookSubscription Hooks::onNativeKey(std::function<void(const NativeKeyEventValue&)>) { return {}; }
HookSubscription Hooks::onActorNameTag(std::function<void(const ActorNameTagEventValue&)>) { return {}; }
bool Hooks::install(std::string_view, std::string_view, std::string& error) { error = "native hooks unavailable"; return false; }
bool Hooks::install(const SymbolResolver&, std::string_view, std::string& error) { error = "native hooks unavailable"; return false; }
bool Hooks::installInput(const SymbolResolver&, std::string& error) { error = "native hooks unavailable"; return false; }
bool Hooks::installFov(const SymbolResolver&, std::string& error) { error = "native hooks unavailable"; return false; }
bool Hooks::installNativeKeyboard(const SymbolResolver&, std::string_view, std::string& error) { error = "native hooks unavailable"; return false; }
bool Hooks::installGameplay(const SymbolResolver&, std::string& error) { error = "native hooks unavailable"; return false; }
bool Hooks::installLevelTick(const SymbolResolver&, std::string& error) { error = "native hooks unavailable"; return false; }
bool Hooks::sendNativeKey(unsigned, bool) { return false; }
bool Hooks::sendMouseInput(unsigned, bool) { return false; }
bool Hooks::sendMouseMotion(int, int) { return false; }
bool Hooks::gameplayActive() const noexcept { return false; }
bool Hooks::installWorldEvents(const SymbolResolver&, std::string_view, std::string& error) { error = "native hooks unavailable"; return false; }
void Hooks::pinWorldEventClient(void*) {}
bool Hooks::bindGameAccess(void*, std::string& error) { error = "native hooks unavailable"; return false; }
void Hooks::stop() noexcept {}
std::string hookRuntimeDiagnostic() { return {}; }
#endif

} // namespace bedrock::sdk