#include <bedrock/sdk/actors.hpp>
#ifdef _WIN32
#include <bedrock/native/actors.hpp>
#include <bedrock/native/items.hpp>
#include <bedrock/native/actor_registry.hpp>
#include <bedrock/native/local_player.hpp>
#include <bedrock/native/player_list.hpp>
#endif
#include <utility>

namespace bedrock::sdk {

struct ActorFacade::Impl {
#ifdef _WIN32
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
#ifdef _WIN32
    if (!bedrock::parseNativeItemLayout(profileJson, impl_->itemLayout, error)) return false;
    if (!bedrock::parseNativeActorLayout(profileJson, impl_->actorLayout, error)) return false;
    impl_->localPlayer.load(profileJson);
    impl_->playerList.load(profileJson);
    impl_->registry.configure(impl_->actorLayout, actorTypeValueOffset, playerTypeId);
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
                            {native.maximum.x,native.maximum.y,native.maximum.z}};
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

std::uint64_t ActorFacade::runtimeId(void* actor) const noexcept {
#ifdef _WIN32
    return impl_->registry.runtimeIdentity(actor);
#else
    (void)actor;
    return 0;
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
        samples.push_back({src.runtimeId,
                           {{src.bounds.minimum.x,src.bounds.minimum.y,src.bounds.minimum.z},
                            {src.bounds.maximum.x,src.bounds.maximum.y,src.bounds.maximum.z}},
                           src.isPlayer,src.entityId});
    }
    if (!ok) error = stats.failure ? stats.failure : stats.stage;
    return ok;
#else
    (void)local;(void)playersOnly;(void)cursor;samples.clear();error="native Actor facade unavailable on this host";return false;
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
