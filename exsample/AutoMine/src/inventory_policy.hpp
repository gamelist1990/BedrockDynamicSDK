#pragma once
#include <string>
#include <string_view>
#include <set>
namespace automine {
inline std::string itemName(std::string value) { const auto c=value.find(':');return c==std::string::npos?value:value.substr(c+1); }
inline int toolSlot(const std::string& raw) {
    const auto id=itemName(raw);
    if (id.ends_with("_sword")) return 0;
    if (id.ends_with("_pickaxe")) return 2;
    if (id.ends_with("_axe")) return 1;
    if (id.ends_with("_shovel")) return 3;
    return -1;
}
inline int toolRank(const std::string& raw) {
    const auto id=itemName(raw);
    for (const auto& [prefix,rank]:{std::pair{"netherite_",6}, {"diamond_",5}, {"iron_",4},{"stone_",3},{"golden_",2},{"wooden_",1}})
        if (id.starts_with(prefix)) return rank;
    return 0;
}
inline bool consumeToolFirst(int rank, int remaining, int slot,
                             int bestRank, int bestRemaining, int bestSlot, int selected) {
    if (bestSlot < 0 || rank != bestRank) return bestSlot < 0 || rank < bestRank;
    if (remaining != bestRemaining) return remaining < bestRemaining;
    if ((slot == selected) != (bestSlot == selected)) return slot == selected;
    return slot < bestSlot;
}
inline bool rawBuildingWood(const std::string &raw) {
    const auto id = itemName(raw);
    return id.ends_with("_log") || id.ends_with("_wood") ||
           id.ends_with("_stem") || id.ends_with("_hyphae");
}
inline int buildingPriority(const std::string &raw) {
    const auto id = itemName(raw);
    if (id == "dirt" || id == "andesite" || id == "granite" || id == "diorite" ||
        id == "netherrack" || id == "tuff" || id == "coarse_dirt" ||
        id == "mossy_cobblestone" || id == "grass_block" || id == "podzol" ||
        id == "rooted_dirt") return 0;
    if (id == "cobblestone" || id == "stone" || id == "cobbled_deepslate") return 1;
    if (id.ends_with("_planks")) return 2;
    return -1; // raw wood, valuable materials and non-blocks are never placed
}
inline bool discardable(const std::string& raw,int slot,const std::set<std::string>& allow,const std::set<std::string>& protectedItems) {
    const auto id=itemName(raw);
    if (slot<9 || slot>=36 || !allow.count(id) || protectedItems.count(id) || toolSlot(id)>=0) return false;
    for (const auto* keep:{"_log","_wood","_planks","_stem","_hyphae","_ore","_ingot","_sword","_axe","_pickaxe","_shovel","_hoe"})
        if (id.ends_with(keep)) return false;
    for (const auto* keep:{"diamond","emerald","netherite_scrap","ancient_debris","raw_iron","raw_gold","raw_copper","coal","charcoal","stick","crafting_table","furnace","cobblestone","cobbled_deepslate","dirt","torch","apple","bread","cooked_beef","cooked_porkchop","cooked_chicken","cooked_mutton","baked_potato"})
        if (id==keep) return false;
    return true;
}
}
