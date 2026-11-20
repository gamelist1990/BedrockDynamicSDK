#include "game_frame.hpp"

namespace bedrock::sdk {
namespace detail {
std::optional<DynamicProperty> getProperty(const WorldSession::Properties& properties,
                                          std::string_view key) {
    const auto it = properties.find(std::string(key));
    return it == properties.end() ? std::nullopt : std::optional{it->second};
}

Result<void> setProperty(WorldSession::Properties& properties, std::string_view key,
                         std::optional<DynamicProperty> value) {
    if (key.empty()) return Result<void>::error(ErrorCode::Invalid, "Dynamic property key is empty");
    if (value) properties.insert_or_assign(std::string(key), std::move(*value));
    else properties.erase(std::string(key));
    return Result<void>::ok();
}
} // namespace detail

std::optional<Player> World::localPlayer() const {
    if (auto* frame = detail::frameFor(this))
        if (const auto id = frame->localPlayerId()) return Player{frame->session, *id};
    return std::nullopt;
}

std::vector<Player> World::getAllPlayers() const {
    std::vector<Player> players;
    if (auto* frame = detail::frameFor(this))
        for (auto id : frame->entityIds())
            if (frame->isPlayer(id)) players.push_back(Player{frame->session, id});
    return players;
}

std::optional<Dimension> World::currentDimension() const {
    if (auto* frame = detail::frameFor(this))
        return Dimension{frame->session, frame->dimensionRevision, frame->dimensionTypeId()};
    return std::nullopt;
}

std::optional<Dimension> World::getDimension(std::string_view typeId) const {
    auto dimension = currentDimension();
    return dimension && dimension->typeId() && *dimension->typeId() == typeId ? dimension : std::nullopt;
}

Result<void> World::sendMessage(std::string_view text) const {
    if (auto player = localPlayer()) return player->sendMessage(text);
    return detail::unavailable("No local player to receive a client message");
}

std::optional<DynamicProperty> World::getDynamicProperty(std::string_view key) const {
    if (auto* frame = detail::frameFor(this)) return detail::getProperty(frame->session->worldProperties, key);
    return std::nullopt;
}

Result<void> World::setDynamicProperty(std::string_view key, std::optional<DynamicProperty> value) const {
    if (auto* frame = detail::frameFor(this))
        return detail::setProperty(frame->session->worldProperties, key, std::move(value));
    return detail::unavailable("No active client world");
}

// The active-frame resolvers deliberately do not go through frameFor(this):
// a shared-tick host may dispatch a command while the frame is bound to a
// different Mod's World. They use the frame's session directly instead.
std::optional<Player> World::activeLocalPlayer() {
    auto* frame = detail::activeFrame;
    if (frame && frame->session)
        if (const auto id = frame->localPlayerId()) return Player{frame->session, *id};
    return std::nullopt;
}

std::vector<Player> World::activePlayers() {
    std::vector<Player> players;
    auto* frame = detail::activeFrame;
    if (frame && frame->session)
        for (auto id : frame->entityIds())
            if (frame->isPlayer(id)) players.push_back(Player{frame->session, id});
    return players;
}

std::optional<Dimension> World::activeDimension() {
    auto* frame = detail::activeFrame;
    if (frame && frame->session)
        return Dimension{frame->session, frame->dimensionRevision, frame->dimensionTypeId()};
    return std::nullopt;
}

std::optional<DynamicProperty> World::activeWorldProperty(std::string_view key) {
    auto* frame = detail::activeFrame;
    if (frame && frame->session) return detail::getProperty(frame->session->worldProperties, key);
    return std::nullopt;
}

Result<void> World::setActiveWorldProperty(std::string_view key,
                                           std::optional<DynamicProperty> value) {
    auto* frame = detail::activeFrame;
    if (frame && frame->session)
        return detail::setProperty(frame->session->worldProperties, key, std::move(value));
    return detail::unavailable("No active client world");
}

std::optional<Block> Dimension::getBlock(BlockPosition position) const {
    auto* frame = detail::frameFor(session_);
    if (!frame || frame->dimensionRevision != revision_) return std::nullopt;
    if (auto id = frame->blockTypeId(position)) return Block{session_, revision_, position, std::move(*id)};
    return std::nullopt;
}

std::vector<Entity> Dimension::getEntities() const {
    std::vector<Entity> entities;
    auto* frame = detail::frameFor(session_);
    if (!frame || frame->dimensionRevision != revision_) return entities;
    for (auto id : frame->entityIds()) entities.push_back(Entity{session_, id});
    return entities;
}

Result<Entity> Dimension::spawnEntity(std::string_view typeId, Vector3 position) const {
    auto* frame = detail::frameFor(session_);
    if (!frame || frame->dimensionRevision != revision_)
        return detail::unavailable<Entity>("Dimension is not active");
    return frame->spawnEntity(typeId, position);
}

Result<void> Block::setType(std::string_view typeId) const {
    auto* frame = detail::frameFor(session_);
    if (!frame || frame->dimensionRevision != dimension_) return detail::unavailable("Block is not in the active dimension");
    return frame->setBlockType(position_, typeId);
}
} // namespace bedrock::sdk
