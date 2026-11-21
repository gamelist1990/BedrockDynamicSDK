#pragma once
#include <bedrock/packets/protocol_codec.hpp>
namespace bedrock {
// Wire-only action encoding: no guessed native variant layout is required.
inline std::string dropRequestPayload(std::int32_t requestId,int slot,std::int32_t netId,int count) {
    if (slot<0 || slot>=36 || netId<=0 || count<1 || count>64)
        throw std::runtime_error("invalid inventory drop request");
    using Json=nlohmann::json;
    const Json source{{"slot_type",{{"container_id","hotbar_and_inventory"},{"dynamic_container_id",nullptr}}},{"slot",slot},{"stack_id",netId}};
    const Json action{{"type_id","drop"},{"legacy_type_id",0},{"count",count},{"source",source},{"randomly",false}};
    const Json request{{"request_id",requestId},{"actions",Json::array({action})},{"custom_names",Json::array()},{"cause",-1}};
    return ProtocolCodec{}.encode(PacketId::ItemStackRequest,Json{{"requests",Json::array({request})}});
}
}
