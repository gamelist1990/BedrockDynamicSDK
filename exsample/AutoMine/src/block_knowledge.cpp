#include "block_knowledge.hpp"

#include <algorithm>
#include <cctype>
#include <utility>

namespace automine::client {

std::string lower(std::string value) {
    for (auto &c : value)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return value;
}
// "minecraft:iron_ore" -> "iron_ore"; leaves other namespaces untouched textually.
std::string localPart(std::string id) {
    id = lower(std::move(id));
    const auto colon = id.find(':');
    return colon == std::string::npos ? id : id.substr(colon + 1);
}
bool isAir(const std::string &id) {
    const auto local = localPart(id);
    return local.empty() || local == "air" || local == "cave_air" || local == "void_air";
}
bool contains(const std::string &text, std::string_view needle) {
    return text.find(needle) != std::string::npos;
}

ToolClass classForBlock(const std::string &rawId) {
    const auto id = localPart(rawId);
    if (id.empty() || isAir(id))
        return ToolClass::None;
    // Pickaxe first: "sandstone" must not fall into the shovel "sand" rule.
    if (contains(id, "_ore") || id == "obsidian" || id == "ancient_debris" ||
        contains(id, "stone") || contains(id, "deepslate") || contains(id, "netherrack") ||
        contains(id, "basalt") || contains(id, "blackstone") || contains(id, "brick") ||
        contains(id, "quartz") || contains(id, "prismarine") || contains(id, "terracotta") ||
        contains(id, "concrete") || id == "ice" || id == "packed_ice" || id == "blue_ice" ||
        contains(id, "amethyst") || contains(id, "copper") || contains(id, "iron") ||
        contains(id, "gold_block") || contains(id, "diamond_block") || contains(id, "emerald") ||
        contains(id, "lapis") || contains(id, "redstone_block") || contains(id, "furnace") ||
        contains(id, "anvil") || contains(id, "enchanting_table") ||
        contains(id, "brewing_stand") || contains(id, "hopper") || contains(id, "rails") ||
        id == "rail" || contains(id, "_rail") || contains(id, "lantern") || contains(id, "chain") ||
        id == "glass" || contains(id, "_glass") || contains(id, "end_stone") ||
        contains(id, "purpur") || contains(id, "magma") || contains(id, "bone_block") ||
        contains(id, "dripstone") || contains(id, "calcite") || contains(id, "tuff") ||
        contains(id, "mud_bricks"))
        return ToolClass::Pickaxe;
    if (id == "sand" || id == "red_sand" || id == "gravel" || contains(id, "dirt") ||
        id == "grass_block" || id == "podzol" || id == "mycelium" || contains(id, "clay") ||
        contains(id, "snow") || id == "soul_sand" || id == "soul_soil" || id == "mud" ||
        id == "farmland" || contains(id, "concrete_powder"))
        return ToolClass::Shovel;
    if (contains(id, "_log") || contains(id, "_wood") || contains(id, "planks") ||
        contains(id, "_stem") || contains(id, "_hyphae") || contains(id, "chest") ||
        id == "crafting_table" || id == "barrel" || id == "bookshelf" || id == "lectern" ||
        id == "note_block" || id == "jukebox" || id == "composter" || id == "loom" ||
        id == "fletching_table" || id == "cartography_table" || id == "smithing_table" ||
        contains(id, "pumpkin") || id == "melon" || contains(id, "bamboo") ||
        contains(id, "_sign") || contains(id, "_door") || contains(id, "_fence") ||
        contains(id, "ladder") || contains(id, "scaffolding"))
        return ToolClass::Axe;
    if (contains(id, "leaves") || id == "hay_block" || contains(id, "sculk") || id == "target" ||
        contains(id, "moss"))
        return ToolClass::Hoe;
    if (id == "wool" || contains(id, "_wool") || id == "cobweb" || id == "web")
        return ToolClass::Shears;
    return ToolClass::None;
}

// Minimum tool tier (material rank) needed for the block to drop anything.
int requiredTier(const std::string &rawId) {
    const auto id = localPart(rawId);
    if (id == "obsidian" || id == "ancient_debris" || id == "netherite_block" ||
        id == "crying_obsidian" || id == "respawn_anchor")
        return 5; // diamond+
    if (contains(id, "diamond_ore") || contains(id, "emerald_ore") || contains(id, "gold_ore") ||
        contains(id, "redstone_ore") || id == "gold_block" || id == "diamond_block" ||
        id == "emerald_block" || id == "redstone_block")
        return 4; // iron+
    if (contains(id, "iron_ore") || contains(id, "lapis_ore") || id == "iron_block" ||
        id == "lapis_block" || contains(id, "copper") || contains(id, "_rail") || id == "rail")
        return 2; // stone+
    if (classForBlock(id) == ToolClass::Pickaxe)
        return 1; // wooden pickaxe still required
    return 0;
}

ToolClass classForItem(const std::string &rawId, int &tier) {
    const auto id = localPart(rawId);
    tier = 0;
    if (id == "shears")
        return ToolClass::Shears;
    ToolClass kind = ToolClass::None;
    if (contains(id, "_pickaxe"))
        kind = ToolClass::Pickaxe;
    else if (contains(id, "_shovel"))
        kind = ToolClass::Shovel;
    else if (contains(id, "_hoe"))
        kind = ToolClass::Hoe;
    else if (contains(id, "_axe"))
        kind = ToolClass::Axe;
    if (kind == ToolClass::None)
        return ToolClass::None;
    if (contains(id, "netherite_"))
        tier = 6;
    else if (contains(id, "diamond_"))
        tier = 5;
    else if (contains(id, "iron_"))
        tier = 4;
    else if (contains(id, "golden_"))
        tier = 1; // gold harvests at wooden tier
    else if (contains(id, "stone_"))
        tier = 2;
    else if (contains(id, "wooden_"))
        tier = 1;
    return kind;
}
// What a block drops when mined with a sufficient tool (for goal counts).
std::string dropOf(const std::string &rawId) {
    const auto id = localPart(rawId);
    if (contains(id, "diamond_ore"))
        return "diamond";
    if (contains(id, "emerald_ore"))
        return "emerald";
    if (contains(id, "iron_ore"))
        return "raw_iron";
    if (contains(id, "gold_ore"))
        return "raw_gold";
    if (contains(id, "copper_ore"))
        return "raw_copper";
    if (contains(id, "coal_ore"))
        return "coal";
    if (contains(id, "redstone_ore"))
        return "redstone";
    if (contains(id, "lapis_ore"))
        return "lapis_lazuli";
    if (contains(id, "quartz_ore"))
        return "quartz";
    if (id == "grass_block" || id == "dirt_path" || id == "farmland")
        return "dirt";
    if (id == "stone")
        return "cobblestone";
    if (id == "deepslate")
        return "cobbled_deepslate";
    if (contains(id, "leaves") || id == "glass" || id == "ice")
        return {};
    return id;
}

bool handMineable(const std::string &blockId) {
    // Blocks that drop (or are acceptably broken) with a bare hand / any item.
    return classForBlock(blockId) != ToolClass::Pickaxe && requiredTier(blockId) == 0;
}

CraftPlan planPickaxe(const std::unordered_map<std::string, int> &items) {
    auto have = [&](std::string_view key) {
        int total = 0;
        for (const auto &[id, count] : items)
            if (contains(id, key))
                total += count;
        return total;
    };
    const int logs = have("_log") + have("_wood");
    const int planks = have("planks");
    const int sticks = have("stick");
    const int tables = have("crafting_table");
    const int cobble = have("cobblestone");
    // Wooden pickaxe: 3 planks + 2 sticks on a table. Sticks: 2 planks.
    // Table: 4 planks. Total planks for the full chain without a table: 9.
    const int plankTotal = planks + logs * 4;
    CraftPlan plan;
    if (cobble >= 3 && (sticks >= 2 || plankTotal >= 2) && (tables >= 1 || plankTotal >= 4)) {
        plan.possible = true;
        plan.summary = "丸石3＋棒2＋作業台で「石のツルハシ」が作れるよ";
        return plan;
    }
    if (plankTotal >= (tables >= 1 ? 5 : 9)) {
        plan.possible = true;
        plan.summary = "原木" + std::to_string(logs) + "・板材" + std::to_string(planks) +
                       "から 板材→（作業台）→棒→「木のツルハシ」が作れるよ";
        return plan;
    }
    plan.summary = "ツルハシの材料が足りない（原木あと" +
                   std::to_string(std::max(0, 3 - logs - (planks >= 9 ? 3 : 0))) +
                   "個くらい必要）。原木を集めてホットバーに置いてね";
    return plan;
}

} // namespace automine::client
