#pragma once
#include <nlohmann/json.hpp>
#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace bedrock {
struct WorldRenderProfile {
    struct Function { std::uint32_t rva{}; std::vector<std::uint8_t> guard; };
    std::array<Function, 7> functions;
    std::uint32_t actorTypeValueOffset{}, playerTypeId{}, commonMaterialGroup{}, player{}, origin{}, material{}, tess{}, shader{}, translation{}, building{}, special{}, dirty{};
};
inline bool parseWorldRenderProfile(std::string_view source, WorldRenderProfile& out, std::string& error) {
    try {
        const auto root = nlohmann::json::parse(source);
        const auto& data = root.at("native_world_render");
        if (data.at("schema") != 1 || data.at("pointer_size") != 8)
            throw std::runtime_error("unsupported schema/pointer size");
        const auto number = [](const nlohmann::json& v) -> std::uint32_t {
            const auto n = v.is_string() ? std::stoull(v.get<std::string>(), nullptr, 0)
                                         : v.get<std::uint64_t>();
            if (n > UINT32_MAX) throw std::runtime_error("integer overflow");
            return static_cast<std::uint32_t>(n);
        };
        const auto imageSize = number(root.at("target").at("size_of_image"));
        WorldRenderProfile result;
        constexpr std::array names{"render_level", "begin", "vertex", "color", "render_mesh", "hashed_name_ctor", "material_ctor"};
        for (std::size_t i=0; i<names.size(); ++i) {
            const auto& value = data.at("functions").at(names[i]);
            auto& f = result.functions[i];
            f.rva = number(value.at("rva"));
            const auto bytes = value.at("entry_check").get<std::string>();
            if (bytes.size() < 47 || bytes.size() > 191 || (bytes.size()+1)%3)
                throw std::runtime_error("invalid entry guard length");
            const auto digit=[](char c) -> unsigned {
                if(c>='0' && c<='9') return c-'0';
                if(c>='a' && c<='f') return c-'a'+10;
                if(c>='A' && c<='F') return c-'A'+10;
                throw std::runtime_error("invalid entry guard byte");
            };
            for (std::size_t j=0; j<bytes.size(); j+=3) {
                f.guard.push_back(static_cast<std::uint8_t>((digit(bytes[j])<<4)|digit(bytes[j+1])));
                if(j+2<bytes.size() && bytes[j+2]!=' ') throw std::runtime_error("invalid entry guard separator");
            }
            if (!f.rva || f.rva >= imageSize || f.guard.size() > imageSize-f.rva)
                throw std::runtime_error("function outside image");
            for (std::size_t j=0;j<i;++j) if (result.functions[j].rva==f.rva)
                throw std::runtime_error("duplicate function RVA");
        }
        result.commonMaterialGroup=number(data.at("common_material_group_rva"));
        if (!result.commonMaterialGroup || result.commonMaterialGroup>=imageSize ||
            imageSize-result.commonMaterialGroup<sizeof(void*)) throw std::runtime_error("material group outside image");
        const auto& actorType=root.at("native_actors").at("actor_type");
        result.actorTypeValueOffset=number(actorType.at("value_offset"));
        result.playerTypeId=number(actorType.at("player_type_id"));
        if(number(actorType.at("value_size"))!=sizeof(std::uint32_t) ||
            result.actorTypeValueOffset>0x10000 || !result.playerTypeId)
            throw std::runtime_error("invalid ActorType filter layout");
        const auto& offsets=data.at("offsets");
        const auto offset=[&](const char* key) {
            const auto n=number(offsets.at(key));
            if (!n || n>0x10000) throw std::runtime_error("invalid object offset");
            return n;
        };
        result.player=offset("level_renderer_player"); result.origin=offset("render_origin");
        result.material=offset("selection_material"); result.tess=offset("screen_tessellator");
        result.shader=offset("screen_shader_color"); result.translation=offset("tess_translation");
        result.building=offset("tess_building"); result.special=offset("tess_special_mode");
        result.dirty=offset("shader_color_dirty");
        out=std::move(result); return true;
    } catch (const std::exception& e) { error=std::string("native_world_render: ")+e.what(); return false; }
}
}
