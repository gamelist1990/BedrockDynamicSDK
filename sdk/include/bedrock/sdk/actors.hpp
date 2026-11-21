#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include "world_access.hpp"
#include <string>
#include <string_view>
#include <vector>

namespace bedrock::sdk {

struct ActorBoundsValue { Vec3Value minimum; Vec3Value maximum; };
struct ActorSample {
    std::uint64_t runtimeId{};
    ActorBoundsValue bounds;
    bool isPlayer{};
    std::uint32_t entityId{};
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

    void* localPlayer(void* client) const noexcept;
    std::optional<ActorBoundsValue> bounds(void* actor) const noexcept;
    std::optional<Vec3Value> position(void* actor) const noexcept;
    std::uint64_t runtimeId(void* actor) const noexcept;
    std::optional<std::uint32_t> entityId(void* actor) const noexcept;

    bool capture(void* local,
                 bool playersOnly,
                 std::size_t& cursor,
                 std::vector<ActorSample>& samples,
                 std::string& error) const noexcept;

    std::vector<PlayerNameValue> playerNames(void* level) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace bedrock::sdk
