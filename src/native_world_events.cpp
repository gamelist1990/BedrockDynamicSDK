#include <bedrock/profile_json.hpp>
#include <bedrock/native_world_events.hpp>
#include <cstdint>
#include <nlohmann/json.hpp>
#include <stdexcept>

namespace bedrock {
std::optional<NativeWorldEventsLayout> parseNativeWorldEventsLayout(
    std::string_view profile, std::string& error) {
    try {
        const auto root = bedrock::parseNativeProfile(profile);
        const auto& value = root.at("native_world_events");
        if (value.at("schema") != 1 || value.at("pointer_size") != sizeof(void*))
            throw std::runtime_error("unsupported native world event layout");
        const auto& offset = value.at("exit_client_offset");
        if (!offset.is_number_unsigned() && !offset.is_number_integer())
            throw std::runtime_error("exit_client_offset must be an integer");
        const auto signedOffset = offset.get<std::int64_t>();
        if (signedOffset < 0 || signedOffset > 4096 || signedOffset % sizeof(void*) != 0)
            throw std::runtime_error("exit_client_offset is outside the supported range");
        error.clear();
        return NativeWorldEventsLayout{static_cast<std::size_t>(signedOffset)};
    } catch (const std::exception& ex) {
        error = std::string("native_world_events: ") + ex.what();
        return std::nullopt;
    }
}
} // namespace bedrock
