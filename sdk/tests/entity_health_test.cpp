#include "../src/entity_health_cache.hpp"

#include <bedrock/packets/protocol_codec.hpp>
#include <cassert>
#include <limits>

using bedrock::PacketId;
using bedrock::ProtocolCodec;
using bedrock::WireWriter;
using bedrock::sdk::detail::EntityHealthCache;
using Json = nlohmann::json;

int main() {
    const ProtocolCodec codec;
    EntityHealthCache cache;
    constexpr std::uint64_t local = 10, remote = (1ULL << 63) + 11, mob = 12;
    constexpr std::int64_t playerUnique = -1200, mobUnique = -1300;
    const std::string uuid(16, '\xA7');
    const auto update = [&](std::uint64_t id, float value, std::string_view name = "minecraft:health") {
        WireWriter wire;
        wire.varuint(id);
        wire.varuint(1); // attributes
        for (float field : {0.f, 20.f, value, 0.f, 20.f, 20.f}) wire.f32(field);
        wire.string(name);
        wire.varuint(0); // modifiers
        wire.varuint(0); // tick
        cache.observeAttributes(codec.decode(PacketId::UpdateAttributes, wire.data()));
    };
    const auto remove = [&](std::int64_t unique) {
        WireWriter wire;
        wire.zigzag(unique);
        cache.remove(codec.decode(PacketId::RemoveActor, wire.data()).at("entity_id_self").get<std::int64_t>());
    };

    assert(!cache.health(local) && !cache.health(remote) && !cache.health(mob));
    cache.setLocalHealth(20); // No known target yet.
    assert(!cache.health(0));
    cache.setLocalRuntimeId(local);
    cache.setLocalHealth(20);
    cache.setLocalHealth(0);
    assert(cache.health(local) == 20);

    // AddPlayer identity comes from the matching PlayerList UUID, not from
    // interpreting the packet's runtime ID as a unique ID.
    cache.observePlayerList({{"records", Json::array({{
        {"uuid", Json::binary(std::vector<std::uint8_t>(uuid.begin(), uuid.end()))},
        {"type", "add"}, {"entity_unique_id", playerUnique},
    }})}});
    WireWriter player;
    player.bytes(uuid);
    player.string("remote player");
    player.varuint(remote);
    player.bytes("unparsed trailing fields");
    cache.observePlayer(codec.decodePrefix(PacketId::AddPlayer, player.data(), 3));
    update(remote, 13.5f);
    assert(cache.health(remote) == 13.5f && cache.health(local) == 20);
    update(remote, 7, "minecraft:movement");
    update(remote, std::numeric_limits<float>::infinity());
    cache.observeAttributes({{"runtime_entity_id", remote}, {"attributes", Json::array({
        {{"name", "minecraft:health"}}
    })}});
    assert(cache.health(remote) == 13.5f);

    // AddActor gives the non-player actor's initial current value, even if
    // the spawn packet arrives before a frame exists.
    WireWriter actor;
    actor.zigzag(mobUnique);
    actor.varuint(mob);
    actor.string("minecraft:cow");
    for (int i = 0; i < 10; ++i) actor.f32(0); // position, velocity, angles
    actor.varuint(1);
    actor.string("minecraft:health");
    for (float field : {0.f, 8.f, 10.f}) actor.f32(field);
    actor.bytes("unparsed trailing fields");
    cache.observeActor(codec.decodePrefix(PacketId::AddActor, actor.data(), 10));
    assert(cache.health(mob) == 8);
    update(mob, 0);
    assert(cache.health(mob) == 0);
    update(local, 19);
    assert(cache.health(local) == 19);
    remove(mobUnique);
    remove(playerUnique);
    assert(!cache.health(mob) && !cache.health(remote) && cache.health(local) == 19);

    cache.observeActor(codec.decodePrefix(PacketId::AddActor, actor.data(), 10));
    update(mob, 3);
    cache.observeActor(codec.decodePrefix(PacketId::AddActor, actor.data(), 10));
    assert(cache.health(mob) == 8);
    cache.changeDimension();
    assert(!cache.health(mob) && cache.health(local) == 19);
    cache = {};
    assert(!cache.health(local));
}
