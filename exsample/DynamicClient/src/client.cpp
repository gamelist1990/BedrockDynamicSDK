#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include "client.hpp"
#include <bedrock/sdk.hpp>
#include <bedrock/logging/file_log_sink.hpp>

namespace {
bedrock::Logger logger{"DynamicClient"};
bedrock::FileLogSink fileLog;

template<class Start>
void startFeature(const char* name, Start start) {
    try { start(); }
    catch (const std::exception& e) { logger.error(name, ": ", e.what()); }
    catch (...) { logger.error(name, ": initialization failed"); }
}

class DynamicClientMod final : public bedrock::sdk::Mod {
public:
    DynamicClientMod() : Mod("dynamic-client") {}

private:
    void onEnable() override {
        // Content Log smoke line: identifiable in Settings -> Creator ->
        // Content Log on BOL. The same SDK logger automatically routes every
        // DynamicClient line there once the native core installs the bridge.
        logger.info("[ContentLogSmoke] DynamicClient onEnable: SDK logger -> vanilla Content Log");
        dynamic_client::registerSettings(*this, logger);
        dynamic_client::registerKeybinds(*this);
    }

    void onDisable() override {
        const auto removed = settings("title").unregisterPage();
        if (!removed) throw std::runtime_error(removed.error().message);
    }

    void onPlayerSpawn(bedrock::sdk::Player& player) override {
        const auto position = player.location();
        if (position) logger.info("SDK local player ready at ", position->x, ", ", position->y, ", ", position->z);
        else logger.info("SDK local player ready (position unavailable)");
    }

    void onKeybind(std::string_view action, bool pressed) override {
        if (action == "zoom") dynamic_client::onZoomKeybind(pressed);
        else if (action == "tablist") dynamic_client::onTabListKeybind(pressed);
        else if (action == "freecam") dynamic_client::onFreecamKeybind(pressed);
    }

    void onUpdate() override {
        if (!firstTick_) {
            firstTick_ = true;
            logger.info("SDK native tick active");
        }
    }

    bool firstTick_{};
};

DynamicClientMod& clientMod() {
    static DynamicClientMod mod;
    return mod;
}

bedrock::sdk::ClientHost& clientHost() {
    // The client module is pinned; the broker/loader cannot be called from
    // process detach. An explicit loader stop can perform orderly shutdown.
    static auto* host = new bedrock::sdk::ClientHost;
    return *host;
}

DWORD WINAPI start(void* parameter) {
    HMODULE pinned{};
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                            reinterpret_cast<LPCWSTR>(&start), &pinned)) return 1;
    try {
        wchar_t path[32768]{};
        const auto length = GetModuleFileNameW(static_cast<HMODULE>(parameter), path, ARRAYSIZE(path));
        if (!length || length >= ARRAYSIZE(path)) return 2;
        const auto directory = std::filesystem::path(path).parent_path();
        if (fileLog.open(directory / L"DynamicClient.log")) fileLog.attach(logger);
        using namespace dynamic_client;
        // Validate pristine mapped .text before OreUI or gameplay installs a
        // detour. BOL/GDK can legitimately have a different on-disk hash.
        bedrock::ProfileManager profiles;
        std::string error;
        const bool supported = profiles.open({}, {bedrock::UpdatePolicy::EmbeddedOnly,
            bedrock::CandidatePolicy::AllowCandidates}, error);
        if (!supported) {
            logger.error("Game profile unavailable: ", error);
            return 4;
        }
        const auto profile = profiles.snapshot();
        if (const auto enabled = clientHost().start(clientMod(), [](const auto& error) {
                logger.error("SDK client host: ", error.message);
            }); !enabled) {
            logger.error("Cannot start SDK client host: ", enabled.error().message);
            // Keep the previously supported feature/settings paths available.
            if (const auto fallback = clientMod().enable(); !fallback)
                logger.error("Cannot enable DynamicClient: ", fallback.error().message);
        }
        logger.info("Exact bundled profile ready: ", profile->choice().version);
        startFeature("Zoom", [&] { startZoom(*profile, logger); });
        startFeature("AutoSprint", [&] { startSprint(*profile, logger); });
        startFeature("Fullbright", [&] { startBrightness(clientHost(), logger); });
        startFeature("TabList", [&] { startTabList(*profile, directory); });
        startFeature("Freecam", [&] { runFreecam(*profile, directory); });
        return 0;
    } catch (...) {
        OutputDebugStringA("[DynamicClient] Startup failed\n");
        return 3;
    }
}
}

unsigned dynamic_client::boundKey(std::string_view action, bedrock::sdk::Key defaultKey) {
    return static_cast<unsigned>(clientMod().keybinds().key(action).value_or(defaultKey));
}

BOOL WINAPI DllMain(HINSTANCE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(module);
        if (const auto worker = CreateThread(nullptr, 0, start, module, 0, nullptr)) CloseHandle(worker);
    }
    return TRUE;
}
