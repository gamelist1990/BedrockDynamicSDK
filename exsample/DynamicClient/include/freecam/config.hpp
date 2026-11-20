#pragma once
#include <algorithm>
#include <cmath>
#include <string>
#include <string_view>
#include <nlohmann/json.hpp>
namespace freecam {
struct Config {
    bool enabled=true, allowInteraction=false, disableOnDamage=true, playerHitboxes=true;
    bool spectatorMode=false, fullBright=true, visibilityDiagnostics=false;
    unsigned toggleKey=77, forward=87, back=83, left=65, right=68, up=32, down=16, fast=17;
    float speed=8.f, fastMultiplier=3.f, sensitivity=0.15f, radius=32.f;
    unsigned maxPlayers=64, captureRows=8;
};
inline nlohmann::json configJson(const Config& c) {
    return {{"config_revision",2},{"enabled",c.enabled},{"toggle_key",c.toggleKey},{"allow_interaction",c.allowInteraction},
        {"disable_on_damage",c.disableOnDamage},{"player_hitboxes",c.playerHitboxes},
        {"spectator_mode",c.spectatorMode},{"full_bright",c.fullBright},{"visibility_diagnostics",c.visibilityDiagnostics},
        {"speed_blocks_per_second",c.speed},{"fast_multiplier",c.fastMultiplier},
        {"mouse_sensitivity",c.sensitivity},{"hitbox_radius",c.radius},{"max_players",c.maxPlayers},
        {"capture_rows_per_update",c.captureRows},
        {"keys",{{"forward",c.forward},{"back",c.back},{"left",c.left},{"right",c.right},
                 {"up",c.up},{"down",c.down},{"fast",c.fast}}}};
}
inline Config parseConfig(std::string_view source) {
    const auto j=nlohmann::json::parse(source); if(!j.is_object()) throw std::runtime_error("config must be an object");
    Config c;
    c.enabled=j.value("enabled",c.enabled);c.allowInteraction=j.value("allow_interaction",c.allowInteraction);
    c.disableOnDamage=j.value("disable_on_damage",c.disableOnDamage);c.playerHitboxes=j.value("player_hitboxes",c.playerHitboxes);
    c.visibilityDiagnostics=j.value("visibility_diagnostics",c.visibilityDiagnostics);
    c.spectatorMode=j.value("spectator_mode",c.spectatorMode);c.fullBright=j.value("full_bright",c.fullBright);
    const auto integer=[](const nlohmann::json& v,unsigned lo,unsigned hi) {
        if(!v.is_number_integer()) throw std::runtime_error("expected integer");
        const auto n=v.get<long long>();if(n<lo||n>hi) throw std::runtime_error("integer out of range");return static_cast<unsigned>(n);
    };
    if(j.contains("toggle_key")) c.toggleKey=integer(j.at("toggle_key"),1,255);
    if(j.contains("max_players")) c.maxPlayers=integer(j.at("max_players"),1,128);
    if(j.contains("capture_rows_per_update")) c.captureRows=integer(j.at("capture_rows_per_update"),1,16);
    const auto number=[&](const char* key,float value,float lo,float hi) {
        const float n=j.value(key,value);if(!std::isfinite(n)||n<lo||n>hi) throw std::runtime_error(std::string(key)+" out of range");return n;
    };
    c.speed=number("speed_blocks_per_second",c.speed,0.1f,64.f);
    c.fastMultiplier=number("fast_multiplier",c.fastMultiplier,1.f,8.f);
    c.sensitivity=number("mouse_sensitivity",c.sensitivity,0.001f,5.f);
    c.radius=number("hitbox_radius",c.radius,1.f,100.f);
    if(j.contains("keys")) {
        const auto& k=j.at("keys");if(!k.is_object()) throw std::runtime_error("keys must be an object");
        const auto key=[&](const char* name,unsigned& target){if(k.contains(name)) target=integer(k.at(name),1,255);};
        key("forward",c.forward);key("back",c.back);key("left",c.left);key("right",c.right);
        key("up",c.up);key("down",c.down);key("fast",c.fast);
    }
    for(unsigned key:{c.forward,c.back,c.left,c.right,c.up,c.down,c.fast})
        if(key==c.toggleKey) throw std::runtime_error("toggle_key conflicts with movement key");
    return c;
}
}
