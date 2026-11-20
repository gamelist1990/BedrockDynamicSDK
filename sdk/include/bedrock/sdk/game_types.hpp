#pragma once

#include <cstdint>
#include <cstddef>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace bedrock::sdk {

struct Vector3 { float x{}, y{}, z{}; };
struct BlockPosition { int x{}, y{}, z{}; };
struct Rotation { float x{}, y{}; }; // Pitch and yaw, in degrees.

using DynamicProperty = std::variant<bool, double, std::string, Vector3>;

class ItemStack {
public:
    explicit ItemStack(std::string typeId, unsigned amount = 1)
        : typeId_(std::move(typeId)), amount_(amount) {}
    [[nodiscard]] const std::string& typeId() const noexcept { return typeId_; }
    [[nodiscard]] unsigned amount() const noexcept { return amount_; }
private:
    std::string typeId_;
    unsigned amount_;
};

enum class EquipmentSlot { MainHand, Offhand, Head, Chest, Legs, Feet };

class InventoryComponent {
public:
    explicit InventoryComponent(std::vector<std::optional<ItemStack>> slots)
        : slots_(std::move(slots)) {}
    [[nodiscard]] std::size_t size() const noexcept { return slots_.size(); }
    [[nodiscard]] std::optional<ItemStack> getItem(std::size_t slot) const {
        return slot < slots_.size() ? slots_[slot] : std::nullopt;
    }
private:
    std::vector<std::optional<ItemStack>> slots_;
};

struct HealthComponent {
    explicit HealthComponent(float value) : value_(value) {}
    [[nodiscard]] float currentValue() const noexcept { return value_; }
private:
    float value_;
};

struct FoodComponent {
    explicit FoodComponent(float value) : value_(value) {}
    [[nodiscard]] float currentValue() const noexcept { return value_; }
private:
    float value_;
};

struct MovementComponent {
    explicit MovementComponent(Vector3 velocity) : velocity_(velocity) {}
    [[nodiscard]] Vector3 velocity() const noexcept { return velocity_; }
private:
    Vector3 velocity_;
};

struct EffectOptions { std::uint8_t amplifier{}; bool showParticles{true}; };
struct Effect {
    std::string typeId;
    double duration{}; // Ticks, or positive infinity.
    EffectOptions options;
};

} // namespace bedrock::sdk
