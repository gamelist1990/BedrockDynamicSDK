#include <bedrock/api/game_api.hpp>
#include <bedrock/generated/symbols.hpp>
#include <bedrock/native/world_events.hpp>
#include <bedrock/enums/packet_id.hpp>
#include <cassert>
#include <stdexcept>

int main() {
    using namespace bedrock;
    // A dimension change is a notification even with an unchanged Level*.
    // Its generation must fence worker jobs and clear the observed roster.
    GameApi api;
    void* const client = reinterpret_cast<void*>(0x1000);
    void* const level = reinterpret_cast<void*>(0x2000);
    unsigned general{}, joins{}, dimensions{}, exits{}, cancelled{};
    api.Event.Enum.WorldChange.then([&](const WorldChangeEvent& e) {
        assert(e.epoch == api.worldEpoch());
        assert(e.clientInstance == client);
        ++general;
    });
    api.Event.Enum.ClientJoinLevel.then([&](const auto&) { ++joins; });
    api.Event.Enum.DimensionChange.then([&](const auto& e) {
        assert(e.level == level);
        ++dimensions;
    });
    api.Event.Enum.ClientExitLevel.then([&](const auto&) { ++exits; });
    api.Event.Enum.ClientCancelJoinLevel.then([&](const auto&) { ++cancelled; });
    api.dispatchWorldChange(WorldChangeReason::Joined, client);
    assert(joins == 1 && api.worldEpoch() == 1);
    api.dispatchLevelTick(level, 1);
    const auto oldJobEpoch = api.worldEpoch();
    api.dispatchWorldChange(WorldChangeReason::DimensionChanged, client);
    assert(dimensions == 1 && api.worldEpoch() != oldJobEpoch);
    api.dispatchLevelTick(level, 2);
    api.dispatchWorldChange(WorldChangeReason::DimensionChanged, client);
    assert(dimensions == 2 && api.worldEpoch() == 3);
    api.dispatchWorldChange(WorldChangeReason::Left, reinterpret_cast<void*>(0x3000));
    api.dispatchWorldChange(WorldChangeReason::Left, nullptr);
    assert(general == 3 && api.worldEpoch() == 3); // another split-screen client
    api.dispatchWorldChange(WorldChangeReason::Left, client);
    api.dispatchWorldChange(WorldChangeReason::JoinCancelled, client);
    assert(exits == 1 && cancelled == 1 && general == 5);
    api.Event.Enum.WorldChange.then([](const auto&) { throw std::runtime_error("listener"); });
    api.dispatchWorldChange(WorldChangeReason::Joined, client);
    assert(joins == 2); // failed general handler does not swallow typed event
    GameApi replacementClient;
    replacementClient.dispatchWorldChange(WorldChangeReason::Joined, client);
    replacementClient.dispatchWorldChange(WorldChangeReason::Left, client);
    replacementClient.dispatchWorldChange(WorldChangeReason::Joined, reinterpret_cast<void*>(0x4000));
    assert(replacementClient.worldEpoch() == 3); // a new ClientInstance after exit
    replacementClient.setWorldEventClient(client);
    replacementClient.dispatchWorldChange(WorldChangeReason::Left, client);
    replacementClient.dispatchWorldChange(WorldChangeReason::Joined, reinterpret_cast<void*>(0x4000));
    assert(replacementClient.worldEpoch() == 4); // explicit split-screen selection persists

    std::string error;
    assert(parseNativeWorldEventsLayout(R"({"native_world_events":{
        "schema":1,"pointer_size":8,"exit_client_offset":136}})", error));
    assert(error.empty());
    assert(!parseNativeWorldEventsLayout(R"({"native_world_events":{
        "schema":1,"pointer_size":8,"exit_client_offset":-8}})", error));
    assert(!parseNativeWorldEventsLayout(R"({"native_world_events":{
        "schema":1,"pointer_size":8,"exit_client_offset":7}})", error));
    assert(!parseNativeWorldEventsLayout("{}", error));
    const auto embedded = parseNativeWorldEventsLayout(generated::kSymbolDatabase, error);
    assert(embedded && embedded->exitClientOffset == 136 && error.empty());

    static_assert(packetIdValue(PacketList::ChangeDimension) == 61);
    static_assert(packetIdValue(PacketId::InventorySlot) == 50);
    static_assert(packetIdValue(PacketId::Transfer) == 85);
    static_assert(PacketId::MobEquipment == PacketId::PlayerEquipment);
    static_assert(packetIdFromHeader(61u | (2u << 10) | (1u << 12)) == PacketId::ChangeDimension);
    static_assert(!packetIdFromValue(61u | (2u << 10)));
    static_assert(!packetIdFromValue(201)); // private framework ID, never vanilla
    static_assert(!packetIdFromValue(0x1003D)); // do not narrow before validating
    static_assert(packetIdFromName("InventorySlot") == PacketId::InventorySlot);
    static_assert(packetName(PacketId::Unknown) == "Unknown");
    assert(!packetIdFromName("NotAPacket"));
}
