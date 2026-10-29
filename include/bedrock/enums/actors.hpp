#pragma once
#include <cstdint>
#include <cstddef>

namespace bedrock {
enum class EquipmentSlot : std::uint8_t { MainHand, Offhand, Head, Chest, Legs, Feet };

enum class NameTagSource : std::uint8_t { ActorGetter, View };
} // namespace bedrock
