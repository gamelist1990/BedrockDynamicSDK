#include <bedrock/sdk/game_hooks.hpp>
#ifdef _WIN32
#include <bedrock/detail/hook_dispatch.hpp>
#include <bedrock/runtime/game_hooks.hpp>
#endif
namespace bedrock::sdk {
struct HookSubscription::Impl{};
HookSubscription::HookSubscription()=default; HookSubscription::~HookSubscription()=default; HookSubscription::HookSubscription(HookSubscription&&) noexcept=default; HookSubscription& HookSubscription::operator=(HookSubscription&&) noexcept=default;
struct Hooks::Impl {
#ifdef _WIN32
 bedrock::detail::HookDispatch dispatch;
 std::unique_ptr<bedrock::GameHooks> native;
#endif
};
Hooks::Hooks():impl_(std::make_unique<Impl>()){} Hooks::~Hooks()=default;
#ifdef _WIN32
HookSubscription Hooks::onLevelTick(std::function<void(const LevelTickEventValue&)> h){impl_->dispatch.on<bedrock::LevelTickEvent>([h=std::move(h)](const bedrock::LevelTickEvent& e){h({e.level,e.sequence});});HookSubscription s;return s;} HookSubscription Hooks::onGameplay(std::function<void(const GameplayEventValue&)> h){impl_->dispatch.on<bedrock::GameplayEvent>([h=std::move(h)](const bedrock::GameplayEvent& e){h({e.clientInstance,e.active});});HookSubscription s;return s;} HookSubscription Hooks::onWorldChange(std::function<void(const WorldChangeEventValue&)> h){impl_->dispatch.on<bedrock::WorldChangeEvent>([h=std::move(h)](const bedrock::WorldChangeEvent& e){h({e.clientInstance,e.level,e.player});});HookSubscription s;return s;} HookSubscription Hooks::onKey(std::function<void(const KeyEventValue&)> h){impl_->dispatch.on<bedrock::KeyEvent>([h=std::move(h)](const bedrock::KeyEvent& e){h({static_cast<int>(e.inputKey()),e.pressed,e.cancelled()});});HookSubscription s;return s;}
bool Hooks::install(std::string_view,std::string_view,std::string& error){impl_->native=std::make_unique<bedrock::GameHooks>(impl_->dispatch);return true;}
std::string hookRuntimeDiagnostic(){return std::string{};}
#else
HookSubscription Hooks::onLevelTick(std::function<void(const LevelTickEventValue&)>) {return {};}
HookSubscription Hooks::onGameplay(std::function<void(const GameplayEventValue&)>) {return {};}
HookSubscription Hooks::onWorldChange(std::function<void(const WorldChangeEventValue&)>) {return {};}
HookSubscription Hooks::onKey(std::function<void(const KeyEventValue&)>) {return {};}
bool Hooks::install(std::string_view,std::string_view,std::string& error){error="native hooks unavailable";return false;} std::string hookRuntimeDiagnostic(){return{};}
#endif
}
