#include <bedrock/sdk/game_hooks.hpp>
#include "resolver_access.hpp"
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
#endif
};

Hooks::Hooks() : impl_(std::make_unique<Impl>()) {}
Hooks::~Hooks() = default;

#ifdef _WIN32
void Hooks::ensureNative() noexcept {
    if (!impl_->native) impl_->native = std::make_unique<bedrock::GameHooks>(impl_->dispatch);
}

HookSubscription Hooks::onLevelTick(std::function<void(const LevelTickEventValue&)> h) {
    impl_->dispatch.on<bedrock::LevelTickEvent>([h=std::move(h)](const bedrock::LevelTickEvent& e) { h({e.level,e.sequence}); });
    return {};
}
HookSubscription Hooks::onGameplay(std::function<void(const GameplayEventValue&)> h) {
    impl_->dispatch.on<bedrock::GameplayEvent>([h=std::move(h)](const bedrock::GameplayEvent& e) { h({e.clientInstance,e.active}); });
    return {};
}
HookSubscription Hooks::onWorldChange(std::function<void(const WorldChangeEventValue&)> h) {
    impl_->dispatch.on<bedrock::WorldChangeEvent>([h=std::move(h)](const bedrock::WorldChangeEvent& e) { h({e.clientInstance,e.level,e.player}); });
    return {};
}
HookSubscription Hooks::onKey(std::function<void(const KeyEventValue&)> h) {
    impl_->dispatch.on<bedrock::KeyEvent>([h=std::move(h)](const bedrock::KeyEvent& e) { h({static_cast<int>(e.inputKey()),e.pressed,e.cancelled()}); });
    return {};
}
HookSubscription Hooks::onMouse(std::function<void(MouseEventValue&)> h) {
    impl_->dispatch.on<bedrock::MouseEvent>([h=std::move(h)](const bedrock::MouseEvent& e) {
        MouseEventValue v{ e.action, e.state, e.synthetic, e.cancelled() };
        h(v);
        if (v.cancelled) e.cancel();
    });
    return {};
}
HookSubscription Hooks::onFov(std::function<void(FovEventValue&)> h) {
    impl_->dispatch.on<bedrock::FovEvent>([h=std::move(h)](const bedrock::FovEvent& e) {
        FovEventValue v{ e.resultFov };
        h(v);
        e.setResultFov(v.resultFov);
    });
    return {};
}
HookSubscription Hooks::onNativeKey(std::function<void(const NativeKeyEventValue&)> h) {
    impl_->dispatch.on<bedrock::NativeKeyEvent>([h=std::move(h)](const bedrock::NativeKeyEvent& e) {
        h({ e.inputKey, e.pressed });
    });
    return {};
}
HookSubscription Hooks::onActorNameTag(std::function<void(const ActorNameTagEventValue&)> h) {
    impl_->dispatch.on<bedrock::ActorNameTagEvent>([h=std::move(h)](const bedrock::ActorNameTagEvent& e) {
        h({ e.actor, std::string(e.nameTag) });
    });
    return {};
}

bool Hooks::install(std::string_view, std::string_view, std::string& error) {
    impl_->native = std::make_unique<bedrock::GameHooks>(impl_->dispatch);
    return true;
}

bool Hooks::install(const SymbolResolver& resolver, std::string_view profileJson, std::string& error) {
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

void Hooks::stop() noexcept {
    if (impl_->native) impl_->native->stop();
}

bool Hooks::installInput(const SymbolResolver& resolver, std::string& error) {
    ensureNative();
    const auto& nativeResolver = SymbolResolverRuntimeAccess::resolver(resolver);
    return impl_->native->installInput(nativeResolver, error);
}

bool Hooks::installFov(const SymbolResolver& resolver, std::string& error) {
    ensureNative();
    const auto& nativeResolver = SymbolResolverRuntimeAccess::resolver(resolver);
    return impl_->native->installFov(nativeResolver, error);
}

bool Hooks::installNativeKeyboard(const SymbolResolver& resolver, std::string_view profileJson, std::string& error) {
    ensureNative();
    const auto& nativeResolver = SymbolResolverRuntimeAccess::resolver(resolver);
    return impl_->native->installNativeKeyboard(nativeResolver, profileJson, error);
}

bool Hooks::installGameplay(const SymbolResolver& resolver, std::string& error) {
    ensureNative();
    const auto& nativeResolver = SymbolResolverRuntimeAccess::resolver(resolver);
    return impl_->native->installGameplay(nativeResolver, error);
}

bool Hooks::installLevelTick(const SymbolResolver& resolver, std::string& error) {
    ensureNative();
    const auto& nativeResolver = SymbolResolverRuntimeAccess::resolver(resolver);
    return impl_->native->installLevelTick(nativeResolver, error);
}

bool Hooks::sendNativeKey(unsigned key, bool pressed) {
    return impl_->dispatch.sendNativeKey(key, pressed);
}

std::string hookRuntimeDiagnostic() {
    if (const char* diagnostic = BDS_HookRuntimeDiagnostic()) return diagnostic;
    return {};
}
#else
HookSubscription Hooks::onLevelTick(std::function<void(const LevelTickEventValue&)>) { return {}; }
HookSubscription Hooks::onGameplay(std::function<void(const GameplayEventValue&)>) { return {}; }
HookSubscription Hooks::onWorldChange(std::function<void(const WorldChangeEventValue&)>) { return {}; }
HookSubscription Hooks::onKey(std::function<void(const KeyEventValue&)>) { return {}; }
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
void Hooks::stop() noexcept {}
std::string hookRuntimeDiagnostic() { return {}; }
#endif

} // namespace bedrock::sdk