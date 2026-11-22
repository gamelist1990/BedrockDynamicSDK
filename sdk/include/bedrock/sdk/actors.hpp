#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include "world_access.hpp"
#include "resolver.hpp"
#include <string>
#include <string_view>
#include <vector>

namespace bedrock::sdk {

struct ActorBoundsValue { Vec3Value minimum; Vec3Value maximum; bool valid{}; };
struct ActorSample {
    std::uint64_t runtimeId{};
    ActorBoundsValue bounds;
    bool isPlayer{};
    std::uint32_t entityId{};
    std::uint64_t capturedAt{};
    std::uintptr_t actorKey{};
    std::uint64_t uniqueId{};
};
struct PlayerNameValue {
    std::uint64_t uuidLow{};
    std::uint64_t uuidHigh{};
    std::uint64_t uniqueId{};
    std::string name;
};

class ActorFacade {
public:
    ActorFacade();
    ~ActorFacade();

    bool configure(std::string_view profileJson,
                   std::size_t actorTypeValueOffset,
                   std::uint32_t playerTypeId,
                   std::string& error);

    bool configure(std::string_view profileJson,
                   std::size_t actorTypeValueOffset,
                   std::uint32_t playerTypeId,
                   const SymbolResolver& resolver,
                   std::string& error);

    void* localPlayer(void* client) const noexcept;
    std::optional<ActorBoundsValue> bounds(void* actor) const noexcept;
    std::optional<Vec3Value> position(void* actor) const noexcept;
    std::uint64_t runtimeId(void* actor) const noexcept;
    std::optional<std::uint32_t> entityId(void* actor) const noexcept;
    // The actor's type value (the ActorType component read the native
    // registry performs), or nullopt when unreadable.
    std::optional<std::uint32_t> actorType(void* actor) const noexcept;

    bool capture(void* local,
                 bool playersOnly,
                 std::size_t& cursor,
                 std::vector<ActorSample>& samples,
                 std::string& error) const noexcept;

    struct ActorCaptureStatsValue {
        const char* stage{};
        const char* failure{};
        std::uint32_t faultCode{};
        std::uintptr_t faultAddress{};
        std::size_t population{}, nativePlayers{}, rememberedPlayers{}, missingRuntime{},
                    filteredActors{}, invalidOwner{}, missingShape{}, invalidBounds{};
    };
    bool capture(void* local,
                 bool playersOnly,
                 std::size_t& cursor,
                 std::size_t count,
                 std::vector<ActorSample>& samples,
                 std::size_t& visited,
                 ActorCaptureStatsValue& stats,
                 std::string& error) const noexcept;

    bool registryIdentity(void* actor, std::uintptr_t& identity) const noexcept;
    bool nameTag(void* actor, std::string& out, std::string& error) const noexcept;
    bool refreshTrackedPoses(void* local,
                             std::vector<ActorSample>& handles,
                             std::vector<ActorSample>& copies,
                             std::size_t& copied) const noexcept;

    std::vector<PlayerNameValue> playerNames(void* level) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace bedrock::sdk
