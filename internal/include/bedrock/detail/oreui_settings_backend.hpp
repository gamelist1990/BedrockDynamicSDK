#pragma once

#include <cstdint>
#include <type_traits>

namespace bedrock::detail::oreui_settings {

// Strings are NUL-terminated UTF-8 and descriptor data is copied at registration.
enum class Kind : std::uint32_t { Boolean, Number, Option, String, Action, Text, Image, Key, SessionBoolean };

struct Text {
    const char* value{};
    bool is_key{};
    constexpr Text(const char* literal = nullptr) noexcept : value(literal) {}
};

constexpr Text tr(const char* key) noexcept {
    Text result{key};
    result.is_key = true;
    return result;
}

struct Translation {
    const char* key;
    const char* value;
};

struct Locale {
    const char* id; // Minecraft locale, e.g. "ja_JP"
    const Translation* entries;
    std::uint32_t count;
};

template<std::uint32_t N>
constexpr Locale locale(const char* id, const Translation (&entries)[N]) noexcept {
    return {id, entries, N};
}

struct RawValue {
    Kind kind{};
    std::uint32_t boolean{};
    double number{};
    // String value, or the stable choice key for Option (not the label/index).
    const char* text{};
};

struct Choice {
    const char* id;
    Text label;
};

struct ControlDescriptor {
    const char* id;
    Text label;
    Text description{};
    Kind kind;
    RawValue initial{};
    double minimum{};
    double maximum{};
    double step{};
    const Choice* choices{};
    std::uint32_t choice_count{};
    // Image only: absolute PNG path, read and copied during registration.
    const char* image_path{};
    // Image only: alternatively, pass a PNG buffer copied during registration.
    const unsigned char* image_png{};
    std::uint32_t image_png_size{};
};

// Common controls without the low-level descriptor boilerplate.
constexpr ControlDescriptor toggle(const char* id, Text label, bool initial,
                          Text description = {}) noexcept {
    return {.id = id, .label = label, .description = description, .kind = Kind::Boolean,
            .initial = {Kind::Boolean, initial ? 1u : 0u}};
}

constexpr ControlDescriptor slider(const char* id, Text label, double initial,
                         double minimum, double maximum, double step,
                          Text description = {}) noexcept {
    return {.id = id, .label = label, .description = description, .kind = Kind::Number,
            .initial = {.kind = Kind::Number, .number = initial},
            .minimum = minimum, .maximum = maximum, .step = step};
}

constexpr ControlDescriptor text(const char* id, Text label, Text description) noexcept {
    return {.id = id, .label = label, .description = description, .kind = Kind::Text};
}

using OnChange = void (*)(void* context, const char* control_id, const RawValue* value) noexcept;

struct PageDescriptor {
    const char* mod_id;
    Text title;
    const ControlDescriptor* controls;
    std::uint32_t control_count;
    OnChange on_change;
    void* context;
    const Locale* locales{};
    std::uint32_t locale_count{};
    Text mod_name{}; // Optional sidebar name; defaults to the page title.

    template<std::uint32_t N>
    constexpr PageDescriptor& translations(const Locale (&dictionaries)[N]) noexcept {
        locales = dictionaries;
        locale_count = N;
        return *this;
    }
};

template<std::uint32_t N>
constexpr PageDescriptor makePage(const char* id, Text title, const ControlDescriptor (&controls)[N],
                        OnChange on_change = nullptr, void* context = nullptr) noexcept {
    return {id, title, controls, N, on_change, context};
}

enum class Result : std::uint32_t {
    Ok, Invalid, AlreadyRegistered, NotFound, WrongType, StorageError, Unavailable, BufferTooSmall
};

static_assert(std::is_standard_layout_v<PageDescriptor> && std::is_trivially_copyable_v<PageDescriptor>);
static_assert(std::is_standard_layout_v<ControlDescriptor> && std::is_trivially_copyable_v<ControlDescriptor>);

// Private OreUI transport, shared by the SDK and its backend diagnostic.
// Register/unregister outside DllMain. The callback owner must unregister before
// unloading. Callbacks run synchronously on the OreUI resource request thread
// after a successful committed write or action. Do not call game-thread-only
// APIs or re-enter Mod Settings here. Input pointers are valid only until
// the callback returns.
Result registerPage(const PageDescriptor& page) noexcept;
Result unregisterPage(const char* mod_id) noexcept;

// Only stored value controls are readable. For String and Option, pass a
// buffer for a NUL-terminated UTF-8 copy; on success out.text points into it.
Result readValue(const char* mod_id, const char* control_id, RawValue& out,
                 char* text_buffer = nullptr, std::uint32_t capacity = 0) noexcept;

} // namespace bedrock::detail::oreui_settings
