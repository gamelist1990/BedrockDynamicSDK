#pragma once

#include "bedrock/detail/oreui_settings_backend.hpp"

#include <chrono>
#include <filesystem>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace bedrock::detail::oreui_settings::impl {

using StoredValue = std::variant<std::monostate, bool, double, std::string>;

struct DisplayText {
    std::string value;
    bool is_key;
};

using Dictionary = std::map<std::string, std::string, std::less<>>;

struct Row {
    std::string id;
    DisplayText label, description;
    Kind kind;
    StoredValue value;
    double minimum, maximum, step;
    std::vector<std::pair<std::string, DisplayText>> choices;
    std::string image_url;
    std::uint32_t image_width{}, image_height{};
    unsigned default_key{};
};

struct Menu {
    std::string id;
    DisplayText title;
    std::optional<DisplayText> mod_name;
    std::vector<Row> rows;
    std::map<std::string, Dictionary, std::less<>> locales;
};

bool validLocale(std::string_view id);

class Store {
public:
    explicit Store(std::filesystem::path root) : root_(std::move(root)) {}
    Result add(const PageDescriptor& page);
    Result remove(std::string_view mod_id);
    Result read(std::string_view mod_id, std::string_view control_id, RawValue& out,
                char* text_buffer, std::size_t capacity) const;
    std::vector<Menu> snapshot() const;
    void noteVisibility(std::string_view mod_id, bool visible);
    Result noteDropZone(std::string_view mod_id, DropZoneRect rect);
    Result dispatch(std::string_view wire_id, const RawValue& value, bool commit = true);

    static constexpr std::string_view prefix = "bedrockdynamic.mod.";
    static std::string wireId(std::string_view mod_id, std::string_view control_id);

private:
    struct Entry {
        Menu menu;
        OnChange callback;
        void* context;
        std::chrono::steady_clock::time_point visibleUntil{};
        DropZoneRect zone{};
        std::chrono::steady_clock::time_point zoneUntil{};
    };
    bool save(const Menu& menu) const;
    bool load(Menu& menu) const;

    std::filesystem::path root_;
    // Serialize callbacks with unregister. Recursive to allow the owner to
    // inspect/register settings from a callback; dispatch retains no references
    // into the registry after calling it.
    mutable std::recursive_mutex mutex_;
    std::map<std::string, Entry, std::less<>> pages_;
};

} // namespace bedrock::detail::oreui_settings::impl
