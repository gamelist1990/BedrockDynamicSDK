#include "overlay.hpp"
#include <imgui.h>
#include <algorithm>

namespace dynamic_client::tab_overlay {
void drawFacePixels(ImDrawList* draw, const std::array<std::uint32_t, 64>& face,
                    float x, float y, float size) {
    if (!draw) return;
    std::size_t visiblePixels = 0;
    for (const auto color : face)
        if ((color & 0xFFU) != 0) ++visiblePixels;
    if (visiblePixels == 0) return;

    draw->PrimReserve(static_cast<int>(visiblePixels * 6),
                      static_cast<int>(visiblePixels * 4));
    const float pixelSize = size / 8.0f;
    for (std::size_t py = 0; py < 8; ++py) {
        for (std::size_t px = 0; px < 8; ++px) {
            const auto value = face[py * 8 + px];
            if ((value & 0xFFU) == 0) continue;
            const auto color = IM_COL32((value >> 24) & 0xFF,
                                        (value >> 16) & 0xFF,
                                        (value >> 8) & 0xFF,
                                        value & 0xFF);
            const ImVec2 topLeft{x + static_cast<float>(px) * pixelSize,
                                 y + static_cast<float>(py) * pixelSize};
            draw->PrimRect(topLeft,
                           {topLeft.x + pixelSize, topLeft.y + pixelSize}, color);
        }
    }
}

void paintTabList(const tablist::Config& settings, std::span<const PlayerRow> players,
                  bool packetRosterMode) {
    const auto* config = &settings;
    const auto& io = ImGui::GetIO();
    if(players.empty()){
        auto* draw=ImGui::GetForegroundDrawList();const char* title="Players  0";
        const char* message=packetRosterMode?"Waiting for player list. Rejoin the world if needed.":"Waiting for player list.";
        const float width=std::max(280.0f,ImGui::CalcTextSize(message).x+24.0f);
        const ImVec2 start{(io.DisplaySize.x-width)*0.5f,config->topOffset};
        draw->AddRectFilled(start,{start.x+width,start.y+60},IM_COL32(20,24,32,230),6);
        draw->AddText({start.x+12,start.y+8},IM_COL32(240,240,245,255),title);
        draw->AddText({start.x+12,start.y+34},IM_COL32(180,185,195,255),message);
        return;
    }
    const auto layout = tablist::layoutFor(players.size(), config->maximumRows);
    auto* draw = ImGui::GetForegroundDrawList();
    const auto display = ImGui::GetIO().DisplaySize;
    constexpr float horizontalPadding = 10.0f;
    const float faceArea = config->showFaces ? config->faceSize + 7.0f : 8.0f;
    const auto packed = [](tablist::Color color) {
        return ImGui::ColorConvertFloat4ToU32({color.r, color.g, color.b, color.a});
    };
    static constexpr std::array<ImU32, 28> minecraftNameColors{
        IM_COL32(0, 0, 0, 255),       // §0 black
        IM_COL32(0, 0, 170, 255),     // §1 dark blue
        IM_COL32(0, 170, 0, 255),     // §2 dark green
        IM_COL32(0, 170, 170, 255),   // §3 dark aqua
        IM_COL32(170, 0, 0, 255),     // §4 dark red
        IM_COL32(170, 0, 170, 255),   // §5 dark purple
        IM_COL32(255, 170, 0, 255),   // §6 gold
        IM_COL32(170, 170, 170, 255), // §7 gray
        IM_COL32(85, 85, 85, 255),    // §8 dark gray
        IM_COL32(85, 85, 255, 255),   // §9 blue
        IM_COL32(85, 255, 85, 255),   // §a green
        IM_COL32(85, 255, 255, 255),  // §b aqua
        IM_COL32(255, 85, 85, 255),   // §c red
        IM_COL32(255, 85, 255, 255),  // §d light purple
        IM_COL32(255, 255, 85, 255),  // §e yellow
        IM_COL32(255, 255, 255, 255), // §f white
        IM_COL32(221, 214, 5, 255),   // §g minecoin gold
        IM_COL32(227, 212, 209, 255), // §h material quartz
        IM_COL32(154, 92, 198, 255),  // §i rendered purple on the observed server
        IM_COL32(68, 58, 59, 255),    // §j material netherite
        IM_COL32(151, 22, 7, 255),    // §m material redstone
        IM_COL32(180, 104, 77, 255),  // §n material copper
        IM_COL32(222, 177, 45, 255),  // §p material gold
        IM_COL32(17, 159, 54, 255),   // §q material emerald
        IM_COL32(44, 186, 168, 255),  // §s material diamond
        IM_COL32(33, 73, 123, 255),   // §t material lapis
        IM_COL32(154, 92, 198, 255),  // §u material amethyst
        IM_COL32(235, 113, 20, 255),  // §v material resin
    };

    // Player snapshots are capped at 512 in collectRows. Fixed scratch arrays
    // avoid heap allocations on every DXGI frame while Tab is held.
    std::array<float, 512> widths{};
    std::array<float, 512> columnX{};
    struct RowTextMetrics { ImVec2 name{}, role{}, spectator{}, item{}; };
    std::array<RowTextMetrics, 512> textMetrics{};
    std::fill_n(widths.begin(), layout.columns, config->minimumColumnWidth);
    for (std::size_t i = 0; i < players.size(); ++i) {
        const auto column = i / config->maximumRows;
        const auto roleText = players[i].murderRole == tablist::MurderRole::Sheriff
            ? " [Sheriff]" : players[i].murderRole == tablist::MurderRole::Murderer
                ? " [Murderer]" : "";
        auto& metrics = textMetrics[i];
        metrics.name = ImGui::CalcTextSize(players[i].name.c_str());
        metrics.role = ImGui::CalcTextSize(roleText);
        if (players[i].spectator)
            metrics.spectator = ImGui::CalcTextSize(" [Spectator]");
        if (!players[i].heldItemName.empty())
            metrics.item = ImGui::CalcTextSize(players[i].heldItemName.c_str());
        widths[column] = std::max(widths[column],
            metrics.name.x + metrics.role.x + metrics.spectator.x +
            (metrics.item.x > 0.0f ? metrics.item.x + 8.0f : 0.0f) +
            horizontalPadding * 2.0f + faceArea);
    }
    float totalWidth = 0.0f;
    for (std::size_t column = 0; column < layout.columns; ++column)
        totalWidth += widths[column];
    totalWidth += config->columnGap * static_cast<float>(layout.columns - 1);
    const float totalHeight = config->headerHeight +
        config->rowHeight * static_cast<float>(layout.rows);
    const ImVec2 topLeft{(display.x - totalWidth) * 0.5f, config->topOffset};
    const ImVec2 bottomRight{topLeft.x + totalWidth, topLeft.y + totalHeight};

    draw->AddRectFilled(topLeft, bottomRight, packed(config->background), config->cornerRadius);
    draw->AddRect(topLeft, bottomRight, packed(config->border), config->cornerRadius,
                  ImDrawFlags_None, 1.2f);
    const std::string title = "Players  " + std::to_string(players.size());
    const auto titleSize = ImGui::CalcTextSize(title.c_str());
    draw->AddText({topLeft.x + (totalWidth - titleSize.x) * 0.5f,
                   topLeft.y + (config->headerHeight - titleSize.y) * 0.5f},
                  packed(config->text), title.c_str());
    auto separator = config->border;
    separator.a *= 0.55f;
    draw->AddLine({topLeft.x + 8.0f, topLeft.y + config->headerHeight},
                  {bottomRight.x - 8.0f, topLeft.y + config->headerHeight},
                  packed(separator));

    std::fill_n(columnX.begin(), layout.columns, topLeft.x);
    for (std::size_t column = 1; column < layout.columns; ++column)
        columnX[column] = columnX[column - 1] + widths[column - 1] + config->columnGap;
    for (std::size_t i = 0; i < players.size(); ++i) {
        const auto column = i / config->maximumRows;
        const auto row = i % config->maximumRows;
        const auto roleText = players[i].murderRole == tablist::MurderRole::Sheriff
            ? " [Sheriff]" : players[i].murderRole == tablist::MurderRole::Murderer
                ? " [Murderer]" : "";
        const float y = topLeft.y + config->headerHeight +
            config->rowHeight * static_cast<float>(row);
        if ((row & 1U) != 0)
            draw->AddRectFilled({columnX[column], y},
                                {columnX[column] + widths[column], y + config->rowHeight},
                                IM_COL32(255, 255, 255, 10));
        float textX = columnX[column] + horizontalPadding;
        if (config->showFaces && players[i].hasFace) {
            const float faceX = textX;
            const float faceY = y + (config->rowHeight - config->faceSize) * 0.5f;
            drawFacePixels(draw, players[i].face, faceX, faceY, config->faceSize);
            textX += config->faceSize + 7.0f;
        } else {
            draw->AddCircleFilled({textX + 2.5f, y + config->rowHeight * 0.5f},
                                  2.5f, packed(config->accent));
            textX += 9.0f;
        }
        const auto textSize = textMetrics[i].name;
        const ImU32 nameColor = players[i].spectator
            ? IM_COL32(145, 150, 165, 210)
            : players[i].hasNameColor && players[i].nameColorOrder <
            minecraftNameColors.size()
            ? minecraftNameColors[players[i].nameColorOrder]
            : packed(config->text);
        draw->AddText({textX, y + (config->rowHeight - textSize.y) * 0.5f},
                      nameColor, players[i].name.c_str());
        if (players[i].murderRole != tablist::MurderRole::None) {
            const char* roleLabel = players[i].murderRole == tablist::MurderRole::Sheriff
                ? " [Sheriff]" : " [Murderer]";
            const ImU32 roleColor = players[i].murderRole == tablist::MurderRole::Sheriff
                ? IM_COL32(85, 220, 255, 255) : IM_COL32(255, 85, 85, 255);
            draw->AddText({textX + textSize.x,
                           y + (config->rowHeight - textSize.y) * 0.5f},
                          roleColor, roleLabel);
        }
        if (players[i].spectator) {
            const float suffixX = textX + textSize.x + textMetrics[i].role.x;
            draw->AddText({suffixX, y + (config->rowHeight - textSize.y) * 0.5f},
                          IM_COL32(165, 170, 185, 230), " [Spectator]");
        }
        if (!players[i].heldItemName.empty()) {
            const float itemX = textX + textSize.x + textMetrics[i].role.x +
                textMetrics[i].spectator.x + 8.0f;
            draw->AddText({itemX, y + (config->rowHeight - textMetrics[i].item.y) * 0.5f},
                          IM_COL32(190, 198, 210, 220), players[i].heldItemName.c_str());
        }
    }
}

}
