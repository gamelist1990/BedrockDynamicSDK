#pragma once
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

namespace bedrock::detail {
// Stable vanilla Bedrock effect ordering. The numeric IDs are the wire/component
// identity; the canonical typeId is `minecraft:<name>`. Keeping both conversions
// in one place makes SDK readback and native writes agree on the same identity.
inline constexpr std::string_view effectNames[] = {
    {},                       // 0 (none)
    "minecraft:speed",
    "minecraft:slowness",
    "minecraft:haste",
    "minecraft:mining_fatigue",
    "minecraft:strength",
    "minecraft:instant_health",
    "minecraft:instant_damage",
    "minecraft:jump_boost",
    "minecraft:nausea",
    "minecraft:regeneration",
    "minecraft:resistance",
    "minecraft:fire_resistance",
    "minecraft:water_breathing",
    "minecraft:invisibility",
    "minecraft:blindness",
    "minecraft:night_vision",
    "minecraft:hunger",
    "minecraft:weakness",
    "minecraft:poison",
    "minecraft:wither",
    "minecraft:health_boost",
    "minecraft:absorption",
    "minecraft:saturation",
    "minecraft:levitation",
    "minecraft:fatal_poison",
    "minecraft:conduit_power",
    "minecraft:slow_falling",
    "minecraft:bad_omen",
    "minecraft:village_hero",
    "minecraft:darkness",
    "minecraft:infested",
    "minecraft:oozing",
    "minecraft:weaving",
    "minecraft:wind_charged",
    "minecraft:raid_omen",
    "minecraft:trial_omen",
};

[[nodiscard]] inline std::string effectName(int id) {
    if (id > 0 && static_cast<std::size_t>(id) < std::size(effectNames) && !effectNames[id].empty())
        return std::string(effectNames[id]);
    return "minecraft:unknown_effect_" + std::to_string(id);
}

// Accepts a canonical `minecraft:name`, a bare `name`, or an already-synthetic
// unknown id. All canonical IDs returned by getEffects() go through here so a
// shadow entry written as bare `levitation` can never masquerade as a second
// effect next to the real `minecraft:levitation` entry.
[[nodiscard]] inline std::string canonicalEffectName(std::string_view typeId) {
    if (typeId.empty()) return {};
    if (typeId.find(':') != std::string_view::npos) return std::string(typeId);
    return std::string("minecraft:") + std::string(typeId);
}

[[nodiscard]] inline std::optional<int> effectIdForName(std::string_view typeId) {
    const auto canonical = canonicalEffectName(typeId);
    for (std::size_t i = 1; i < std::size(effectNames); ++i)
        if (effectNames[i] == canonical) return static_cast<int>(i);
    return std::nullopt;
}
} // namespace bedrock::detail
