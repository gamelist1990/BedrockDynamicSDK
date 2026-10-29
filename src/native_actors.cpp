#include <bedrock/profiles/profile_json.hpp>
#include <bedrock/native/actors.hpp>

#include <nlohmann/json.hpp>

#include <limits>
#include <stdexcept>
#include <utility>

namespace bedrock {
namespace {
std::size_t number(const nlohmann::json& parent, const char* key) {
    const auto& value = parent.at(key);
    const auto result = value.is_string()
        ? std::stoull(value.get<std::string>(), nullptr, 0)
        : value.get<std::uint64_t>();
    if (result > std::numeric_limits<std::size_t>::max())
        throw std::invalid_argument(std::string{"native_actors."} + key + " is too large");
    return static_cast<std::size_t>(result);
}
}

bool NativeActorLayout::valid() const noexcept {
    return actorEntityContextOffset <= 0x10000 && contextRegistryOffset <= 0x10000 &&
           contextEntityIdOffset <= 0x10000 && actorOwnerComponentHash != 0 &&
           actorOwnerPointerOffset <= 0x10000 && actorTypeComponentHash != 0 &&
           (actorUniqueIdComponentHash == 0 ||
            (actorUniqueIdValueOffset <= 0x10000 &&
             actorUniqueIdValueSize == sizeof(std::uint64_t))) &&
           runtimeIdComponentHash != 0 && runtimeIdValueOffset <= 0x10000 &&
           runtimeIdValueSize == sizeof(std::uint64_t) && aabbShapeComponentHash != 0 &&
           aabbMinimumOffset <= 0x10000 && aabbMaximumOffset <= 0x10000 &&
           stateVectorComponentHash != 0 && statePositionOffset <= 0x10000 &&
           actorRotationComponentHash != 0 && rotationPitchOffset <= 0x10000 &&
           rotationYawOffset <= 0x10000;
}

bool parseNativeActorLayout(std::string_view source, NativeActorLayout& output,
                            std::string& error) {
    try {
        const auto root = bedrock::parseNativeProfile(source);
        const auto& data = root.at("native_actors");
        if (data.at("schema").get<unsigned>() != 1 ||
            data.at("pointer_size").get<unsigned>() != sizeof(void*))
            throw std::invalid_argument("unsupported native_actors schema or pointer size");

        NativeActorLayout value;
        const auto& context = data.at("actor_context");
        value.actorEntityContextOffset = number(context, "actor_offset");
        value.contextRegistryOffset = number(context, "registry_offset");
        value.contextEntityIdOffset = number(context, "entity_id_offset");

        const auto& owner = data.at("actor_owner");
        value.actorOwnerComponentHash = static_cast<std::uint32_t>(number(owner, "component_hash"));
        value.actorOwnerComponentHashAlternate = static_cast<std::uint32_t>(
            number(owner, "component_hash_alternate"));
        value.actorOwnerPointerOffset = number(owner, "actor_pointer_offset");

        value.actorTypeComponentHash = static_cast<std::uint32_t>(
            number(data.at("actor_type"), "component_hash"));
        if (data.contains("actor_unique_id")) {
            const auto& uniqueId = data.at("actor_unique_id");
            value.actorUniqueIdComponentHash = static_cast<std::uint32_t>(
                number(uniqueId, "component_hash"));
            value.actorUniqueIdValueOffset = number(uniqueId, "value_offset");
            value.actorUniqueIdValueSize = number(uniqueId, "value_size");
        }
        const auto& runtimeId = data.at("runtime_id");
        value.runtimeIdComponentHash = static_cast<std::uint32_t>(
            number(runtimeId, "component_hash"));
        value.runtimeIdValueOffset = number(runtimeId, "value_offset");
        value.runtimeIdValueSize = number(runtimeId, "value_size");

        const auto& aabb = data.at("aabb_shape");
        value.aabbShapeComponentHash = static_cast<std::uint32_t>(number(aabb, "component_hash"));
        value.aabbShapeComponentHashAlternate = static_cast<std::uint32_t>(
            number(aabb, "component_hash_alternate"));
        value.aabbMinimumOffset = number(aabb, "minimum_offset");
        value.aabbMaximumOffset = number(aabb, "maximum_offset");

        const auto& state = data.at("state_vector");
        value.stateVectorComponentHash = static_cast<std::uint32_t>(number(state, "component_hash"));
        value.stateVectorComponentHashAlternate = static_cast<std::uint32_t>(
            number(state, "component_hash_alternate"));
        value.statePositionOffset = number(state, "position_offset");

        const auto& rotation = data.at("actor_rotation");
        value.actorRotationComponentHash = static_cast<std::uint32_t>(number(rotation, "component_hash"));
        value.actorRotationComponentHashAlternate = static_cast<std::uint32_t>(
            number(rotation, "component_hash_alternate"));
        value.rotationPitchOffset = number(rotation, "pitch_offset");
        value.rotationYawOffset = number(rotation, "yaw_offset");
        value.evidence = data.value("evidence", std::string{});
        if (!value.valid()) throw std::invalid_argument("native_actors contains out-of-range layout values");
        output = std::move(value);
        error.clear();
        return true;
    } catch (const std::exception& exception) {
        error = exception.what();
        return false;
    }
}

} // namespace bedrock
