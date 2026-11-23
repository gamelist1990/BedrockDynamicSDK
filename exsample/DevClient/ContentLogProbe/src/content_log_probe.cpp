#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <bedrock/sdk.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>

namespace {

class ContentLogProbe final : public bedrock::sdk::Mod {
public:
    ContentLogProbe(HANDLE written, std::ofstream& evidence)
        : Mod("ContentLogProbe"), written_(written), evidence_(evidence) {}

private:
    void onPlayerSpawn(bedrock::sdk::Player& player) override {
        if (attempted_ || !player.isValid()) return;
        attempted_ = true;
        using namespace bedrock::sdk;
        const auto timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        const auto marker = "CPP_CONTENT_LOG_PROBE_" + std::to_string(timestamp);
        evidence_ << "ContentLog::available()=" << std::boolalpha << ContentLog::available()
                  << " marker=" << marker << std::endl;
        ContentLog::write(LogLevel::Warn, "ContentLogProbe", marker);
        SetEvent(written_);
    }

    HANDLE written_;
    std::ofstream& evidence_;
    bool attempted_{};
};

DWORD WINAPI run(void* module) {
    try {
        wchar_t path[32768]{};
        const auto length = GetModuleFileNameW(static_cast<HMODULE>(module), path, ARRAYSIZE(path));
        if (!length || length >= ARRAYSIZE(path)) return 1;
        std::ofstream evidence(std::filesystem::path(path).parent_path() / "ContentLogProbe.log");
        if (!evidence) return 2;
        struct Event {
            HANDLE handle = CreateEventW(nullptr, TRUE, FALSE, nullptr);
            ~Event() { if (handle) CloseHandle(handle); }
        } written;
        if (!written.handle) return 3;
        ContentLogProbe probe(written.handle, evidence);
        bedrock::sdk::ClientHost host;
        if (const auto started = host.start(probe); !started) {
            evidence << "start failed: " << started.error().message << '\n';
            return 4;
        }
        const auto wait = WaitForSingleObject(written.handle, 180'000);
        if (const auto stopped = host.stop(); !stopped) {
            evidence << "stop failed: " << stopped.error().message << '\n';
            return 5;
        }
        evidence << (wait == WAIT_OBJECT_0 ? "write returned\n" : "no write observed\n");
        return wait == WAIT_OBJECT_0 ? 0 : 6;
    } catch (...) {
        return 7;
    }
}

} // namespace

BOOL WINAPI DllMain(HINSTANCE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(module);
        if (const auto worker = CreateThread(nullptr, 0, run, module, 0, nullptr)) CloseHandle(worker);
    }
    return TRUE;
}
