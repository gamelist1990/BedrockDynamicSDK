#include <bedrock/sdk/actors.hpp>
#include "resolver_access.hpp"
#ifdef _WIN32
#include <bedrock/runtime/hook_runtime.h>
#include <bedrock/native/actors.hpp>
#include <bedrock/native/items.hpp>
#include <bedrock/native/actor_registry.hpp>
#include <bedrock/native/local_player.hpp>
#include <bedrock/native/player_list.hpp>
#endif
#include <chrono>
#include <utility>

namespace bedrock::sdk {

struct ActorFacade::Impl {
#ifdef _WIN32
    std::uintptr_t nameTagAddress{};
    bedrock::NativeActorRegistry registry;
    bedrock::NativeLocalPlayerGetter localPlayer;
    bedrock::NativePlayerList playerList;
    bedrock::NativeItemLayout itemLayout;
    bedrock::NativeActorLayout actorLayout;
#endif
};

ActorFacade::ActorFacade() : impl_(std::make_unique<Impl>()) {}
ActorFacade::~ActorFacade() = default;

bool ActorFacade::configure(std::string_view profileJson,
                            std::size_t actorTypeValueOffset,
                            std::uint32_t playerTypeId,
                            std::string& error) {
    return configure(profileJson, actorTypeValueOffset, playerTypeId, SymbolResolver{}, error);
}

bool ActorFacade::configure(std::string_view profileJson,
                            std::size_t actorTypeValueOffset,
                            std::uint32_t playerTypeId,
                            const SymbolResolver& resolver,
                            std::string& error) {
#ifdef _WIN32
    if (!bedrock::parseNativeItemLayout(profileJson, impl_->itemLayout, error)) return false;
    if (!bedrock::parseNativeActorLayout(profileJson, impl_->actorLayout, error)) return false;
    impl_->localPlayer.load(profileJson);
    impl_->playerList.load(profileJson);
    impl_->registry.configure(impl_->actorLayout, actorTypeValueOffset, playerTypeId);
    impl_->nameTagAddress = 0;
    const auto& nativeResolver = SymbolResolverRuntimeAccess::resolver(resolver);
    if (auto nameTag = bedrock::resolveSharedHook(nativeResolver, "Actor::getNameTag",
                                                  BDS_HOOK_ACTOR_GET_NAME_TAG)) {
        if (bedrock::isUsableSharedHookResolution(*nameTag))
            impl_->nameTagAddress = nameTag->address;
    }
    return true;
#else
    error = "native Actor facade unavailable on this host";
    return false;
#endif
}

void* ActorFacade::localPlayer(void* client) const noexcept {
#ifdef _WIN32
    return impl_->localPlayer.get(client);
#else
    (void)client;
    return nullptr;
#endif
}

std::optional<ActorBoundsValue> ActorFacade::bounds(void* actor) const noexcept {
#ifdef _WIN32
    bedrock::ActorBounds native{};
    if (!impl_->registry.readBounds(actor, native)) return std::nullopt;
    return ActorBoundsValue{{native.minimum.x,native.minimum.y,native.minimum.z},
                            {native.maximum.x,native.maximum.y,native.maximum.z}, true};
#else
    (void)actor;
    return std::nullopt;
#endif
}

std::optional<Vec3Value> ActorFacade::position(void* actor) const noexcept {
#ifdef _WIN32
    bedrock::Vec3 native{};
    if (!impl_->registry.readPosition(actor, native)) return std::nullopt;
    return Vec3Value{native.x,native.y,native.z};
#else
    (void)actor;
    return std::nullopt;
#endif
}

#ifdef _WIN32
static int callActorNameTag(std::uintptr_t address, void* actor,
                            std::string& out) noexcept {
    using Getter = const std::string& (__fastcall*)(void*);
    __try {
        if (!address || !actor) return -2;
        const auto& name = reinterpret_cast<Getter>(address)(actor);
        out = name;
        return name.empty() ? 0 : 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}
#endif

std::uint64_t ActorFacade::runtimeId(void* actor) const noexcept {
#ifdef _WIN32
    return impl_->registry.runtimeIdentity(actor);
#else
    (void)actor;
    return 0;
#endif
}

bool ActorFacade::nameTag(void* actor, std::string& out, std::string& error) const noexcept {
#ifdef _WIN32
    out.clear();
    if (!actor) { error = "Actor unavailable"; return false; }
    if (!impl_->nameTagAddress) { error = "Actor::getNameTag unavailable"; return false; }
    const int result = callActorNameTag(impl_->nameTagAddress, actor, out);
    if (result == -1) { error = "Actor::getNameTag native fault"; return false; }
    if (result == -2) { error = "Actor::getNameTag unavailable"; return false; }
    if (result == 0) { error = "Actor name tag empty"; return false; }
    error.clear();
    return true;
#else
    (void)actor;(void)out;error="native Actor facade unavailable on this host";return false;
#endif
}

std::optional<std::uint32_t> ActorFacade::entityId(void* actor) const noexcept {
#ifdef _WIN32
    std::uint32_t id{};
    return impl_->registry.readEntityId(actor,id) ? std::optional<std::uint32_t>{id} : std::nullopt;
#else
    (void)actor;
    return std::nullopt;
#endif
}


bool ActorFacade::capture(void* local, bool playersOnly,
                          std::size_t& cursor,
                          std::vector<ActorSample>& samples,
                          std::string& error) const noexcept {
#ifdef _WIN32
    bedrock::ActorRegistryCaptureStats stats;
    if (!impl_->registry.registryPopulation(local, stats)) {
        error = stats.failure ? stats.failure : stats.stage;
        return false;
    }
    bedrock::ActorRegistryCapturePolicy policy;
    policy.playersOnly = playersOnly;
    std::vector<bedrock::ActorRegistrySample> native(stats.population);
    std::size_t count{};
    std::size_t visited{};
    const bool ok = impl_->registry.captureBatch(local, policy, stats.population,
                                                 cursor, native.data(), native.size(),
                                                 count, visited, stats);
    samples.clear();
    samples.reserve(count);
    for (std::size_t i=0;i<count;++i) {
        const auto& src=native[i];
        const auto capturedAt=std::chrono::duration_cast<std::chrono::steady_clock::duration>(src.seenAt.time_since_epoch()).count();
        samples.push_back({src.runtimeId,
                           {{src.bounds.minimum.x,src.bounds.minimum.y,src.bounds.minimum.z},
                            {src.bounds.maximum.x,src.bounds.maximum.y,src.bounds.maximum.z}, src.bounds.valid()},
                           src.isPlayer,src.entityId,
                           static_cast<std::uint64_t>(capturedAt),src.actorKey,src.uniqueId});
    }
    if (!ok) error = stats.failure ? stats.failure : stats.stage;
    return ok;
#else
    (void)local;(void)playersOnly;(void)cursor;samples.clear();error="native Actor facade unavailable on this host";return false;
#endif
}

bool ActorFacade::capture(void* local,
                          bool playersOnly,
                          std::size_t& cursor,
                          std::size_t count,
                          std::vector<ActorSample>& samples,
                          std::size_t& visited,
                          ActorCaptureStatsValue& publicStats,
                          std::string& error) const noexcept {
#ifdef _WIN32
    bedrock::ActorRegistryCaptureStats stats;
    if (!impl_->registry.registryPopulation(local, stats)) {
        publicStats = {stats.stage, stats.failure, stats.faultCode, stats.faultAddress,
                       stats.population, stats.nativePlayers, stats.rememberedPlayers,
                       stats.missingRuntime, stats.filteredActors, stats.invalidOwner,
                       stats.missingShape, stats.invalidBounds};
        error = stats.failure ? stats.failure : stats.stage;
        return false;
    }
    bedrock::ActorRegistryCapturePolicy policy;
    policy.playersOnly = playersOnly;
    std::vector<bedrock::ActorRegistrySample> native(count ? count : stats.population);
    std::size_t sampleCount{};
    const bool ok = impl_->registry.captureBatch(local, policy, count,
                                                 cursor, native.data(), native.size(),
                                                 sampleCount, visited, stats);
    samples.clear();
    samples.reserve(sampleCount);
    for (std::size_t i=0;i<sampleCount;++i) {
        const auto& src=native[i];
        const auto capturedAt=std::chrono::duration_cast<std::chrono::steady_clock::duration>(src.seenAt.time_since_epoch()).count();
        samples.push_back({src.runtimeId,
                           {{src.bounds.minimum.x,src.bounds.minimum.y,src.bounds.minimum.z},
                            {src.bounds.maximum.x,src.bounds.maximum.y,src.bounds.maximum.z}, src.bounds.valid()},
                           src.isPlayer,src.entityId,
                           static_cast<std::uint64_t>(capturedAt),src.actorKey,src.uniqueId});
    }
    publicStats = {stats.stage, stats.failure, stats.faultCode, stats.faultAddress,
                   stats.population, stats.nativePlayers, stats.rememberedPlayers,
                   stats.missingRuntime, stats.filteredActors, stats.invalidOwner,
                   stats.missingShape, stats.invalidBounds};
    if (!ok) error = stats.failure ? stats.failure : stats.stage;
    return ok;
#else
    (void)local;(void)playersOnly;(void)cursor;(void)count;(void)samples;(void)visited;(void)publicStats;
    error="native Actor facade unavailable on this host";return false;
#endif
}

bool ActorFacade::registryIdentity(void* actor, std::uintptr_t& identity) const noexcept {
#ifdef _WIN32
    identity = impl_->registry.registryIdentity(actor); return identity != 0;
#else
    (void)actor; identity=0; return false;
#endif
}

bool ActorFacade::refreshTrackedPoses(void* local,
                                      std::vector<ActorSample>& handles,
                                      std::vector<ActorSample>& copies,
                                      std::size_t& copied) const noexcept {
#ifdef _WIN32
    std::vector<bedrock::ActorRegistrySample> nativeHandles(handles.size());
    for(std::size_t i=0;i<handles.size();++i) {
        nativeHandles[i].runtimeId=handles[i].runtimeId;
        nativeHandles[i].bounds={{handles[i].bounds.minimum.x,handles[i].bounds.minimum.y,handles[i].bounds.minimum.z},
                                 {handles[i].bounds.maximum.x,handles[i].bounds.maximum.y,handles[i].bounds.maximum.z}};
        nativeHandles[i].isPlayer=handles[i].isPlayer;
        nativeHandles[i].entityId=handles[i].entityId;
    }
    std::vector<bedrock::ActorRegistrySample> nativeCopies(handles.size());
    std::size_t count=handles.size();
    impl_->registry.copyTrackedPoses(local,nativeHandles.data(),count,nativeCopies.data());
    copied=count;
    copies.resize(copied);
    for(std::size_t i=0;i<copied;++i) {
        const auto& src=nativeCopies[i];
        const auto capturedAt=std::chrono::duration_cast<std::chrono::steady_clock::duration>(src.seenAt.time_since_epoch()).count();
        copies[i]={src.runtimeId,{{src.bounds.minimum.x,src.bounds.minimum.y,src.bounds.minimum.z},
                                  {src.bounds.maximum.x,src.bounds.maximum.y,src.bounds.maximum.z}, src.bounds.valid()},
                                  src.isPlayer,src.entityId,static_cast<std::uint64_t>(capturedAt),src.actorKey,src.uniqueId};
    }
    return copied != 0;
#else
    (void)local;(void)handles;(void)copies;copied=0;return false;
#endif
}

std::vector<PlayerNameValue> ActorFacade::playerNames(void* level) const {
#ifdef _WIN32
    std::vector<PlayerNameValue> result;
    const auto rows = impl_->playerList.snapshot(level);
    if (!rows) return result;
    result.reserve(rows->size());
    for (const auto& row : *rows)
        result.push_back({row.uuid.low,row.uuid.high,row.uniqueId,std::move(row.name)});
    return result;
#else
    (void)level;
    return {};
#endif
}

} // namespace bedrock::sdk
