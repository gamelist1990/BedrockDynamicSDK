#include "bedrock/detail/mod_settings_oreui.hpp"

#include <algorithm>
#include <cmath>

#include <nlohmann/json.hpp>

namespace bedrock::detail::oreui_settings::impl {
namespace {
using Json = nlohmann::json;

int digit(char value) {
    if (value >= '0' && value <= '9') return value - '0';
    if (value >= 'a' && value <= 'f') return value - 'a' + 10;
    return -1;
}

bool sequence(std::string_view value) {
    return !value.empty() && value.size() <= 16 &&
        std::ranges::all_of(value, [](char c) { return c >= '0' && c <= '9'; });
}

constexpr std::string_view kindName(Kind kind) {
    switch (kind) {
    case Kind::Boolean: return "boolean";
    case Kind::Number: return "number";
    case Kind::Option: return "option";
    case Kind::String: return "string";
    case Kind::Action: return "action";
    case Kind::Text: return "text";
    case Kind::Image: return "image";
    case Kind::Key: return "key";
    }
    return {};
}

const std::string& resolve(const DisplayText& text, const Dictionary* current, const Dictionary* english) {
    if (text.is_key) {
        for (const auto* dictionary : {current, english}) {
            if (!dictionary) continue;
            const auto entry = dictionary->find(text.value);
            if (entry != dictionary->end()) return entry->second;
        }
    }
    return text.value;
}

const Dictionary* dictionary(const Menu& menu, std::string_view locale) {
    const auto entry = menu.locales.find(locale);
    return entry == menu.locales.end() ? nullptr : &entry->second;
}

Json describe(const Row& row, std::string_view mod_id, const Dictionary* current, const Dictionary* english) {
    Json result{{"id", Store::wireId(mod_id, row.id)}, {"label", resolve(row.label, current, english)},
                {"description", resolve(row.description, current, english)}, {"kind", kindName(row.kind)},
                {"value", nullptr}};
    std::visit([&](const auto& value) {
        if constexpr (!std::is_same_v<std::decay_t<decltype(value)>, std::monostate>) result["value"] = value;
    }, row.value);
    if (row.kind == Kind::Number) {
        result["minimum"] = row.minimum;
        result["maximum"] = row.maximum;
        result["step"] = row.step;
    } else if (row.kind == Kind::Option) {
        Json choices = Json::array();
        for (const auto& [id, label] : row.choices)
            choices.push_back({{"id", id}, {"label", resolve(label, current, english)}});
        result["choices"] = std::move(choices);
    } else if (row.kind == Kind::Image) {
        const auto scale = std::min({1.0, 240.0 / row.image_width, 135.0 / row.image_height});
        result["width"] = std::max(1L, std::lround(row.image_width * scale));
        result["height"] = std::max(1L, std::lround(row.image_height * scale));
        result["image"] = row.image_url;
    } else if (row.kind == Kind::Key) {
        result["defaultKey"] = row.default_key;
    }
    return result;
}

Result write(Store& store, std::string_view input) {
    if (input.empty() || input.size() > 25'000 || input.size() % 2) return Result::Invalid;
    std::string bytes;
    bytes.reserve(input.size() / 2);
    for (std::size_t i = 0; i < input.size(); i += 2) {
        const auto a = digit(input[i]), b = digit(input[i + 1]);
        if (a < 0 || b < 0) return Result::Invalid;
        bytes.push_back(static_cast<char>((a << 4) | b));
    }
    const auto json = Json::parse(bytes, nullptr, false);
    if (!json.is_object() || json.size() != 3 || !json.contains("id") || !json["id"].is_string() ||
        !json.contains("kind") || !json["kind"].is_string() || !json.contains("value")) return Result::Invalid;
    const auto& id = json["id"].get_ref<const std::string&>();
    const auto& kind = json["kind"].get_ref<const std::string&>();
    const auto& value = json["value"];
    if (kind == "boolean" && value.is_boolean()) return store.dispatch(id, {Kind::Boolean, value.get<bool>()});
    if (kind == "number" && value.is_number()) return store.dispatch(id, {Kind::Number, 0, value.get<double>()});
    if (kind == "key" && value.is_number()) return store.dispatch(id, {Kind::Key, 0, value.get<double>()});
    if (kind == "action" && value.is_null()) return store.dispatch(id, {Kind::Action});
    if ((kind == "option" || kind == "string") && value.is_string()) {
        const auto& text = value.get_ref<const std::string&>();
        if (text.find('\0') != std::string::npos) return Result::Invalid;
        return store.dispatch(id, {kind == "option" ? Kind::Option : Kind::String, 0, 0, text.c_str()});
    }
    return Result::Invalid;
}
} // namespace

std::optional<std::string> settingsResource(Store& store, std::string_view endpoint) {
    if (endpoint.starts_with("menus/") && endpoint.ends_with(".json") && endpoint.size() > 11) {
        const auto args = endpoint.substr(6, endpoint.size() - 11);
        const auto slash = args.find('/');
        const auto language = slash == std::string_view::npos ? "en_US" : args.substr(slash + 1);
        if (!sequence(args.substr(0, slash)) || !validLocale(language)) return {};
        Json menus = Json::array();
        Json keyGroups = Json::array();
        for (const auto& menu : store.snapshot()) {
            const auto* current = dictionary(menu, language);
            const auto* english = dictionary(menu, "en_US");
            Json rows = Json::array();
            Json keys = Json::array();
            for (const auto& row : menu.rows) {
                auto& target = row.kind == Kind::Key ? keys : rows;
                target.push_back(describe(row, menu.id, current, english));
            }
            if (!keys.empty())
                keyGroups.push_back({{"id", std::string(menu_route_prefix) + menu.id},
                    {"title", resolve(menu.title, current, english)}, {"rows", std::move(keys)}});
            if (rows.empty()) continue;
            menus.push_back({{"id", std::string(menu_route_prefix) + menu.id},
                {"title", resolve(menu.title, current, english)},
                {"rows", std::move(rows)}});
            if (menu.mod_name) menus.back()["modName"] = resolve(*menu.mod_name, current, english);
        }
        const bool japanese = language == "ja_JP";
        const Dictionary keyTranslations = japanese ?
            Dictionary{{"keybind.title", "Mod キー設定"}, {"keybind.capture", "キーを変更"},
                       {"keybind.waiting", "キーを押してください（Esc で初期値に戻す）"},
                       {"keybind.space", "スペース"}} :
            Dictionary{{"keybind.title", "Mod KeyBind"}, {"keybind.capture", "Change key"},
                       {"keybind.waiting", "Press a key (Esc restores the default)"},
                       {"keybind.space", "Space"}};
        return Json{{"ok", true}, {"menus", std::move(menus)}, {"keyGroups", std::move(keyGroups)},
                     {"keyTranslations", keyTranslations},
                    {"menuLabel", japanese ? "Mod 設定" : "Mod Menu"},
                    {"actionLabel", japanese ? "実行" : "Run"}}.dump();
    }
    if (!endpoint.starts_with("write/") || !endpoint.ends_with(".json")) return {};
    endpoint.remove_prefix(6);
    endpoint.remove_suffix(5);
    const auto slash = endpoint.find('/');
    if (slash == std::string_view::npos || !sequence(endpoint.substr(0, slash))) return {};
    const auto result = write(store, endpoint.substr(slash + 1));
    return Json{{"ok", result == Result::Ok}, {"result", static_cast<std::uint32_t>(result)}}.dump();
}
} // namespace bedrock::detail::oreui_settings::impl
