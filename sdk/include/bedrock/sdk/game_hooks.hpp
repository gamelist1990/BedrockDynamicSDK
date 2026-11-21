#pragma once
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <cstdint>
namespace bedrock::sdk {
struct LevelTickEventValue { void* level{}; std::uint64_t sequence{}; };
struct GameplayEventValue { void* clientInstance{}; bool active{}; };
struct WorldChangeEventValue { void* clientInstance{}; void* level{}; void* player{}; };
struct KeyEventValue { int key{}; bool pressed{}; bool cancelled{}; };
class HookSubscription { public:
 HookSubscription(); ~HookSubscription(); HookSubscription(HookSubscription&&) noexcept; HookSubscription& operator=(HookSubscription&&) noexcept;
 HookSubscription(const HookSubscription&) = delete; HookSubscription& operator=(const HookSubscription&) = delete;
 private: struct Impl; std::unique_ptr<Impl> impl_; friend class Hooks;
};
class Hooks { public:
 Hooks(); ~Hooks(); Hooks(const Hooks&) = delete; Hooks& operator=(const Hooks&) = delete;
 HookSubscription onLevelTick(std::function<void(const LevelTickEventValue&)>);
 HookSubscription onGameplay(std::function<void(const GameplayEventValue&)>);
 HookSubscription onWorldChange(std::function<void(const WorldChangeEventValue&)>);
 HookSubscription onKey(std::function<void(const KeyEventValue&)>);
 bool install(std::string_view selectedProfileJson, std::string_view embeddedProfileJson, std::string& error);
 private: struct Impl; std::unique_ptr<Impl> impl_;
};
std::string hookRuntimeDiagnostic();
}
