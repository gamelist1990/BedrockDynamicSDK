#include <bedrock/sdk.hpp>
#include "../src/game_frame.hpp"
#include "../src/item_packet_decode.hpp"
#include <bedrock/enums/actors.hpp>

#include <cassert>
#include <limits>
#include <thread>
#include <unordered_map>

namespace sdk = bedrock::sdk;

namespace {
class TestFrame final : public sdk::detail::GameFrame {
public:
    std::optional<std::uint64_t> local{42};
    std::vector<std::uint64_t> ids{42, 43};
    std::string message;
    int clears{};
    std::unordered_map<std::uint64_t, std::uint32_t> actorTypes;
    std::unordered_map<std::uint64_t, sdk::ItemStack> itemStacks;
    std::optional<std::uint64_t> localPlayerId() const override { return local; }
    const std::vector<std::uint64_t>& entityIds() override { return ids; }
    bool isPlayer(std::uint64_t id) override { return id == local || id == 43; }
    bool contains(std::uint64_t id) override { return isPlayer(id) || itemStacks.contains(id); }
    std::optional<std::string> typeId(std::uint64_t id) override {
        const auto type = actorTypes.find(id);
        if (type != actorTypes.end() &&
            type->second == static_cast<std::uint32_t>(bedrock::ActorType::ItemEntity))
            return "minecraft:item";
        return isPlayer(id) ? "minecraft:player" : "minecraft:zombie";
    }
    std::optional<std::string> name(std::uint64_t) override { return "Alex"; }
    std::optional<sdk::Vector3> location(std::uint64_t) override { return sdk::Vector3{1, 64, -2}; }
    std::optional<sdk::Rotation> rotation(std::uint64_t) override { return sdk::Rotation{10, 90}; }
    std::optional<std::string> dimensionTypeId() const override { return "minecraft:overworld"; }
    std::optional<std::string> blockTypeId(sdk::BlockPosition) override { return "minecraft:glass"; }
    std::optional<sdk::HealthComponent> health(std::uint64_t) const override { return sdk::HealthComponent{20}; }
    std::optional<sdk::InventoryComponent> inventory(std::uint64_t) override {
        return sdk::InventoryComponent{{sdk::ItemStack{"minecraft:diamond", 2}}};
    }
    std::optional<sdk::EquippableComponent> equippable(std::uint64_t) override {
        return equipment({sdk::ItemStack{"minecraft:diamond", 2}});
    }
    std::optional<sdk::ItemStack> itemStack(std::uint64_t id) const override {
        const auto it = itemStacks.find(id);
        return it != itemStacks.end() ? std::optional{it->second} : std::nullopt;
    }
    sdk::Result<void> sendMessage(std::uint64_t id, std::string_view text) override {
        assert(id == local);
        message = text;
        return sdk::Result<void>::ok();
    }
    sdk::Result<void> clearEffects(std::uint64_t id) override {
        assert(id == local);
        ++clears;
        return sdk::Result<void>::ok();
    }
};

class TestMod final : public sdk::Mod {
public:
    TestMod() : Mod("world-test") {}
    int spawns{}, updates{}, disables{}, scheduled{};
    bool throwOnSpawn{};
private:
    void onEnable() override { system().run([this] { ++scheduled; }); }
    void onDisable() override { ++disables; }
    void onPlayerSpawn(sdk::Player& player) override {
        assert(player.isValid());
        ++spawns;
        if (throwOnSpawn) throw std::runtime_error("spawn failed");
        assert(player.sendMessage("hello"));
    }
    void onUpdate() override { ++updates; }
};

// Minimal wire writers kept local so the host test does not need the legacy
// <bedrock/...> include root (which would shadow the new SDK headers).
void appendVarint(std::string& out, std::uint64_t value) {
    do {
        const auto byte = static_cast<unsigned char>(value & 0x7f);
        value >>= 7;
        out.push_back(static_cast<char>(byte | (value ? 0x80 : 0)));
    } while (value);
}
void appendZigzag(std::string& out, std::int64_t value) {
    const auto bits = static_cast<std::uint64_t>(value);
    appendVarint(out, (bits << 1) ^ static_cast<std::uint64_t>(-(value < 0)));
}
void appendString(std::string& out, std::string_view text) {
    appendVarint(out, text.size());
    out.append(text);
}
void appendLi16(std::string& out, std::uint16_t value) {
    out.push_back(static_cast<char>(value & 0xff));
    out.push_back(static_cast<char>(value >> 8));
}

// item-entity-detection: the two native packets are decoded with the same
// bounded decoders the native backend uses, then the SDK's public enumeration
// path must surface the dropped stack (type ID and amount).
void testItemEntityDetection() {
    std::string registryPayload;
    appendVarint(registryPayload, 2); // itemstates count
    appendString(registryPayload, "minecraft:leather");
    appendLi16(registryPayload, 42);  // runtime_id
    registryPayload.push_back(0);     // component_based = false
    appendZigzag(registryPayload, 0); // version: legacy
    registryPayload.append({10, 0, 0}); // network NBT: empty compound + TAG_End
    appendString(registryPayload, "minecraft:beef");
    appendLi16(registryPayload, 43);
    registryPayload.push_back(0);
    appendZigzag(registryPayload, 0);
    registryPayload.append({10, 0, 0});

    std::unordered_map<std::uint16_t, std::string> runtimeIds;
    assert(sdk::detail::decodeItemRegistry(registryPayload, runtimeIds));
    assert(runtimeIds.size() == 2);
    assert(runtimeIds.at(42) == "minecraft:leather");

    std::string addPayload;
    appendZigzag(addPayload, 123);  // entity_id_self
    appendVarint(addPayload, 777);  // runtime_entity_id
    appendLi16(addPayload, 42);     // ItemV4.network_id
    appendLi16(addPayload, 3);      // ItemV4.count
    const auto decoded = sdk::detail::decodeAddItemActorPrefix(addPayload);
    assert(decoded && decoded->runtimeId == 777 && decoded->networkId == 42 && decoded->count == 3);

    // Mirror the native backend: packet payload -> runtime-ID map -> frame state.
    TestFrame frame;
    frame.ids.push_back(777);
    frame.actorTypes[777] = static_cast<std::uint32_t>(bedrock::ActorType::ItemEntity);
    frame.itemStacks.insert_or_assign(777, sdk::ItemStack{
        runtimeIds.at(static_cast<std::uint16_t>(decoded->networkId)), decoded->count});
    TestMod mod;
    assert(mod.enable());
    frame.world = &mod.world();
    frame.session = std::make_shared<sdk::detail::WorldSession>();
    {
        sdk::detail::GameFrameScope scope{frame};
        const auto dimension = mod.world().currentDimension();
        assert(dimension);
        bool found = false;
        for (const auto& entity : dimension->getEntities()) {
            const auto typeId = entity.typeId();
            if (!typeId || *typeId != "minecraft:item") continue;
            const auto stack = entity.getItemStack();
            assert(stack);
            assert(stack->typeId() == "minecraft:leather");
            assert(stack->amount() == 3);
            found = true;
        }
        assert(found);
    }
    assert(mod.disable());
}
// Stage B live fix: a shared-tick host may run a JS handler while the active
// frame is bound to a different Mod's World. The active-session resolvers must
// still see the local player, while the instance accessor stays World-bound.
void testActiveSessionResolvers() {
    TestMod mod, otherMod;
    assert(mod.enable());
    assert(!sdk::World::activeLocalPlayer()); // no active frame yet
    TestFrame frame;
    frame.world = &otherMod.world(); // deliberately NOT mod.world()
    frame.session = std::make_shared<sdk::detail::WorldSession>();
    {
        sdk::detail::GameFrameScope scope{frame};
        assert(!mod.world().localPlayer()); // instance accessor is still World-bound
        const auto active = sdk::World::activeLocalPlayer();
        assert(active && active->name() == "Alex");
        assert(sdk::World::activePlayers().size() == 2);
        assert(sdk::World::activeDimension());
        assert(sdk::World::setActiveWorldProperty("k", sdk::DynamicProperty{7.0}));
        const auto prop = sdk::World::activeWorldProperty("k");
        assert(prop && std::get<double>(*prop) == 7.0);
    }
    assert(!sdk::World::activeLocalPlayer()); // frame scope ended
}
} // namespace

