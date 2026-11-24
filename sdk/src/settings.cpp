#include "bedrock/sdk/settings.hpp"
#include "bedrock/sdk/profiles.hpp"
#include <bedrock/detail/oreui_settings_backend.hpp>
#include <cstring>
#include <deque>
#include <mutex>
#include <unordered_map>

namespace bedrock::sdk {
namespace {
namespace Backend = bedrock::detail::oreui_settings;

struct Registered {
    // The OreUI descriptors borrow c_str() addresses while the page is built.
    std::deque<std::string> strings;
    std::vector<Backend::Choice> choices;
    std::vector<Backend::ControlDescriptor> controls;
    std::vector<Backend::Translation> translations;
    std::vector<Backend::Locale> locales;
    Backend::PageDescriptor page{};
    SettingChanged callback;
};

std::mutex registryMutex;
std::unordered_map<std::string, std::shared_ptr<Registered>> registry;

const char* keep(Registered& state, std::string value) {
    state.strings.push_back(std::move(value));
    return state.strings.back().c_str();
}

Backend::Text text(Registered& state, const std::string& key) {
    return Backend::tr(keep(state, key));
}

Error mapError(Backend::Result result) {
    switch (result) {
    case Backend::Result::AlreadyRegistered: return {ErrorCode::AlreadyRegistered, "settings page already registered"};
    case Backend::Result::NotFound: return {ErrorCode::NotFound, "settings page or value not found"};
    case Backend::Result::WrongType: return {ErrorCode::WrongType, "settings value has the wrong type"};
    case Backend::Result::StorageError: return {ErrorCode::Storage, "settings file could not be loaded or saved"};
    case Backend::Result::Unavailable: return {ErrorCode::Unavailable, "Mod Settings runtime unavailable"};
    case Backend::Result::BufferTooSmall: return {ErrorCode::BufferTooSmall, "settings value exceeds the read buffer"};
    default: return {ErrorCode::Invalid, "invalid settings definition or value"};
    }
}

void onBackendChange(void* context, const char* id, const Backend::RawValue* value) noexcept {
    auto* registered = static_cast<Registered*>(context);
    try {
        SettingValue converted;
        if (value->kind == Backend::Kind::Boolean || value->kind == Backend::Kind::SessionBoolean)
            converted = value->boolean != 0;
        else if (value->kind == Backend::Kind::Number || value->kind == Backend::Kind::Key) converted = value->number;
        else converted = std::string(value->text ? value->text : "");
        if (registered->callback) registered->callback(id ? id : "", converted);
    } catch (...) { /* C callback boundary: never unwind into OreUI */ }
}
} // namespace

Setting Setting::toggle(std::string id, std::string labelKey, bool initial) {
    return {std::move(id), std::move(labelKey), {}, Kind::Toggle, initial};
}
Setting Setting::sessionToggle(std::string id, std::string labelKey, bool initial) {
    return {std::move(id), std::move(labelKey), {}, Kind::SessionToggle, initial};
}
Setting Setting::action(std::string id, std::string labelKey, std::string descriptionKey) {
    return {std::move(id), std::move(labelKey), std::move(descriptionKey), Kind::Action, false};
}
Setting Setting::slider(std::string id, std::string labelKey, double initial,
                        double minimum, double maximum, double step) {
    Setting result{std::move(id), std::move(labelKey), {}, Kind::Slider, initial};
    result.minimum = minimum; result.maximum = maximum; result.step = step; return result;
}
Setting Setting::choice(std::string id, std::string labelKey, std::string initial,
                        std::vector<std::pair<std::string, std::string>> choices) {
    Setting result{std::move(id), std::move(labelKey), {}, Kind::Choice, std::move(initial)};
    result.choices = std::move(choices); return result;
}
Setting Setting::text(std::string id, std::string labelKey, std::string descriptionKey) {
    return {std::move(id), std::move(labelKey), std::move(descriptionKey), Kind::Text, false};
}

SettingsPage::SettingsPage(std::string modId, std::string titleKey)
    : modId_(std::move(modId)), titleKey_(std::move(titleKey)) {}
SettingsPage& SettingsPage::add(Setting setting) { settings_.push_back(std::move(setting)); return *this; }
SettingsPage& SettingsPage::locale(std::string id, std::vector<Translation> entries) {
    locales_.push_back({std::move(id), std::move(entries)}); return *this;
}
SettingsPage& SettingsPage::onChange(SettingChanged callback) { callback_ = std::move(callback); return *this; }
SettingsPage& SettingsPage::modName(std::string key) { modNameKey_ = std::move(key); return *this; }

Result<void> SettingsPage::registerPage() const noexcept {
    return registerWith(nullptr);
}

Result<void> SettingsPage::registerPage(const Profile& profile) const noexcept {
    return registerWith(&profile);
}

Result<void> SettingsPage::registerWith(const Profile* profile) const noexcept {
    try {
        auto state = std::make_shared<Registered>();
        state->controls.reserve(settings_.size());
        std::size_t choiceCount = 0, translationCount = 0;
        for (const auto& setting : settings_) choiceCount += setting.choices.size();
        for (const auto& locale : locales_) translationCount += locale.entries.size();
        state->choices.reserve(choiceCount);
        state->translations.reserve(translationCount);
        state->locales.reserve(locales_.size());
        state->callback = callback_;
        for (const auto& setting : settings_) {
            Backend::ControlDescriptor control{};
            control.id = keep(*state, setting.id);
            control.label = text(*state, setting.labelKey);
            control.description = setting.descriptionKey.empty() ? Backend::Text{} : text(*state, setting.descriptionKey);
            switch (setting.kind) {
            case Setting::Kind::Toggle: control.kind = Backend::Kind::Boolean; control.initial = {control.kind, std::get<bool>(setting.initial) ? 1u : 0u}; break;
            case Setting::Kind::SessionToggle: control.kind = Backend::Kind::SessionBoolean; control.initial = {control.kind, std::get<bool>(setting.initial) ? 1u : 0u}; break;
            case Setting::Kind::Action: control.kind = Backend::Kind::Action; break;
            case Setting::Kind::Slider: control.kind = Backend::Kind::Number; control.initial = {control.kind, 0, std::get<double>(setting.initial)}; control.minimum = setting.minimum; control.maximum = setting.maximum; control.step = setting.step; break;
            case Setting::Kind::Choice:
                control.kind = Backend::Kind::Option; control.initial = {control.kind, 0, 0, keep(*state, std::get<std::string>(setting.initial))};
                for (const auto& [id, label] : setting.choices) state->choices.push_back({keep(*state, id), text(*state, label)});
                control.choices = state->choices.data() + (state->choices.size() - setting.choices.size()); control.choice_count = static_cast<std::uint32_t>(setting.choices.size()); break;
            case Setting::Kind::Text: control.kind = Backend::Kind::Text; break;
            case Setting::Kind::Key:
                control.kind = Backend::Kind::Key;
                control.initial = {control.kind, 0, std::get<double>(setting.initial)};
                break;
            }
            state->controls.push_back(control);
        }
        for (const auto& locale : locales_) {
            const auto start = state->translations.size();
            for (const auto& entry : locale.entries) state->translations.push_back({keep(*state, entry.key), keep(*state, entry.value)});
            state->locales.push_back({keep(*state, locale.id), state->translations.data() + start,
                                      static_cast<std::uint32_t>(state->translations.size() - start)});
        }
        state->page.mod_id = keep(*state, modId_);
        state->page.title = text(*state, titleKey_);
        state->page.controls = state->controls.data();
        state->page.control_count = static_cast<std::uint32_t>(state->controls.size());
        state->page.on_change = onBackendChange;
        state->page.context = state.get();
        state->page.locales = state->locales.data();
        state->page.locale_count = static_cast<std::uint32_t>(state->locales.size());
        state->page.mod_name = modNameKey_.empty() ? Backend::Text{} : text(*state, modNameKey_);
        const auto snapshot = profile ? ProfileRuntimeAccess::runtime(*profile) : nullptr;
        const auto result = Backend::registerPage(state->page, snapshot.get());
        if (result != Backend::Result::Ok) {
            auto error = mapError(result);
            return Result<void>::error(error.code, std::move(error.message));
        }
        std::lock_guard lock(registryMutex);
        registry[modId_] = std::move(state);
        return Result<void>::ok();
    } catch (const std::exception& e) { return Result<void>::error(ErrorCode::Internal, e.what()); }
      catch (...) { return Result<void>::error(ErrorCode::Internal, "settings registration failed"); }
}

Result<void> SettingsPage::unregisterPage() const noexcept {
    const auto result = Backend::unregisterPage(modId_.c_str());
    if (result != Backend::Result::Ok) {
        auto error = mapError(result);
        return Result<void>::error(error.code, std::move(error.message));
    }
    std::lock_guard lock(registryMutex); registry.erase(modId_); return Result<void>::ok();
}

Result<SettingValue> SettingsPage::value(std::string_view settingId) const noexcept {
    try {
        Backend::RawValue raw{}; char text[512]{};
        std::string id(settingId);
        const auto result = Backend::readValue(modId_.c_str(), id.c_str(), raw, text, sizeof text);
        if (result != Backend::Result::Ok) {
            auto error = mapError(result);
            return Result<SettingValue>::error(error.code, std::move(error.message));
        }
        if (raw.kind == Backend::Kind::Boolean || raw.kind == Backend::Kind::SessionBoolean)
            return Result<SettingValue>::ok(raw.boolean != 0);
        if (raw.kind == Backend::Kind::Number || raw.kind == Backend::Kind::Key)
            return Result<SettingValue>::ok(raw.number);
        return Result<SettingValue>::ok(std::string(raw.text ? raw.text : text));
    } catch (const std::exception& e) { return Result<SettingValue>::error(ErrorCode::Internal, e.what()); }
      catch (...) { return Result<SettingValue>::error(ErrorCode::Internal, "settings read failed"); }
}
} // namespace bedrock::sdk
