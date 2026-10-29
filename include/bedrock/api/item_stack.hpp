#pragma once
#include <string>
namespace bedrock {
// Stable value snapshot. The native ItemStack pointer never escapes the adapter.
// displayName/customName are optional: many servers replace item IDs without
// supplying a localized name, so callers should always retain identifier.
struct ItemStack {
    std::string identifier;
    std::string displayName;
    unsigned count{};
    std::string customName;
    std::string translationKey;

    [[nodiscard]] bool valid() const noexcept {
        return !identifier.empty() || !displayName.empty() || !customName.empty() ||
               !translationKey.empty();
    }
    [[nodiscard]] std::string getName() const {
        if (!customName.empty())
            return customName;
        if (!displayName.empty())
            return displayName;
        return identifier;
    }
};
using HeldItemInfo = ItemStack; // source-compatible name for older examples

} // namespace bedrock
