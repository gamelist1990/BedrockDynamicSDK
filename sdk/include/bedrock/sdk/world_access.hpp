#pragma once
#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include "block_types.hpp"

namespace bedrock::sdk {
class WorldAccess;
namespace detail { void* nativeWorldAccessHandle(WorldAccess&) noexcept; }
struct Vec3Value { float x{}; float y{}; float z{}; };
struct BlockPositionValue { int x{}; int y{}; int z{}; };
struct HotbarSnapshot {
    std::array<bedrock::sdk::HotbarSlot, 9> copied;
    std::array<void*, 9> types{};
    std::array<int, 9> aux{};
};
struct BlockInfoValue { std::string name; void* type{}; void* nativeState{}; };
struct ActorLayoutValue { std::uint32_t actorRotationComponentHash{}; std::size_t rotationPitchOffset{}; std::size_t rotationYawOffset{}; std::uint32_t aabbShapeComponentHash{}; std::uint32_t aabbShapeComponentHashAlternate{}; std::size_t aabbMinimumOffset{}; std::size_t aabbMaximumOffset{}; };
class WorldAccess { public:
    WorldAccess(); ~WorldAccess(); WorldAccess(const WorldAccess&) = delete; WorldAccess& operator=(const WorldAccess&) = delete;
    void load(const std::string& profileJson); void loadWorld(const std::string& profileJson);
    bool placementReady() const noexcept; std::uint64_t layout(std::string_view name) const;
    void reserveCaches(std::size_t states, std::size_t names); void clear();
    bool readField(void* object, std::string_view layout, std::uint64_t& value) const noexcept;
    bool readField(void* object, std::string_view layout, std::uint16_t& value) const noexcept;
    bool readField(void* object, std::size_t offset, float& value) const noexcept;
    bool readField(void* object, std::size_t offset, std::uint16_t& value) const noexcept;
    bool readField(void* object, std::size_t offset, std::uint64_t& value) const noexcept;
    bool readField(void* object, std::size_t offset, void*& value) const noexcept;
    bool readField(void* object, std::size_t offset, Vec3Value& value) const noexcept;
    bool writeField(void* object, std::size_t offset, float value) noexcept;
    bool build(void* player, void* gameMode, BlockPos support, unsigned face, Vec3Value eye, Vec3Value hit, bool* accepted) const noexcept;
    bool readField(void* object, std::string_view layout, float& value) const noexcept;
    bool writeField(void* object, std::string_view layout, float value) noexcept;
    void* localPlayer(void* client) const noexcept; void* region(void* client) const noexcept;
    bool pose(void* player, Vec3Value& position, std::uint64_t& runtime) const noexcept;
    void* gameMode(void* player) const noexcept; void* component(void* actor, std::uint32_t hash) const noexcept;
    int selected(void* player) const noexcept; bool select(void* player, unsigned slot) noexcept;
    bool hotbar(void* player, HotbarSnapshot& snapshot) const noexcept;
    const BlockInfoValue* describe(void* state); std::string typeName(void* type) const;
    ActorLayoutValue actors() const noexcept;
    Cell getBlock(void* source, BlockPos pos, bool inspectSupport = false) const;
    Cell describeCell(void* state, BlockPos pos = {}) const;
    bool predict(void* type, void* player, BlockPositionValue target, unsigned face, Vec3Value fractional,
                 int aux, float yaw, float pitch, void*& result) const noexcept;
private: struct Impl; std::unique_ptr<Impl> impl_;
    friend void* detail::nativeWorldAccessHandle(WorldAccess&) noexcept; };
} // namespace bedrock::sdk
