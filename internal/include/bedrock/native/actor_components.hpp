#pragma once
#include <bedrock/detail/native_world_access.hpp>
#include <bedrock/detail/native_profile_functions.hpp>
#include <optional>

namespace bedrock {
// Owning-game-tick component access. Hashes and field positions come from the
// selected exact-build data; no component type registration is needed in Mods.
class NativeActorComponents {
  public:
    using Entity = world_native::Entity;
    using Registry = entt::basic_registry<Entity>;
    void configure(const NativeActorLayout &actors, const nlohmann::json &profile) {
        layout_ = actors;
        ready_ = layout_.valid();
        const auto root =
            profile.contains("native_camera") ? profile : parseNativeProfile(profile.dump());
        gameTypeHash_ = 0;
        gameTypeOffset_ = 0;
        // Runtime IDs and ordinary components do not require Camera metadata.
        // Game-type display is an optional reader for profiles that provide it.
        if (!root.contains("native_camera") || !root.at("native_camera").contains("visual"))
            return;
        const auto &visual = root.at("native_camera").at("visual");
        if (!visual.contains("game_type_component_hash"))
            return;
        gameTypeHash_ = native_profile::number(visual.at("game_type_component_hash"));
        gameTypeOffset_ = native_profile::number(visual.at("game_type_value_offset"));
        if (!gameTypeHash_ || gameTypeOffset_ > 4096 || visual.at("game_type_value_size") != 4)
            throw std::runtime_error("invalid Actor game type component layout");
    }
    void *component(void *actor, std::uint32_t hash) const noexcept {
        if (!ready_ || !hash)
            return nullptr;
        void *registry{};
        std::uint32_t id{};
        if (!native_profile::read(actor,
                                  layout_.actorEntityContextOffset + layout_.contextRegistryOffset,
                                  registry) ||
            !native_profile::read(
                actor, layout_.actorEntityContextOffset + layout_.contextEntityIdOffset, id) ||
            !registry)
            return nullptr;
        __try {
            auto *typed = static_cast<Registry *>(registry);
            const Entity entity{id};
            auto *storage = typed->storage(hash);
            return typed->valid(entity) && storage && storage->contains(entity)
                       ? storage->value(entity)
                       : nullptr;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return nullptr;
        }
    }
    std::optional<std::uint64_t> runtimeId(void *actor) const noexcept {
        std::uint64_t value{};
        if (!native_profile::read(component(actor, layout_.runtimeIdComponentHash),
                                  layout_.runtimeIdValueOffset, value) ||
            !value)
            return std::nullopt;
        return value;
    }
    std::optional<std::int32_t> gameType(void *actor) const noexcept {
        std::int32_t value{};
        if (!native_profile::read(component(actor, gameTypeHash_), gameTypeOffset_, value))
            return std::nullopt;
        return value;
    }

  private:
    NativeActorLayout layout_;
    std::uint32_t gameTypeHash_{};
    std::size_t gameTypeOffset_{};
    bool ready_{};
};
} // namespace bedrock
