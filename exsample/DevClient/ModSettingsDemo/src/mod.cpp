#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <bedrock/detail/oreui_settings_backend.hpp>
#include <bedrock/logging/logger.hpp>
#include <filesystem>
#include <string>
#include "mod_settings_demo_preview.hpp"

namespace {
using namespace bedrock::detail::oreui_settings;
constexpr auto mod_id = "settings-demo";
constexpr Translation english[] = {
    {"title", "Settings Demo"}, {"enable", "Enable demo"}, {"safe", "Safe"},
};
constexpr Translation japanese[] = {
    {"title", "設定デモ"}, {"enable", "デモを有効にする"}, {"safe", "安全"},
};
constexpr Locale dictionaries[] = {locale("en_US", english), locale("ja_JP", japanese)};
bedrock::Logger logger("ModSettingsDemo");
bedrock::FileLogSink log_file;

void changed(void*, const char* id, const RawValue* value) noexcept {
    try {
        std::string message;
        switch (value->kind) {
        case Kind::Boolean: message += value->boolean ? "true" : "false"; break;
        case Kind::Number: message += std::to_string(value->number); break;
        case Kind::Option:
        case Kind::String: message += value->text; break;
        case Kind::Action: message += "invoked"; break;
        default: return;
        }
        logger.info(id, " -> ", message);
    } catch (...) {}
}

DWORD WINAPI start(void* parameter) {
    // Keep this sample resident until process exit, including its callbacks.
    HMODULE pin{};
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                            reinterpret_cast<LPCWSTR>(&start), &pin)) return 1;
    try {
        wchar_t file[32768]{};
        const auto len = GetModuleFileNameW(static_cast<HMODULE>(parameter), file, ARRAYSIZE(file));
        if (!len || len == ARRAYSIZE(file)) return 2;
        const auto directory = std::filesystem::path(file).parent_path();
        if (log_file.open(directory / "ModSettingsDemo.log")) log_file.attach(logger);
        constexpr Choice choices[] = {{"safe", tr("safe")}, {"fast", "Fast"}, {"precise", "Precise"}};
        const ControlDescriptor controls[] = {
            {.id="enabled", .label=tr("enable"), .description="Boolean toggle", .kind=Kind::Boolean,
             .initial={Kind::Boolean, 1, 0, nullptr}},
            {.id="intensity", .label="Intensity", .description="0 to 100", .kind=Kind::Number,
             .initial={Kind::Number, 0, 50, nullptr}, .minimum=0, .maximum=100, .step=5},
            {.id="mode", .label="Mode", .kind=Kind::Option, .initial={Kind::Option, 0, 0, "safe"},
             .choices=choices, .choice_count=3},
            {.id="message", .label="Message", .description="Commits when editing ends", .kind=Kind::String,
             .initial={Kind::String, 0, 0, "Hello OreUI"}},
            {.id="ping", .label="Log a ping", .kind=Kind::Action},
            {.id="about", .label="SDK Settings Demo", .description="Changes are stored per Mod.", .kind=Kind::Text},
            {.id="preview", .label="Crystal preview", .description="PNG embedded in the Mod DLL.",
             .kind=Kind::Image, .image_png=demo_preview_png, .image_png_size=sizeof(demo_preview_png)},
        };
        auto page = makePage(mod_id, tr("title"), controls, changed);
        const auto result = registerPage(page.translations(dictionaries));
        logger.info("Register -> ", static_cast<unsigned>(result));
        return result == Result::Ok ? 0 : 4;
    } catch (...) {
        OutputDebugStringA("[ModSettingsDemo] Registration threw\n");
        return 5;
    }
}
} // namespace

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(instance);
        if (const auto thread = CreateThread(nullptr, 0, start, instance, 0, nullptr)) CloseHandle(thread);
    }
    return TRUE;
}
