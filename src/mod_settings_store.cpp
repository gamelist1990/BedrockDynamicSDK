#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

#include "bedrock/detail/mod_settings_store.hpp"
#include "bedrock/detail/mod_key_code.hpp"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <set>
#include <utility>

#include <nlohmann/json.hpp>

namespace bedrock::detail::oreui_settings::impl {
namespace {
using Json = nlohmann::json;
constexpr std::size_t max_file = 1024 * 1024;

bool validId(std::string_view id) {
    return !id.empty() && id.size() <= 64 && std::ranges::all_of(id, [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_';
    });
}

bool validText(const char* text, std::size_t limit) {
    if (!text || std::string_view(text).size() > limit) return false;
    try {
        (void)Json(text).dump();
        return true;
    } catch (const Json::exception&) {
        return false;
    }
}

bool validDisplay(Text text, std::size_t limit, bool optional = false) {
    if (!text.value) return optional && !text.is_key;
    return ((optional && !text.is_key) || *text.value) && validText(text.value, limit);
}

DisplayText copyDisplay(Text text) {
    return {text.value ? text.value : "", text.is_key};
}

bool hasChoice(const Row& row, std::string_view id) {
    return std::ranges::any_of(row.choices, [&](const auto& choice) { return choice.first == id; });
}

std::optional<StoredValue> convert(const Row& row, const RawValue& value) {
    if (value.kind != row.kind) return {};
    switch (row.kind) {
    case Kind::Boolean:
        if (value.boolean <= 1) return StoredValue(value.boolean != 0);
        break;
    case Kind::Number:
        if (std::isfinite(value.number) && value.number >= row.minimum && value.number <= row.maximum)
            return StoredValue(value.number);
        break;
    case Kind::Key:
        if (std::isfinite(value.number) && value.number >= 0 && value.number <= 123 &&
            value.number == std::trunc(value.number) && validModKey(static_cast<unsigned>(value.number)))
            return StoredValue(value.number);
        break;
    case Kind::Option:
        if (value.text && hasChoice(row, value.text)) return StoredValue(std::string(value.text));
        break;
    case Kind::String:
        if (validText(value.text, 2048)) return StoredValue(std::string(value.text));
        break;
    case Kind::Image:
    case Kind::Text:
    case Kind::Action:
        return StoredValue(std::monostate{});
    }
    return {};
}

std::optional<std::string> readFile(const std::filesystem::path& path, std::size_t limit) {
    std::ifstream stream(path, std::ios::binary | std::ios::ate);
    if (!stream) return {};
    const auto size = stream.tellg();
    if (size < 0 || size > static_cast<std::streamoff>(limit)) return {};
    std::string result(static_cast<std::size_t>(size), '\0');
    stream.seekg(0);
    if (!stream.read(result.data(), static_cast<std::streamsize>(result.size()))) return {};
    return result;
}

std::string pngUrl(const ControlDescriptor& control, std::uint32_t& image_width, std::uint32_t& image_height) {
    std::optional<std::string> file_bytes;
    std::string_view bytes;
    if (control.image_png || control.image_png_size) {
        if (control.image_path || !control.image_png || !control.image_png_size || control.image_png_size > max_file)
            return {};
        bytes = {reinterpret_cast<const char*>(control.image_png), control.image_png_size};
    } else {
        if (!validText(control.image_path, 4096)) return {};
        const auto file = std::filesystem::path(reinterpret_cast<const char8_t*>(control.image_path));
        std::error_code error;
        if (!file.is_absolute() || !std::filesystem::is_regular_file(file, error)) return {};
        file_bytes = readFile(file, max_file);
        if (!file_bytes) return {};
        bytes = *file_bytes;
    }
    if (bytes.size() < 33 || bytes.compare(0, 16, "\x89PNG\r\n\x1a\n\0\0\0\x0dIHDR", 16) != 0) return {};
    const auto uint32At = [&](std::size_t offset) {
        std::uint32_t result = 0;
        for (std::size_t i = offset; i < offset + 4; ++i)
            result = (result << 8) | static_cast<unsigned char>(bytes[i]);
        return result;
    };
    const auto width = uint32At(16), height = uint32At(20);
    if (!width || !height || width > 2048 || height > 2048) return {};
    image_width = width;
    image_height = height;
    constexpr std::string_view alphabet =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string url = "data:image/png;base64,";
    url.reserve(url.size() + (bytes.size() + 2) / 3 * 4);
    for (std::size_t i = 0; i < bytes.size(); i += 3) {
        const auto a = static_cast<unsigned char>(bytes[i]);
        const auto b = i + 1 < bytes.size() ? static_cast<unsigned char>(bytes[i + 1]) : 0;
        const auto c = i + 2 < bytes.size() ? static_cast<unsigned char>(bytes[i + 2]) : 0;
        url += alphabet[a >> 2];
        url += alphabet[(a & 3) << 4 | b >> 4];
        url += i + 1 < bytes.size() ? alphabet[(b & 15) << 2 | c >> 6] : '=';
        url += i + 2 < bytes.size() ? alphabet[c & 63] : '=';
    }
    return url;
}

bool replaceFile(const std::filesystem::path& from, const std::filesystem::path& to) {
#ifdef _WIN32
    return MoveFileExW(from.c_str(), to.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
#else
    std::error_code error;
    std::filesystem::rename(from, to, error);
    return !error;
#endif
}
} // namespace

bool validLocale(std::string_view id) {
    const auto letter = [](char c) { return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'); };
    return id.size() == 5 && id[2] == '_' && letter(id[0]) && letter(id[1]) &&
        letter(id[3]) && letter(id[4]);
}

Result Store::add(const PageDescriptor& page) {
    if (!page.mod_id || !validId(page.mod_id) || !validDisplay(page.title, 256) ||
        !validDisplay(page.mod_name, 256, true) ||
        !page.controls || page.control_count == 0 || page.control_count > 64) return Result::Invalid;
    Entry entry{{page.mod_id, copyDisplay(page.title), {}, {}, {}}, page.on_change, page.context};
    if (page.mod_name.value) entry.menu.mod_name = copyDisplay(page.mod_name);
    if (page.locale_count > 64 || (page.locale_count && !page.locales)) return Result::Invalid;
    for (std::uint32_t i = 0; i < page.locale_count; ++i) {
        const auto& dict = page.locales[i];
        if (!dict.id || !validLocale(dict.id) || dict.count > 1024 || (dict.count && !dict.entries))
            return Result::Invalid;
        auto [it, inserted] = entry.menu.locales.try_emplace(dict.id);
        if (!inserted) return Result::Invalid;
        for (std::uint32_t j = 0; j < dict.count; ++j) {
            const auto& translation = dict.entries[j];
            if (!validText(translation.key, 1024) || !*translation.key ||
                !validText(translation.value, 1024) ||
                !it->second.try_emplace(translation.key, translation.value).second) return Result::Invalid;
        }
    }
    std::set<std::string_view> ids;
    std::size_t image_bytes = 0;
    for (std::uint32_t i = 0; i < page.control_count; ++i) {
        const auto& control = page.controls[i];
        if (!control.id || !validId(control.id) || !ids.emplace(control.id).second ||
            !validDisplay(control.label, 256) || !validDisplay(control.description, 1024, true))
            return Result::Invalid;
        Row row{control.id, copyDisplay(control.label), copyDisplay(control.description), control.kind,
                {}, 0, 0, 0, {}, {}};
        switch (row.kind) {
        case Kind::Number:
            row.minimum = control.minimum;
            row.maximum = control.maximum;
            row.step = control.step;
            if (!std::isfinite(row.minimum) || !std::isfinite(row.maximum) || !std::isfinite(row.step) ||
                row.minimum >= row.maximum || row.step <= 0 || row.step > row.maximum - row.minimum ||
                !std::isfinite(row.maximum - row.minimum)) return Result::Invalid;
            break;
        case Kind::Option: {
            if (!control.choices || control.choice_count < 2 || control.choice_count > 64) return Result::Invalid;
            std::set<std::string_view> choices;
            for (std::uint32_t n = 0; n < control.choice_count; ++n) {
                const auto& choice = control.choices[n];
                if (!choice.id || !validId(choice.id) || !choices.emplace(choice.id).second ||
                    !validDisplay(choice.label, 256)) return Result::Invalid;
                row.choices.emplace_back(choice.id, copyDisplay(choice.label));
            }
            break;
        }
        case Kind::Image:
            row.image_url = pngUrl(control, row.image_width, row.image_height);
            image_bytes += row.image_url.size();
            if (row.image_url.empty() || image_bytes > 2 * max_file) return Result::Invalid;
            break;
        default:
            break;
        }
        if (row.kind == Kind::Boolean || row.kind == Kind::Number || row.kind == Kind::Key ||
            row.kind == Kind::Option || row.kind == Kind::String) {
            auto initial = convert(row, control.initial);
            if (!initial) return Result::Invalid;
            row.value = std::move(*initial);
            if (row.kind == Kind::Key) row.default_key = static_cast<unsigned>(control.initial.number);
        } else if (row.kind != Kind::Image && row.kind != Kind::Text && row.kind != Kind::Action) {
            return Result::Invalid;
        }
        entry.menu.rows.push_back(std::move(row));
    }
    std::lock_guard lock(mutex_);
    auto key = entry.menu.id;
    if (pages_.contains(key)) return Result::AlreadyRegistered;
    if (pages_.size() >= 16) return Result::Unavailable;
    if (!load(entry.menu)) return Result::StorageError;
    pages_.emplace(std::move(key), std::move(entry));
    return Result::Ok;
}

Result Store::remove(std::string_view mod_id) {
    std::lock_guard lock(mutex_);
    const auto it = pages_.find(mod_id);
    if (it == pages_.end()) return Result::NotFound;
    pages_.erase(it);
    return Result::Ok;
}

std::vector<Menu> Store::snapshot() const {
    std::lock_guard lock(mutex_);
    std::vector<Menu> result;
    result.reserve(pages_.size());
    for (const auto& [id, entry] : pages_) result.push_back(entry.menu);
    return result;
}

Result Store::read(std::string_view mod_id, std::string_view control_id, RawValue& out,
                   char* text_buffer, std::size_t capacity) const {
    std::lock_guard lock(mutex_);
    const auto page = pages_.find(mod_id);
    if (page == pages_.end()) return Result::NotFound;
    const auto& rows = page->second.menu.rows;
    const auto row = std::ranges::find(rows, control_id, &Row::id);
    if (row == rows.end()) return Result::NotFound;
    RawValue result{row->kind};
    if (const auto* value = std::get_if<bool>(&row->value)) result.boolean = *value;
    else if (const auto* number = std::get_if<double>(&row->value)) result.number = *number;
    else if (const auto* text = std::get_if<std::string>(&row->value)) {
        if (!text_buffer || capacity <= text->size()) return Result::BufferTooSmall;
        std::ranges::copy(*text, text_buffer);
        text_buffer[text->size()] = '\0';
        result.text = text_buffer;
    } else return Result::WrongType;
    out = result;
    return Result::Ok;
}

std::string Store::wireId(std::string_view mod_id, std::string_view control_id) {
    return std::string(prefix) + std::string(mod_id) + "." + std::string(control_id);
}

Result Store::dispatch(std::string_view id, const RawValue& value, bool commit) {
    if (!id.starts_with(prefix)) return Result::NotFound;
    id.remove_prefix(prefix.size());
    const auto separator = id.find('.');
    if (separator == std::string_view::npos) return Result::NotFound;
    std::lock_guard lock(mutex_);
    const auto entry = pages_.find(id.substr(0, separator));
    if (entry == pages_.end()) return Result::NotFound;
    auto& page = entry->second;
    const auto row = std::ranges::find(page.menu.rows, id.substr(separator + 1), &Row::id);
    if (row == page.menu.rows.end()) return Result::NotFound;
    if (row->kind != value.kind || row->kind == Kind::Image || row->kind == Kind::Text) return Result::WrongType;
    auto next = convert(*row, value);
    if (!next) return Result::Invalid;
    if (!commit) return Result::Ok;
    if (row->kind != Kind::Action) {
        if (row->value == *next) return Result::Ok;
        auto previous = std::exchange(row->value, std::move(*next));
        try {
            if (!save(page.menu)) {
                row->value = std::move(previous);
                return Result::StorageError;
            }
        } catch (...) {
            row->value = std::move(previous);
            return Result::StorageError;
        }
    }
    if (page.callback) {
        char control_id[65]{};
        std::ranges::copy(row->id, control_id);
        page.callback(page.context, control_id, &value);
    }
    return Result::Ok;
}

bool Store::save(const Menu& menu) const {
    Json values = Json::object();
    for (const auto& row : menu.rows) {
        std::visit([&](const auto& value) {
            if constexpr (!std::is_same_v<std::decay_t<decltype(value)>, std::monostate>)
                values[row.id] = value;
        }, row.value);
    }
    const auto data = Json{{"version", 1}, {"values", std::move(values)}}.dump();
    std::error_code error;
    std::filesystem::create_directories(root_, error);
    if (error) return false;
    const auto file = root_ / ("mod-" + menu.id + ".json");
    auto temp = file;
    temp += ".tmp";
    std::ofstream stream(temp, std::ios::binary | std::ios::trunc);
    stream.write(data.data(), static_cast<std::streamsize>(data.size()));
    stream.close();
    if (stream && replaceFile(temp, file)) return true;
    std::filesystem::remove(temp, error);
    return false;
}

bool Store::load(Menu& menu) const {
    const auto file = root_ / ("mod-" + menu.id + ".json");
    std::error_code error;
    if (std::filesystem::exists(root_, error) && !std::filesystem::is_directory(root_, error)) return false;
    if (error) return false;
    if (!std::filesystem::exists(file, error)) return !error;
    const auto data = readFile(file, max_file);
    if (!data) return false;
    const auto json = Json::parse(*data, nullptr, false);
    if (!json.is_object() || !json.contains("version") || json["version"] != 1 ||
        !json.contains("values") || !json["values"].is_object()) return false;
    for (auto& row : menu.rows) {
        const auto& values = json["values"];
        const auto it = values.find(row.id);
        if (it == values.end()) continue;
        RawValue value{row.kind, 0, 0, nullptr};
        if (row.kind == Kind::Boolean && it->is_boolean()) value.boolean = it->get<bool>();
        else if ((row.kind == Kind::Number || row.kind == Kind::Key) && it->is_number()) value.number = it->get<double>();
        else if ((row.kind == Kind::String || row.kind == Kind::Option) && it->is_string()) {
            const auto& text = it->get_ref<const std::string&>();
            if (text.find('\0') != std::string::npos) continue;
            value.text = text.c_str();
        } else continue;
        if (auto converted = convert(row, value)) row.value = std::move(*converted);
    }
    return true;
}

} // namespace bedrock::detail::oreui_settings::impl
