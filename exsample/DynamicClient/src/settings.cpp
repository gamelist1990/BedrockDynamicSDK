#include "client.hpp"
#include <bedrock/sdk.hpp>
#include <array>
#include <mutex>
#include <set>
#include <variant>

namespace dynamic_client {
namespace {
using bedrock::sdk::Setting;
using bedrock::sdk::SettingValue;

const bedrock::sdk::Translation en[] = {
    {"mod.name", "DynamicClient"}, {"title", "DynamicClient Settings"},
    {"zoom", "Zoom"}, {"zoom.help", "Assign the key under Mod KeyBind. Wheel adjustments last until restart."},
    {"zoom.enabled", "Zoom enabled"}, {"zoom.mode", "Zoom activation"}, {"zoom.hold", "Hold"},
    {"zoom.toggle", "Toggle"}, {"zoom.multiplier", "Zoom multiplier"}, {"zoom.smooth", "Smooth zoom"},
    {"zoom.scroll", "Adjust zoom with wheel"}, {"sprint", "AutoSprint"},
    {"sprint.help", "Sprint while holding W. Hold Shift to pause."}, {"tablist", "TabList"},
    {"tablist.help", "Hold the assigned key in-game."}, {"tablist.enabled", "TabList enabled"},
    {"tablist.faces", "Player faces"}, {"tablist.colors", "Sort by name color"},
    {"tablist.items", "Show held items"}, {"tablist.roles", "Show inferred minigame roles"},
    {"tablist.spectators", "Mark spectators"}, {"brightness", "Fullbright"},
    {"brightness.help", "Override gamma without changing Minecraft options."},
    {"brightness.gamma", "Fullbright gamma"}, {"freecam", "Freecam"},
    {"freecam.warning", "Using Freecam where it is not permitted may result in a ban. No guarantees are provided; use at your own risk."}, {"freecam.enabled", "Allow Freecam"},
    {"freecam.help", "Press the assigned key in-game to toggle the camera."}, {"freecam.speed", "Freecam speed"},
    {"freecam.fast", "Freecam fast multiplier"}, {"freecam.sensitivity", "Freecam sensitivity"},
    {"freecam.damage", "Exit Freecam on damage"}, {"freecam.boxes", "Freecam player boxes"},
    {"freecam.spectator", "Freecam underground view (experimental)"},
};
const bedrock::sdk::Translation ja[] = {
    {"mod.name", "DynamicClient"}, {"title", "DynamicClient 設定"}, {"zoom", "ズーム"},
    {"zoom.help", "Mod キー設定で割り当てます。ホイールで倍率を調整できます。"},
    {"zoom.enabled", "ズームを有効にする"}, {"zoom.mode", "ズームの操作方法"}, {"zoom.hold", "長押し"},
    {"zoom.toggle", "切り替え"}, {"zoom.multiplier", "ズーム倍率"}, {"zoom.smooth", "滑らかにズームする"},
    {"zoom.scroll", "ホイールでズームを調整する"}, {"sprint", "自動ダッシュ"},
    {"sprint.help", "W を押している間はダッシュします。Shift で一時停止します。"},
    {"tablist", "プレイヤー一覧"}, {"tablist.help", "プレイ中に割り当てたキーを押し続けます。"},
    {"tablist.enabled", "プレイヤー一覧を有効にする"}, {"tablist.faces", "プレイヤーの顔を表示する"},
    {"tablist.colors", "名前の色で並べ替える"}, {"tablist.items", "手に持つアイテムを表示する"},
    {"tablist.roles", "ミニゲームの役職を推定する"}, {"tablist.spectators", "観戦者に印を付ける"},
    {"brightness", "明るさ強調"}, {"brightness.help", "ガンマ値を上書きします。"},
    {"brightness.gamma", "明るさ強調のガンマ値"}, {"freecam", "フリーカメラ"},
    {"freecam.warning", "許可されていないサーバーで使うと BAN される可能性があります。保証はしません。自己責任で使ってください。"},
    {"freecam.enabled", "フリーカメラを許可する"}, {"freecam.help", "割り当てたキーでカメラを切り替えます。"},
    {"freecam.speed", "フリーカメラ速度"}, {"freecam.fast", "高速移動倍率"},
    {"freecam.sensitivity", "操作感度"}, {"freecam.damage", "ダメージで終了する"},
    {"freecam.boxes", "プレイヤーの当たり判定を表示する"}, {"freecam.spectator", "地下を表示する（実験的）"},
};

zoom::Config zoomConfig;
tablist::Config tabConfig = [] { tablist::Config c; c.murderDetect = false; return c; }();
freecam::Config freecamConfig = [] { freecam::Config c; c.enabled = false; return c; }();
float gamma = 25;
bool fullbright = false;
std::mutex settingsMutex;
std::set<std::string, std::less<>> changedDuringStartup;
bool restoring = true;

void apply(std::string_view id, const SettingValue& value) {
    const auto boolean = [&] { return std::get<bool>(value); };
    const auto number = [&] { return static_cast<float>(std::get<double>(value)); };
    if (id == "zoom_enabled") zoomConfig.enabled = boolean();
    else if (id == "zoom_mode") zoomConfig.mode = std::get<std::string>(value) == "toggle" ? zoom::ActivationMode::Toggle : zoom::ActivationMode::Hold;
    else if (id == "zoom_multiplier") zoomConfig.zoomMultiplier = number();
    else if (id == "zoom_smooth") zoomConfig.smooth = boolean();
    else if (id == "zoom_scroll") zoomConfig.scrollAdjustment = boolean();
    else if (id == "sprint_enabled") setSprintEnabled(boolean());
    else if (id == "tab_enabled") tabConfig.enabled = boolean();
    else if (id == "tab_faces") tabConfig.showFaces = boolean();
    else if (id == "tab_colors") tabConfig.sortByNameColor = boolean();
    else if (id == "tab_items") tabConfig.showHeldItems = boolean();
    else if (id == "tab_roles") tabConfig.murderDetect = boolean();
    else if (id == "tab_spectators") tabConfig.gamemodeCheck = boolean();
    else if (id == "brightness_enabled") fullbright = boolean();
    else if (id == "brightness_gamma") gamma = number();
    else if (id == "freecam_enabled") freecamConfig.enabled = boolean();
    else if (id == "freecam_damage") freecamConfig.disableOnDamage = boolean();
    else if (id == "freecam_boxes") freecamConfig.playerHitboxes = boolean();
    else if (id == "freecam_spectator") freecamConfig.spectatorMode = boolean();
    else if (id == "freecam_speed") freecamConfig.speed = number();
    else if (id == "freecam_fast") freecamConfig.fastMultiplier = number();
    else if (id == "freecam_sensitivity") freecamConfig.sensitivity = number();
    else return;
    if (id.starts_with("zoom_")) setZoomConfig(zoomConfig);
    if (id.starts_with("tab_")) setTabListConfig(tabConfig);
    if (id.starts_with("brightness_")) setBrightness(gamma, fullbright);
    if (id.starts_with("freecam_")) setFreecamConfig(freecamConfig);
}
}

void registerSettings(const bedrock::sdk::Mod& mod, bedrock::sdk::Logger& logger) {
    using namespace bedrock::sdk;
    auto page = mod.settings("title")
        .modName("mod.name")
        .add(Setting::text("zoom", "zoom", "zoom.help"))
        .add(Setting::toggle("zoom_enabled", "zoom.enabled", true))
        .add(Setting::choice("zoom_mode", "zoom.mode", "hold", {{"hold", "zoom.hold"}, {"toggle", "zoom.toggle"}}))
        .add(Setting::slider("zoom_multiplier", "zoom.multiplier", 4, 1, 50, .5))
        .add(Setting::toggle("zoom_smooth", "zoom.smooth", true))
        .add(Setting::toggle("zoom_scroll", "zoom.scroll", true))
        .add(Setting::toggle("sprint_enabled", "sprint", true))
        .add(Setting::toggle("tab_enabled", "tablist.enabled", true))
        .add(Setting::toggle("tab_faces", "tablist.faces", true))
        .add(Setting::toggle("tab_colors", "tablist.colors", false))
        .add(Setting::toggle("tab_items", "tablist.items", true))
        .add(Setting::toggle("tab_roles", "tablist.roles", false))
        .add(Setting::toggle("tab_spectators", "tablist.spectators", true))
         .add(Setting::toggle("brightness_enabled", "brightness", false))
         .add(Setting::slider("brightness_gamma", "brightness.gamma", 25, 0, 25, .5))
         .add(Setting::text("freecam_warning", "freecam", "freecam.warning"))
         .add(Setting::toggle("freecam_enabled", "freecam.enabled", false))
        .add(Setting::slider("freecam_speed", "freecam.speed", 8, .5, 64, .5))
        .add(Setting::slider("freecam_fast", "freecam.fast", 3, 1, 8, .5))
        .add(Setting::slider("freecam_sensitivity", "freecam.sensitivity", .15, .01, 1, .01))
        .add(Setting::toggle("freecam_damage", "freecam.damage", true))
        .add(Setting::toggle("freecam_boxes", "freecam.boxes", true))
        .add(Setting::toggle("freecam_spectator", "freecam.spectator", false))
        .locale("en_US", {std::begin(en), std::end(en)})
        .locale("ja_JP", {std::begin(ja), std::end(ja)})
        .onChange([](std::string_view id, const SettingValue& value) {
            std::lock_guard lock(settingsMutex);
            if (restoring) changedDuringStartup.emplace(id);
            apply(id, value);
        });
    const auto result = page.registerPage();
    if (!result) { logger.error("Stage A settings registration failed: ", result.error().message); return; }
    // Read without the apply mutex: OreUI holds its store lock on callbacks.
    constexpr std::string_view persistedSettings[] = {
        "zoom_enabled", "zoom_mode", "zoom_multiplier", "zoom_smooth", "zoom_scroll",
        "sprint_enabled", "tab_enabled", "tab_faces", "tab_colors", "tab_items",
        "tab_roles", "tab_spectators", "brightness_enabled", "brightness_gamma",
        "freecam_enabled", "freecam_speed", "freecam_fast", "freecam_sensitivity",
        "freecam_damage", "freecam_boxes", "freecam_spectator",
    };
    for (const auto id : persistedSettings) {
        const auto value = page.value(id);
        if (!value) {
            logger.error("Cannot restore ", id, ": ", value.error().message);
            continue;
        }
        std::lock_guard lock(settingsMutex);
        if (!changedDuringStartup.contains(id)) apply(id, value.value());
    }
    {
        std::lock_guard lock(settingsMutex);
        restoring = false;
        changedDuringStartup.clear();
    }
    logger.info("Stage A settings registered");
}
}
