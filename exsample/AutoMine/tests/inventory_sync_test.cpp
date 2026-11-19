#include <bedrock/detail/inventory_snapshot.hpp>
#include <bedrock/packets/protocol_codec.hpp>
#include <cassert>
#include <string>

struct Slot {
    int slot{};
    std::string item;
    int count{};
    int netId{};
};
struct Shadow {
    Slot item;
    int memNetId{}, memCount{};
};
int main() {
    using bedrock::detail::reconcileInventory;
    std::map<int, Shadow> shadow{{8, {{8, "stone_pickaxe", 1, 42}, 42, 1}}};
    auto result = reconcileInventory(std::vector<Slot>{{8, {}, 0, 0}}, shadow, true);
    assert(shadow.empty());
    assert(result.size() == 1 && result[0].slot == 8 && result[0].count == 0 &&
           result[0].item.empty());
    assert(reconcileInventory(std::vector<Slot>{{8, {}, 0, 0}}, shadow, false).empty());

    // An unreadable slot is not evidence that a tool broke.
    shadow[8] = {{8, "stone_pickaxe", 1, 42}, 0, 0};
    result = reconcileInventory(std::vector<Slot>{}, shadow, true);
    assert(result.size() == 1 && result[0].item == "stone_pickaxe");
    assert(shadow.size() == 1);
    // Newly crafted results remain visible while the native inventory catches up.
    result = reconcileInventory(std::vector<Slot>{{8, {}, 0, 0}}, shadow, true);
    assert(result.size() == 1 && result[0].item == "stone_pickaxe" && result[0].count == 1);
    // A subsequent pickup or replacement retires the response overlay.
    result = reconcileInventory(std::vector<Slot>{{8, "dirt", 4, 99}}, shadow, true);
    assert(shadow.empty() && result[0].item == "dirt" && result[0].count == 4);
    // Workstation results can be deposited in a consumed-log slot whose
    // native item name is still stale, or in an as-yet unreadable main slot.
    shadow[5] = {{5, "crafting_table", 1, 101}, 12, 1};
    shadow[20] = {{20, "furnace", 1, 102}, 0, 0};
    result = reconcileInventory(std::vector<Slot>{{5, "birch_log", 1, 12}}, shadow, true);
    assert(result.size() == 2 && result[0].item == "crafting_table" &&
           result[1].slot == 20 && result[1].item == "furnace");
    result = reconcileInventory(
        std::vector<Slot>{{5, "crafting_table", 1, 101}, {20, "furnace", 1, 102}},
        shadow, true);
    assert(shadow.empty() && result[0].item == "crafting_table" &&
           result[1].item == "furnace");

    const bedrock::ProtocolCodec codec;
    bedrock::WireWriter slot;
    slot.varuint(0);
    slot.varuint(8);
    const auto prefix = codec.decodePrefix(bedrock::PacketId::InventorySlot, slot.data(), 2);
    assert(prefix.at("window_id") == "inventory" && prefix.at("slot") == 8);
    bedrock::WireWriter content;
    content.varuint(122);
    assert(codec.decodePrefix(bedrock::PacketId::InventoryContent, content.data(), 1)
               .at("window_id") == "hotbar");
    bedrock::WireWriter furnace;
    furnace.varuint(1);
    furnace.varuint(8);
    assert(
        codec.decodePrefix(bedrock::PacketId::InventorySlot, furnace.data(), 2).at("window_id") ==
        "first");
    bool rejected = false;
    try {
        (void)codec.decodePrefix(bedrock::PacketId::InventorySlot, {}, 2);
    } catch (...) {
        rejected = true;
    }
    assert(rejected);
}
