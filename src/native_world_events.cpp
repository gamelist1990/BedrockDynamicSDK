#include <bedrock/profiles/profile_json.hpp>
#include <bedrock/native/world_events.hpp>
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
        NativeWorldEventsLayout result{static_cast<std::size_t>(signedOffset)};
        if(value.contains("available_events")) {
            const auto& events=value.at("available_events");
            if(!events.is_array() || events.empty())throw std::runtime_error("available_events must be a nonempty array");
            result.events.fill(false);
            const std::array<std::string,4> names{"onPlayerLoaded","onLevelExit","onDimensionChangedEvent","onCancelJoinGame"};
            for(const auto& name:events) {
                bool found=false;
                for(std::size_t i=0;i<names.size();++i)if(name==names[i]){result.events[i]=true;found=true;}
                if(!found)throw std::runtime_error("unsupported world event name");
            }
            if(!result.events[1] || !result.events[2]) throw std::runtime_error("exit and dimension events are required");
        }
        error.clear(); return result;
    } catch (const std::exception& ex) {
        error = std::string("native_world_events: ") + ex.what();
        return std::nullopt;
    }
}
} // namespace bedrock
