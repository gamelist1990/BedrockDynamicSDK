#pragma once

#include <string>
#include <string_view>
#include <unordered_map>

namespace automine::client {

enum class ToolClass { None, Pickaxe, Axe, Shovel, Hoe, Shears };

struct CraftPlan {
    bool possible{};
    std::string summary;
};

struct BlockPos3 {
    int x{}, y{}, z{};
    bool operator<(const BlockPos3 &o) const noexcept {
        if (x != o.x)
            return x < o.x;
        if (y != o.y)
            return y < o.y;
        return z < o.z;
    }
    bool operator==(const BlockPos3 &) const noexcept = default;
};

std::string lower(std::string value);
std::string localPart(std::string id);
bool isAir(const std::string &id);
bool contains(const std::string &text, std::string_view needle);
ToolClass classForBlock(const std::string &rawId);
int requiredTier(const std::string &rawId);
ToolClass classForItem(const std::string &rawId, int &tier);
std::string dropOf(const std::string &rawId);
bool handMineable(const std::string &blockId);
CraftPlan planPickaxe(const std::unordered_map<std::string, int> &items);

} // namespace automine::client
