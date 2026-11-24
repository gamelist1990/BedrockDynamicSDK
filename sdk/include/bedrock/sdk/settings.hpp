#pragma once

#include "result.hpp"
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace bedrock::sdk {

class Profile;

using SettingValue = std::variant<bool, double, std::string>;
using SettingChanged = std::function<void(std::string_view, const SettingValue&)>;

struct Translation { std::string key; std::string value; };
struct Locale { std::string id; std::vector<Translation> entries; };

class Setting {
public:
    enum class Kind { Toggle, Slider, Choice, Text, Key, Action, SessionToggle };
    static Setting toggle(std::string id, std::string labelKey, bool initial);
    // A toggle whose durable state is managed by the caller, not Mod Settings.
    static Setting sessionToggle(std::string id, std::string labelKey, bool initial);
    static Setting action(std::string id, std::string labelKey, std::string descriptionKey = {});
    static Setting slider(std::string id, std::string labelKey, double initial,
                          double minimum, double maximum, double step);
    static Setting choice(std::string id, std::string labelKey, std::string initial,
                          std::vector<std::pair<std::string, std::string>> choices);
    static Setting text(std::string id, std::string labelKey, std::string descriptionKey = {});

    std::string id;
    std::string labelKey;
    std::string descriptionKey;
    Kind kind{Kind::Text};
    SettingValue initial{false};
    double minimum{}, maximum{}, step{};
    std::vector<std::pair<std::string, std::string>> choices;
};

class SettingsPage {
public:
    SettingsPage(std::string modId, std::string titleKey);
    SettingsPage& add(Setting setting);
    SettingsPage& locale(std::string id, std::vector<Translation> entries);
    SettingsPage& onChange(SettingChanged callback);
    SettingsPage& modName(std::string key);

    [[nodiscard]] Result<void> registerPage() const noexcept;
    // Reuse exact-build validation taken before other SDK hooks patched .text.
    // The settings backend still checks its own live entry-point guards.
    [[nodiscard]] Result<void> registerPage(const Profile& profile) const noexcept;
    [[nodiscard]] Result<void> unregisterPage() const noexcept;
    [[nodiscard]] Result<SettingValue> value(std::string_view settingId) const noexcept;
    [[nodiscard]] const std::string& id() const noexcept { return modId_; }

private:
    Result<void> registerWith(const Profile* profile) const noexcept;
    std::string modId_, titleKey_, modNameKey_;
    std::vector<Setting> settings_;
    std::vector<Locale> locales_;
    SettingChanged callback_;
};

} // namespace bedrock::sdk
