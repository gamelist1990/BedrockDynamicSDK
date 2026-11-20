#pragma once
#include "tablist/config.hpp"
#include "tablist/model.hpp"
#include <array>
#include <cstdint>
#include <span>
#include <string>

namespace dynamic_client::tab_overlay {
struct PlayerRow {
    std::string name;
    std::string heldItemName;
    unsigned nameColorOrder = 28;
    bool hasNameColor = false;
    tablist::MurderRole murderRole = tablist::MurderRole::None;
    bool spectator = false;
    std::array<std::uint32_t, 64> face{};
    bool hasFace = false;
};

void paintTabList(const tablist::Config& settings, std::span<const PlayerRow> players,
                  bool packetRosterMode);
}
