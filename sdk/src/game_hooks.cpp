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
HookSubscription Hooks::onActorNameTag(std::function<void(const ActorNameTagEventValue&)> h) {
    impl_->dispatch.on<bedrock::ActorNameTagEvent>([h=std::move(h)](const bedrock::ActorNameTagEvent& e) {
        h({e.actor,std::string(e.nameTag)});
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

std::string hookRuntimeDiagnostic() {
    if (const char* diagnostic = BDS_HookRuntimeDiagnostic()) return diagnostic;
    return {};
}
#else
HookSubscription Hooks::onLevelTick(std::function<void(const LevelTickEventValue&)>) { return {}; }
HookSubscription Hooks::onGameplay(std::function<void(const GameplayEventValue&)>) { return {}; }
HookSubscription Hooks::onWorldChange(std::function<void(const WorldChangeEventValue&)>) { return {}; }
HookSubscription Hooks::onKey(std::function<void(const KeyEventValue&)>) { return {}; }
HookSubscription Hooks::onActorNameTag(std::function<void(const ActorNameTagEventValue&)>) { return {}; }
bool Hooks::install(std::string_view, std::string_view, std::string& error) { error = "native hooks unavailable"; return false; }
std::string hookRuntimeDiagnostic() { return {}; }
#endif

} // namespace bedrock::sdk
