#include <bedrock/native/resource_packs.hpp>
#include <bedrock/generated/symbols.hpp>
#include <windows.h>
#include <shlobj.h>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <thread>

namespace {
std::atomic<unsigned> startupStatus{};
HANDLE traceFile = INVALID_HANDLE_VALUE;
std::atomic<ULONGLONG> lastTraceTick{};
void traceStep(const char *message) noexcept {
    if (traceFile == INVALID_HANDLE_VALUE) return;
    const DWORD savedError = GetLastError();
    const ULONGLONG tick = GetTickCount64();
    lastTraceTick.store(tick, std::memory_order_relaxed);
    char line[512]{};
    const int length = _snprintf_s(line, sizeof(line), _TRUNCATE,
                                   "tick_ms=%llu thread=%lu %s", tick,
                                   static_cast<unsigned long>(GetCurrentThreadId()), message);
    DWORD written{};
    if (length > 0) WriteFile(traceFile, line, static_cast<DWORD>(length), &written, nullptr);
    FlushFileBuffers(traceFile);
    SetLastError(savedError);
}
LONG CALLBACK observeException(EXCEPTION_POINTERS *pointers) noexcept {
    if (!pointers || !pointers->ExceptionRecord || traceFile == INVALID_HANDLE_VALUE)
        return EXCEPTION_CONTINUE_SEARCH;
    const auto *record = pointers->ExceptionRecord;
    const DWORD code = record->ExceptionCode;
    if (code != 0xc0000005 && code != 0xc0000409 && code != 0xc000001d && code != 0xc0000094)
        return EXCEPTION_CONTINUE_SEARCH;
    const DWORD savedError = GetLastError();
    const auto now = GetTickCount64();
    const auto last = lastTraceTick.load(std::memory_order_relaxed);
    if (!last || now - last > 60000) {
        SetLastError(savedError);
        return EXCEPTION_CONTINUE_SEARCH;
    }
    MEMORY_BASIC_INFORMATION region{};
    const bool knownRegion = VirtualQuery(record->ExceptionAddress, &region, sizeof(region)) == sizeof(region);
    const auto address = reinterpret_cast<std::uintptr_t>(record->ExceptionAddress);
    const auto imageBase = knownRegion ? reinterpret_cast<std::uintptr_t>(region.AllocationBase) : 0;
    const auto gameBase = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    const auto offset = imageBase && address >= imageBase ? address - imageBase : 0;
    char line[512]{};
    const int length = _snprintf_s(line, sizeof(line), _TRUNCATE,
        "tick_ms=%llu thread=%lu first_chance code=0x%08lx address=0x%llx allocation=0x%llx offset=0x%llx game_image=%u flags=0x%lx params=%lu info0=0x%llx info1=0x%llx\r\n",
        now, static_cast<unsigned long>(GetCurrentThreadId()), static_cast<unsigned long>(code),
        static_cast<unsigned long long>(address), static_cast<unsigned long long>(imageBase),
        static_cast<unsigned long long>(offset), imageBase && imageBase == gameBase ? 1u : 0u,
        static_cast<unsigned long>(record->ExceptionFlags), static_cast<unsigned long>(record->NumberParameters),
        static_cast<unsigned long long>(record->NumberParameters > 0 ? record->ExceptionInformation[0] : 0),
        static_cast<unsigned long long>(record->NumberParameters > 1 ? record->ExceptionInformation[1] : 0));
    DWORD written{};
    if (length > 0) WriteFile(traceFile, line, static_cast<DWORD>(length), &written, nullptr);
    char module[MAX_PATH]{};
    if (knownRegion && region.Type == MEM_IMAGE &&
        GetModuleFileNameA(static_cast<HMODULE>(region.AllocationBase), module, MAX_PATH)) {
        _snprintf_s(line, sizeof(line), _TRUNCATE, "fault_module=%s\r\n", module);
        WriteFile(traceFile, line, static_cast<DWORD>(strnlen_s(line, sizeof(line))), &written, nullptr);
    }
    if (const auto *context = pointers->ContextRecord) {
        _snprintf_s(line, sizeof(line), _TRUNCATE,
            "fault_context rip=0x%llx rsp=0x%llx rax=0x%llx rcx=0x%llx rdx=0x%llx r8=0x%llx r9=0x%llx\r\n",
            context->Rip, context->Rsp, context->Rax, context->Rcx, context->Rdx, context->R8, context->R9);
        WriteFile(traceFile, line, static_cast<DWORD>(strnlen_s(line, sizeof(line))), &written, nullptr);
    }
    FlushFileBuffers(traceFile);
    SetLastError(savedError);
    return EXCEPTION_CONTINUE_SEARCH;
}
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
        traceFile = CreateFileW((directory / L"BetterResourcePackManager-stage.log").c_str(),
                                GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, nullptr);
        if (traceFile == INVALID_HANDLE_VALUE) throw std::runtime_error("stage diagnostic log unavailable");
        traceStep("v19 native progress candidate: started\r\n");
        if (!AddVectoredExceptionHandler(1, observeException))
            traceStep("exception_observer_install_failed\r\n");
        else traceStep("exception_observer_installed\r\n");
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
        packs->setTraceCallback(&traceStep);
        const auto snapshot = profiles.snapshot();
        log << "profile=" << snapshot->choice().version
            << " source=" << bedrock::profileSourceName(snapshot->choice().source)
            << " exe_sha256=" << snapshot->choice().identity.fileSha256
            << " text_sha256=" << snapshot->choice().identity.textSha256 << '\n';
        log << "Hooks installed. Exact-build native live reload adapter v19 native progress candidate is experimental; runtime "
               "validation pending.\n";
        log.flush();
        startupStatus = 2;
        bedrock::NativeResourcePacks::Status previous{};
        for (;;) {
            std::this_thread::sleep_for(std::chrono::seconds{2});
            const auto current = packs->status();
            if (current.settingsOpened != previous.settingsOpened ||
                current.failures != previous.failures ||
                current.settingsExited != previous.settingsExited ||
                current.applyEvents != previous.applyEvents ||
                current.stackCompositions != previous.stackCompositions ||
                current.globalStackIncluded != previous.globalStackIncluded ||
                current.applyNavigations != previous.applyNavigations ||
                current.liveApplications != previous.liveApplications ||
                current.inWorldApplyEvents != previous.inWorldApplyEvents ||
                current.gameGateOverrides != previous.gameGateOverrides ||
                current.terrainAtlasCompleted != previous.terrainAtlasCompleted ||
                current.refreshCancelled != previous.refreshCancelled ||
                current.resourceWaitFrames != previous.resourceWaitFrames ||
                current.platformCycles != previous.platformCycles ||
                current.blockGraphicsRebuilt != previous.blockGraphicsRebuilt ||
                current.graphicsCacheResets != previous.graphicsCacheResets ||
                current.camerasRefreshed != previous.camerasRefreshed ||
                current.remoteReloads != previous.remoteReloads ||
                current.suspendGateOverrides != previous.suspendGateOverrides ||
                current.geometryReloaded != previous.geometryReloaded ||
                current.clientDefinitionsReloaded != previous.clientDefinitionsReloaded ||
                current.customMaterialsRemapped != previous.customMaterialsRemapped ||
                current.customUvSetsUpdated != previous.customUvSetsUpdated ||
                current.customUvMismatched != previous.customUvMismatched ||
                current.customUvUnmatched != previous.customUvUnmatched ||
                current.customUvUnchanged != previous.customUvUnchanged ||
                current.nativeScreenWaitFrames != previous.nativeScreenWaitFrames ||
                current.previousReloadWaitFrames != previous.previousReloadWaitFrames) {
                log << "settings=" << current.settingsOpened
                    << " pre_game_overrides=" << current.preGameOverrides
                    << " exits=" << current.settingsExited
                    << " apply_events=" << current.applyEvents
                    << " stack_compositions=" << current.stackCompositions
                    << " global_stack_included=" << current.globalStackIncluded
                    << " apply_navigations=" << current.applyNavigations
                    << " live_applications=" << current.liveApplications
                    << " in_world_apply_events=" << current.inWorldApplyEvents
                    << " game_gate_overrides=" << current.gameGateOverrides
                    << " terrain_atlas_completed=" << current.terrainAtlasCompleted
                    << " refresh_cancelled=" << current.refreshCancelled
                    << " resource_wait_frames=" << current.resourceWaitFrames
                    << " platform_cycles=" << current.platformCycles
                    << " block_graphics_rebuilt=" << current.blockGraphicsRebuilt
                    << " graphics_cache_resets=" << current.graphicsCacheResets
                    << " remote_reloads=" << current.remoteReloads
                    << " suspend_gate_overrides=" << current.suspendGateOverrides
                    << " cameras_refreshed=" << current.camerasRefreshed
                    << " geometry_reloaded=" << current.geometryReloaded
                    << " client_definitions_reloaded=" << current.clientDefinitionsReloaded
                    << " previous_reload_wait_frames=" << current.previousReloadWaitFrames
                    << " native_screen_wait_frames=" << current.nativeScreenWaitFrames
                    << " custom_materials_remapped=" << current.customMaterialsRemapped
                    << " custom_uv_sets_updated=" << current.customUvSetsUpdated
                    << " custom_uv_mismatched=" << current.customUvMismatched
                    << " custom_uv_unmatched=" << current.customUvUnmatched
                    << " custom_uv_unchanged=" << current.customUvUnchanged
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
