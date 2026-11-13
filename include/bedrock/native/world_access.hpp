#pragma once
#include <bedrock/api/blocks.hpp>
#include <bedrock/api/geometry.hpp>
#include <bedrock/detail/native_world_access.hpp>
#include <bedrock/native/actors.hpp>
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>

namespace bedrock {
// Value copy of a native std::string, bounded to 2048 bytes plus terminator.
struct NativeStringCopy {
    char bytes[2049]{};
    std::size_t size{};
};
// One block type/state copied out of the native world by NativeWorldAccess.
struct NativeBlockInfo {
    std::string name;
    void *type{};
    bool solid{};
    std::shared_ptr<const BlockProperties> properties;
};
// Hotbar snapshot: copied SDK slots plus the native type/aux per slot.
struct NativeHotbar {
    Hotbar copied{};
    std::array<void *, 9> types{};
    std::array<int, 9> aux{};
};

// Direct native world access for builder-style Mods: profile-driven function
// bindings (hotbar selection, placement prediction, GameMode build) plus a
// caching block reader. Unlike NativeWorld this is not tick-scoped or
// budgeted; the caller owns call timing on the game thread and must reload
// after a profile or world change. Every native pointer stays inside the
// adapter: blocks leave as Cell/NativeBlockInfo values and the hotbar as a
// NativeHotbar copy.
class NativeWorldAccess {
public:
    NativeWorldAccess() : reader_(bindings_) {}
    NativeWorldAccess(const NativeWorldAccess &) = delete;
    NativeWorldAccess &operator=(const NativeWorldAccess &) = delete;

    // Throws std::runtime_error on an unsupported profile. loadWorld keeps a
    // title/observation-capable state when the placement ABI is incomplete;
    // load additionally requires placementReady().
    void loadWorld(const std::string &profile) {
        bindings_.loadWorld(profile);
        clear();
    }
    void load(const std::string &profile) {
        bindings_.load(profile);
        clear();
    }
    bool placementReady() const noexcept { return bindings_.placementReady; }
    bool scanReady() const noexcept { return bindings_.scanReady; }
    // Named layout offset from the loaded profile; throws when absent.
    std::size_t layout(std::string_view name) const { return bindings_.layout(name); }
    const NativeActorLayout &actors() const noexcept { return bindings_.actors; }

    // Guarded native field access at a profile layout offset.
    template <class T> bool readField(const void *ptr, std::size_t offset, T &out) const noexcept {
        return world_native::read(ptr, offset, out);
    }
    template <class T> bool writeField(void *ptr, std::size_t offset, const T &value) const noexcept {
        return world_native::write(ptr, offset, value);
    }

    void *localPlayer(void *client) const noexcept { return bindings_.local(client); }
    void *region(void *client) const noexcept { return bindings_.region(client); }
    bool pose(void *player, Vec3 &position, std::uint64_t &runtime) const noexcept {
        return bindings_.pose(player, position, runtime);
    }
    void *gameMode(void *player) const noexcept { return bindings_.gameMode(player); }
    void *component(void *actor, std::uint32_t hash) const noexcept {
        return bindings_.component(actor, hash);
    }
    int selected(void *player) const noexcept { return bindings_.selected(player); }
    bool select(void *player, unsigned slot) const noexcept { return bindings_.select(player, slot); }
    // Runs the game's own placement-state predictor with a temporary view
    // rotation; the native state pointer leaves only for immediate identity
    // comparison or describe() within the same tick.
    bool predict(void *type, void *player, const BlockPos &target, unsigned face, Vec3 fractional,
                 int aux, float yaw, float pitch, void **result) const noexcept {
        return bindings_.predict(type, player, target, face, fractional, aux, yaw, pitch, result);
    }
    bool build(void *player, void *gameMode, const BlockPos &support, unsigned face, Vec3 eye,
               Vec3 hit, bool *accepted) const noexcept {
        return bindings_.build(player, gameMode, support, face, eye, hit, accepted);
    }

    void clear() {
        reader_.clear();
        described_.clear();
    }
    void reserveCaches(std::size_t states, std::size_t names) {
        reader_.states.reserve(states);
        reader_.names.reserve(names);
        described_.reserve(states);
    }
    Cell getBlock(void *source, BlockPos pos, bool inspectSupport = false) {
        return reader_.getBlock(source, pos, inspectSupport);
    }
    // Describes a native block state; the result stays valid until clear().
    const NativeBlockInfo *describe(void *state) {
        if (!state)
            return nullptr;
        if (auto it = described_.find(state); it != described_.end())
            return &it->second;
        auto *info = reader_.describe(state);
        if (!info)
            return nullptr;
        NativeBlockInfo copy;
        copy.name = info->name;
        copy.type = info->type;
        copy.solid = info->solid;
        copy.properties = info->properties;
        return &described_.emplace(state, std::move(copy)).first->second;
    }
    Cell describeCell(void *state, BlockPos pos = {}) { return reader_.describeCell(state, pos); }
    std::string typeName(void *type) { return reader_.typeName(type); }
    bool hotbar(void *player, NativeHotbar &result) {
        world_native::NativeHotbar native;
        if (!reader_.hotbar(player, native))
            return false;
        result.copied = native.copied;
        result.types = native.types;
        result.aux = native.aux;
        return true;
    }

private:
    world_native::Bindings bindings_;
    world_native::Reader reader_;
    std::unordered_map<void *, NativeBlockInfo> described_;
};
} // namespace bedrock
