#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace bedrock {

// Version-specific layout for the live Actor ECS. Values are accepted only
// from the exact executable profile and remain candidates until runtime validation.
struct NativeActorLayout {
    std::size_t actorEntityContextOffset{};
    std::size_t contextRegistryOffset{};
    std::size_t contextEntityIdOffset{};
    std::uint32_t actorOwnerComponentHash{};
    std::uint32_t actorOwnerComponentHashAlternate{};
    std::size_t actorOwnerPointerOffset{};
    std::uint32_t actorTypeComponentHash{};
    std::uint32_t runtimeIdComponentHash{};
    std::size_t runtimeIdValueOffset{};
    std::size_t runtimeIdValueSize{};
    std::uint32_t aabbShapeComponentHash{};
    std::uint32_t aabbShapeComponentHashAlternate{};
    std::size_t aabbMinimumOffset{};
    std::size_t aabbMaximumOffset{};
    std::uint32_t stateVectorComponentHash{};
    std::uint32_t stateVectorComponentHashAlternate{};
    std::size_t statePositionOffset{};
    std::uint32_t actorRotationComponentHash{};
    std::uint32_t actorRotationComponentHashAlternate{};
    std::size_t rotationPitchOffset{};
    std::size_t rotationYawOffset{};
    std::string evidence;

    [[nodiscard]] bool valid() const noexcept;
};

[[nodiscard]] bool parseNativeActorLayout(std::string_view profile,
                                          NativeActorLayout& output,
                                          std::string& error);

} // namespace bedrock
