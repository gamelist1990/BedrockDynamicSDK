#include <bedrock/sdk/keybinds.hpp>
#include <bedrock/detail/mod_key_code.hpp>

#include <map>
#include <mutex>

namespace bedrock::sdk {

struct KeyBindings::State {
    struct Binding {
        Key key;
        bool pressed{};
        bool changed{};
    };
    std::mutex mutex;
    std::map<std::string, Binding, std::less<>> bindings;
};

KeyBindings::KeyBindings(std::string modId)
    : state_(std::make_shared<State>()), page_(modId + "-keybinds", modId) {}

KeyBindings::~KeyBindings() {
    if (connected_) (void)disconnect();
}

Result<void> KeyBindings::add(std::string id, std::string labelKey, Key defaultKey) {
    if (connected_) return Result<void>::error(ErrorCode::Invalid, "declare keybinds before enabling");
    if (!bedrock::detail::oreui_settings::impl::validModKey(static_cast<unsigned>(defaultKey)) ||
        id.empty() || labelKey.empty())
        return Result<void>::error(ErrorCode::Invalid, "invalid keybind definition");
    std::lock_guard lock(state_->mutex);
    if (state_->bindings.contains(id))
        return Result<void>::error(ErrorCode::AlreadyRegistered, "duplicate keybind action");
    page_.add({id, std::move(labelKey), {}, Setting::Kind::Key,
               static_cast<double>(static_cast<unsigned>(defaultKey))});
    state_->bindings.emplace(std::move(id), State::Binding{defaultKey});
    return Result<void>::ok();
}

void KeyBindings::locale(std::string id, std::vector<Translation> entries) {
    if (!connected_) page_.locale(std::move(id), std::move(entries));
}

std::optional<Key> KeyBindings::key(std::string_view id) const {
    std::lock_guard lock(state_->mutex);
    const auto binding = state_->bindings.find(id);
    if (binding == state_->bindings.end()) return {};
    return binding->second.key;
}

Result<void> KeyBindings::connect() {
    if (state_->bindings.empty() || connected_) return Result<void>::ok();
    page_.onChange([weak = std::weak_ptr<State>(state_)](std::string_view id, const SettingValue& value) {
        if (const auto state = weak.lock()) {
            std::lock_guard lock(state->mutex);
            const auto binding = state->bindings.find(id);
            if (binding == state->bindings.end()) return;
            binding->second = {static_cast<Key>(static_cast<unsigned>(std::get<double>(value))), false, true};
        }
    });
    auto result = page_.registerPage();
    if (!result) return result;
    connected_ = true;
    for (auto& [id, binding] : state_->bindings) {
        const auto value = page_.value(id);
        if (!value) {
            (void)disconnect();
            return Result<void>::error(value.error().code, value.error().message);
        }
        std::lock_guard lock(state_->mutex);
        if (!binding.changed)
            binding.key = static_cast<Key>(static_cast<unsigned>(std::get<double>(value.value())));
    }
    return Result<void>::ok();
}

Result<void> KeyBindings::disconnect() {
    if (!connected_) return Result<void>::ok();
    auto result = page_.unregisterPage();
    {
        std::lock_guard lock(state_->mutex);
        state_->bindings.clear();
    }
    if (result) connected_ = false;
    clearInput();
    return result;
}

std::vector<std::string> KeyBindings::dispatch(Key physicalKey, bool pressed) {
    std::vector<std::string> matches;
    {
        std::lock_guard lock(state_->mutex);
        for (auto& [id, binding] : state_->bindings) {
            if (binding.key != physicalKey || binding.pressed == pressed) continue;
            binding.pressed = pressed;
            matches.push_back(id);
        }
    }
    for (const auto& id : matches) changed.publish({id, pressed});
    return matches;
}

void KeyBindings::clearInput() {
    std::lock_guard lock(state_->mutex);
    for (auto& [id, binding] : state_->bindings) binding.pressed = false;
}

} // namespace bedrock::sdk
