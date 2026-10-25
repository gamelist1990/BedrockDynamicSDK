#pragma once
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

namespace bedrock {
struct NativeWorldEventsLayout {
    // onLevelExit is a GameCallbacks entry; RCX addresses that base subobject.
    std::size_t exitClientOffset{};
};
std::optional<NativeWorldEventsLayout> parseNativeWorldEventsLayout(
    std::string_view profile, std::string& error);
} // namespace bedrock
