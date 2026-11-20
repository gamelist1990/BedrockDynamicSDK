#pragma once
#include <cstdint>
#include <cstddef>

namespace bedrock {
enum class EquipmentSlot : std::uint8_t { MainHand, Offhand, Head, Chest, Legs, Feet };

enum class NameTagSource : std::uint8_t { ActorGetter, View };

// Full ActorTypeComponent value, not a masked protocol/type ID.
// Reference: LeviLamina src/mc/world/actor/ActorType.h.
enum class ActorType : std::uint32_t { ItemEntity = 64 };
} // namespace bedrock