int main() {
    testItemEntityDetection();
    testActiveSessionResolvers();
    TestMod mod, otherMod;
    assert(mod.enable());
    assert(!mod.world().localPlayer());
    assert(!mod.world().setDynamicProperty("count", sdk::DynamicProperty{1.0}));
    TestFrame frame;
    frame.world = &mod.world();
    frame.session = std::make_shared<sdk::detail::WorldSession>();
    std::optional<sdk::Player> original;
    std::optional<sdk::Dimension> oldDimension;
    {
        sdk::detail::GameFrameScope scope{frame};
        assert(mod.update());
        assert(mod.spawns == 1 && mod.scheduled == 1 && frame.message == "hello");
        assert(!otherMod.world().localPlayer());
        original = mod.world().localPlayer();
        assert(original && original->name() == "Alex" && original->typeId() == "minecraft:player");
        assert(original->location()->z == -2);
        const auto players = mod.world().getPlayers();
        assert(players.size() == 2);
        assert(original->getComponent<sdk::HealthComponent>()->currentValue() == 20);
        assert(!original->getComponent<sdk::FoodComponent>());
        assert(original->getComponent<sdk::InventoryComponent>()->getItem(0)->amount() == 2);
        assert(original->getComponent<sdk::EquippableComponent>()->getEquipment(sdk::EquipmentSlot::MainHand));
        assert(original->setDynamicProperty("count", sdk::DynamicProperty{2.0}));
        assert(mod.world().setDynamicProperty("count", sdk::DynamicProperty{3.0}));
        oldDimension = mod.world().getDimension("minecraft:overworld");
        assert(oldDimension && oldDimension->getBlock({1, 63, -2})->typeId() == "minecraft:glass");
        assert(oldDimension->getEntities().size() == 2);
        assert(!original->addEffect("minecraft:speed", -1));
        const auto effect = original->addEffect("minecraft:speed", std::numeric_limits<double>::infinity());
        assert(!effect && effect.error().code == sdk::ErrorCode::Unavailable);
        const auto missingId = original->removeEffect("");
        assert(!missingId && missingId.error().code == sdk::ErrorCode::Invalid);
        assert(!players.back().clearEffects() && frame.clears == 0);
        assert(original->clearEffects() && frame.clears == 1);
        std::thread worker([&] { assert(!original->isValid()); });
        worker.join();
    }
    assert(!original->isValid());
    assert(!original->clearEffects() && frame.clears == 1);
    {
        sdk::detail::GameFrameScope scope{frame};
        assert(mod.update() && mod.spawns == 1);
        assert(std::get<double>(*original->getDynamicProperty("count")) == 2);
        assert(std::get<double>(*mod.world().getDynamicProperty("count")) == 3);
        assert(original->setDynamicProperty("count", std::nullopt));
        ++frame.dimensionRevision;
        assert(!oldDimension->getBlock({1, 63, -2}));
        assert(!original->getDynamicProperty("count"));
        frame.local = 44; // Respawn, same world.
        frame.ids[0] = 44;
        mod.throwOnSpawn = true;
        assert(!mod.update() && mod.spawns == 2 && mod.updates == 3);
        assert(!original->isValid());
        mod.throwOnSpawn = false;
        frame.session = std::make_shared<sdk::detail::WorldSession>();
        assert(!mod.world().getDynamicProperty("count"));
        assert(mod.update() && mod.spawns == 3);
    }
    assert(mod.disable() && mod.disables == 1);
    assert(!mod.update());
}
