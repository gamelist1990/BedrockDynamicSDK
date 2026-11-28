// ClientInfoProbe — read-only client diagnostics smoke mod.
//
// Logs player.fps / ping / cps / device through ContentLog (and the probe's own
// file) every ~3 s while a world is loaded. The values come from the SDK's
// ClientInfo sampler, which ClientHost::start() installs on this build.
//
// Enable the game's Creator Content Log, then load CheckSig.dll,
// BedrockHookRuntime.dll and ClientInfoProbe.dll in that order and enter a
// world. Lines are tagged "ClientInfoProbe".

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <bedrock/sdk.hpp>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

namespace {

class ClientInfoProbe final : public bedrock::sdk::Mod {
public:
    explicit ClientInfoProbe(std::ofstream& evidence)
        : Mod("ClientInfoProbe"), evidence_(evidence) {}

private:
    void onEnable() override {
        bedrock::sdk::ContentLog::write(
            bedrock::sdk::LogLevel::Info, "ClientInfoProbe",
            std::string("ClientInfoProbe enabled; sampler ") +
                (bedrock::sdk::ClientInfo::installed() ? "installed" : "absent"));
    }

    void onUpdate() override {
        if (++ticks_ < 60) return;  // ~3 s at the client's 20 Hz tick
        ticks_ = 0;
        const auto player = world().localPlayer();
        if (!player) return;
        const auto device = player->device();
        const auto ping = player->ping();
        const auto cps = player->cps();
        char line[512];
        std::snprintf(line, sizeof(line),
                      "fps=%.1f ping=%u median=%u samples=%u connected=%s "
                      "cps=%u/%u device=%s %s %ux%u",
                      player->fps(), ping.lastMs, ping.medianMs, ping.samples,
                      ping.connected ? "true" : "false", cps.left, cps.right,
                      device.platform.empty() ? "?" : device.platform.c_str(),
                      device.clientVersion.empty() ? "?" : device.clientVersion.c_str(),
                      device.screenWidth, device.screenHeight);
        bedrock::sdk::ContentLog::write(bedrock::sdk::LogLevel::Info, "ClientInfoProbe",
                                        line);
        evidence_ << line << '\n';        evidence_.flush();
    }

    std::ofstream& evidence_;
    unsigned ticks_{};
};

DWORD WINAPI run(void* module) {
    try {
        wchar_t path[32768]{};
        const auto length = GetModuleFileNameW(static_cast<HMODULE>(module), path, ARRAYSIZE(path));
        if (!length || length >= ARRAYSIZE(path)) return 1;
        std::ofstream evidence(std::filesystem::path(path).parent_path() / "ClientInfoProbe.log");
        if (!evidence) return 2;
        evidence << "ContentLog::available()=" << std::boolalpha
                 << bedrock::sdk::ContentLog::available() << std::endl;
        ClientInfoProbe probe(evidence);
        bedrock::sdk::ClientHost host;
        if (const auto started = host.start(probe); !started) {
            evidence << "start failed: " << started.error().message << '\n';
            return 3;
        }
        // Keep the probe resident; the host owns the game callbacks.
        for (;;) Sleep(60'000);
    } catch (...) {
        return 4;
    }
}

} // namespace

BOOL WINAPI DllMain(HINSTANCE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(module);
        if (const auto worker = CreateThread(nullptr, 0, run, module, 0, nullptr))
            CloseHandle(worker);
    }
    return TRUE;
}
