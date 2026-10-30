#pragma once
#include <nlohmann/json.hpp>
#include <string_view>
#include <string>
#include <stdexcept>
namespace bedrock {
// Schema 1 profiles support Minecraft class names under layouts. Legacy files
// remain readable. Aliases exist only in this adapter view, never in the file.
inline nlohmann::json parseNativeProfile(std::string_view text) {
    auto root = nlohmann::json::parse(text.begin(), text.end());
    if (!root.contains("layouts")) return root;
    auto& layouts = root.at("layouts");
    if (!layouts.is_object()) throw std::invalid_argument("layouts must be an object");
    const auto expand=[&](const auto& self,nlohmann::json& node,unsigned depth)->void {
        if(depth>64)throw std::invalid_argument("layout reference nesting limit");
        if(node.is_object()){
            if(node.size()==1&&node.contains("symbol")&&node.at("symbol").is_string()){
                const auto name=node.at("symbol").get<std::string>();
                const auto definition=root.at("symbols").at(name);
                if(!definition.contains("rva")||!definition.contains("entry_check"))
                    throw std::invalid_argument("layout reference is not a canonical function: "+name);
                node=definition;node["symbol"]=name;return;
            }
            for(auto& child:node.items())self(self,child.value(),depth+1);
        }else if(node.is_array())for(auto& child:node)self(self,child,depth+1);
    };
    expand(expand,layouts,0);
    if (layouts.contains("GameCore")) {
        if (root.contains("native_keyboard")) throw std::invalid_argument("duplicate layout: GameCore");
        root["native_keyboard"] = layouts.at("GameCore");
    }
    if (layouts.contains("Actor")) {
        if (root.contains("native_actors")) throw std::invalid_argument("duplicate layout: Actor");
        root["native_actors"] = layouts.at("Actor");
    }
    if (layouts.contains("ItemStack")) {
        if (root.contains("native_items")) throw std::invalid_argument("duplicate layout: ItemStack");
        root["native_items"] = layouts.at("ItemStack");
    }
    if (layouts.contains("LocalPlayer")) {
        if (root.contains("native_player_identity")) throw std::invalid_argument("duplicate layout: LocalPlayer");
        root["native_player_identity"] = layouts.at("LocalPlayer");
    }
    if (layouts.contains("BlockSource")) {
        if (root.contains("native_world")) throw std::invalid_argument("duplicate layout: BlockSource");
        root["native_world"] = layouts.at("BlockSource");
    }
    if (layouts.contains("StateVectorComponent")) {
        if (root.contains("native_motion")) throw std::invalid_argument("duplicate layout: StateVectorComponent");
        root["native_motion"] = layouts.at("StateVectorComponent");
    }
    if (layouts.contains("Camera")) {
        if (root.contains("native_camera")) throw std::invalid_argument("duplicate layout: Camera");
        root["native_camera"] = layouts.at("Camera");
    }
    if (layouts.contains("PacketSender")) {
        if (root.contains("native_packets")) throw std::invalid_argument("duplicate layout: PacketSender");
        root["native_packets"] = layouts.at("PacketSender");
    }
    if (layouts.contains("CommandRegistry")) {
        if (root.contains("native_client_commands")) throw std::invalid_argument("duplicate layout: CommandRegistry");
        root["native_client_commands"] = layouts.at("CommandRegistry");
    }
    if (layouts.contains("GuiData")) {
        if (root.contains("native_gui")) throw std::invalid_argument("duplicate layout: GuiData");
        root["native_gui"] = layouts.at("GuiData");
    }
    if (layouts.contains("LevelRenderer")) {
        if (root.contains("native_world_render")) throw std::invalid_argument("duplicate layout: LevelRenderer");
        root["native_world_render"] = layouts.at("LevelRenderer");
    }
    if (layouts.contains("ClientInstance")) {
        if (root.contains("native_world_events")) throw std::invalid_argument("duplicate layout: ClientInstance");
        root["native_world_events"] = layouts.at("ClientInstance");
    }
    if (layouts.contains("CommandRequestPacket")) {
        if (root.contains("native_command_requests")) throw std::invalid_argument("duplicate layout: CommandRequestPacket");
        root["native_command_requests"] = layouts.at("CommandRequestPacket");
    }
    return root;
}
// Fill a missing/outdated layout from the bundled definition only when all
// executable fingerprints match. Legacy caches may have no layouts object.
inline nlohmann::json selectNativeLayoutProfile(std::string_view selected,
                                                std::string_view embedded,
                                                const char *layout, int minimumRevision = 0) {
    auto current = parseNativeProfile(selected);
    const auto usable = [&](const nlohmann::json &root) {
        return root.contains("layouts") && root.at("layouts").contains(layout) &&
            root.at("layouts").at(layout).value("revision", 0) >= minimumRevision;
    };
    if (usable(current)) return current;
    auto fallback = parseNativeProfile(embedded);
    for (const auto *field : {"sha256", "text_sha256", "size_of_image", "text_rva", "text_size"})
        if (current.at("target").at(field) != fallback.at("target").at(field))
            throw std::invalid_argument("embedded layout belongs to another executable");
    if (!usable(fallback))
        throw std::invalid_argument(std::string("missing or outdated SDK layout: ") + layout);
    return fallback;
}
} // namespace bedrock
