#include <bedrock/native/resource_packs.hpp>
#include <bedrock/generated/symbols.hpp>
#include <windows.h>
#include <shlobj.h>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <thread>

namespace {
std::atomic<unsigned> startupStatus{};
DWORD WINAPI start(void *) noexcept {
    startupStatus = 1;
    try {
        PWSTR documents{};
        if (FAILED(SHGetKnownFolderPath(FOLDERID_Documents, KF_FLAG_CREATE, nullptr, &documents)))
            throw std::runtime_error("Documents directory unavailable");
        const auto directory =
            std::filesystem::path(documents) / L"PEXData" / L"BetterResourcePackManager";
        CoTaskMemFree(documents);
        std::filesystem::create_directories(directory);
        std::ofstream log(directory / L"BetterResourcePackManager.log", std::ios::trunc);
        if (!log)
            throw std::runtime_error("log file unavailable");
        log << "BetterResourcePackManager / BedrockDynamicSDK " << BEDROCK_SDK_BUILD_ID << '\n';
        // Installed SDK thunks stay valid until process exit. The adapter and
        // worker intentionally have the same lifetime as Minecraft.
        auto *packs = new bedrock::NativeResourcePacks;
        bedrock::ProfileManager profiles;
        std::string error;
        const bedrock::ProfileOptions options{bedrock::UpdatePolicy::RefreshIfNeeded,
                                              bedrock::CandidatePolicy::AllowCandidates};
        if (!profiles.prepare(
                {bedrock::Feature::ResourcePacks}, options,
                [packs](const bedrock::RuntimeProfile &profile, std::string &failure) {
                    return packs->load(profile, failure);
                },
                error) ||
            !packs->install(error)) {
            log << "Initialization failed: " << error << '\n';
            delete packs;
            startupStatus = 4;
            return 1;
        }
        log << "Hooks installed. Exact-build native menu adapter is experimental; runtime "
               "validation pending.\n";
        log.flush();
        startupStatus = 2;
        bedrock::NativeResourcePacks::Status previous{};
        for (;;) {
            std::this_thread::sleep_for(std::chrono::seconds{2});
            const auto current = packs->status();
            if (current.settingsOpened != previous.settingsOpened ||
                current.preGameOverrides != previous.preGameOverrides ||
                current.failures != previous.failures) {
                log << "settings=" << current.settingsOpened
                    << " pre_game_overrides=" << current.preGameOverrides
                    << " failures=" << current.failures << '\n';
                log.flush();
                previous = current;
            }
        }
    } catch (const std::exception &error) {
        OutputDebugStringA(
            (std::string("[BetterResourcePackManager] ") + error.what() + "\n").c_str());
        startupStatus = 4;
        return 1;
    } catch (...) {
        startupStatus = 4;
        return 1;
    }
}
} // namespace
extern "C" __declspec(dllexport) unsigned BetterResourcePackManager_Status() noexcept {
    return startupStatus.load();
}
BOOL WINAPI DllMain(HINSTANCE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(module);
        HANDLE thread = CreateThread(nullptr, 0, start, nullptr, 0, nullptr);
        if (!thread)
            return FALSE;
        CloseHandle(thread);
    }
    return TRUE;
}
