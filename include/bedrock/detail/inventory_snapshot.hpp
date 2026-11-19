#pragma once

#include <map>
#include <utility>
#include <vector>

namespace bedrock::detail {

// Acknowledged results remain visible even if a native slot cannot yet be read.
// Callers requiring native confirmation must use memorySnapshot instead.
template <class Slot, class Shadow>
std::vector<Slot> reconcileInventory(std::vector<Slot> memory, std::map<int, Shadow> &shadow,
                                     bool includeEmpty) {
    std::map<int, Slot> merged;
    for (auto &slot : memory)
        merged[slot.slot] = std::move(slot);
    for (auto it = shadow.begin(); it != shadow.end();) {
        const auto found = merged.find(it->first);
        if (found == merged.end()) {
            merged.emplace(it->first, it->second.item);
            ++it;
            continue;
        }
        const auto &raw = found->second;
        if (raw.netId != it->second.memNetId || raw.count != it->second.memCount) {
            it = shadow.erase(it);
            continue;
        }
        found->second = it->second.item;
        ++it;
    }
    std::vector<Slot> result;
    for (auto &[index, slot] : merged)
        if (includeEmpty || slot.count > 0)
            result.push_back(std::move(slot));
    return result;
}

} // namespace bedrock::detail
