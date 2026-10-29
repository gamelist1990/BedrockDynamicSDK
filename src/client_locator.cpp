#ifdef _WIN32
// Runtime ClientInstance* acquisition from naturally invoked member methods.
// No fixed global offsets; no ABI-dependent calls on an unknown ClientInstance.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <MinHook.h>
#include "bedrock/runtime/client_locator.hpp"
#include "bedrock/runtime/client_instance_tracker.hpp"
#include "bedrock/generated/symbols.hpp"
#include "bedrock/profiles/resolver.hpp"
#include <atomic>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <mutex>
#include <string>

namespace {
using Method = void (__fastcall*)(void*);

std::mutex g_control;
std::atomic<unsigned> g_status{LocatorNotStarted};
std::atomic<Method> g_originalGrab{nullptr};
std::atomic<Method> g_originalRelease{nullptr};
void* g_grabTarget = nullptr;
void* g_releaseTarget = nullptr;
bool g_minhookInitialized = false;
bool g_enabled = false;
HMODULE g_minecraft = nullptr;
std::uintptr_t g_imageBase = 0;
std::uintptr_t g_imageEnd = 0;
std::uintptr_t g_textStart = 0;
std::uintptr_t g_textEnd = 0;

bool readableRegion(const void* ptr, std::size_t size) noexcept {
    if (!ptr || !size) return false;
    const auto address = reinterpret_cast<std::uintptr_t>(ptr);
    if (address > UINTPTR_MAX - size) return false;
    MEMORY_BASIC_INFORMATION region{};
    if (!VirtualQuery(ptr, &region, sizeof(region)) ||
        region.State != MEM_COMMIT ||
        (region.Protect & (PAGE_GUARD | PAGE_NOACCESS)))
        return false;
    auto start = reinterpret_cast<std::uintptr_t>(region.BaseAddress);
    auto end = start + region.RegionSize;
    if (end < start || end < address + size) return false;
    const DWORD prot = region.Protect & 0xFF;
    return prot == PAGE_READONLY || prot == PAGE_READWRITE ||
           prot == PAGE_WRITECOPY || prot == PAGE_EXECUTE_READ ||
           prot == PAGE_EXECUTE_READWRITE || prot == PAGE_EXECUTE_WRITECOPY;
}

// Explicit SEH protects only the immediate pointer read; no assurance of lifetime.
bool readPointer(const void* address, std::uintptr_t& out) noexcept {
    if (!readableRegion(address, sizeof(void*))) return false;
    __try {
        out = *static_cast<const std::uintptr_t*>(address);
        return true;
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool validateClientInstance(const void* candidate, void*) noexcept {
    if (!candidate || !g_imageBase || !g_textStart) return false;
    // A polymorphic client object starts with a vtable pointer.
    // Deliberately do NOT use historical ClientInstance field offsets.
    std::uintptr_t vtable = 0;
    if (!readPointer(candidate, vtable) ||
        vtable < g_imageBase || vtable >= g_imageEnd)
        return false;
    std::uintptr_t firstVirtualFunction = 0;
    if (!readPointer(reinterpret_cast<void*>(vtable), firstVirtualFunction))
        return false;
    return firstVirtualFunction >= g_textStart &&
           firstVirtualFunction < g_textEnd;
}

bedrock::ClientInstanceTracker g_tracker(validateClientInstance);
void trace(const char* text) noexcept { OutputDebugStringA(text); }

bool calculateImageSections() {
    auto* base = reinterpret_cast<const unsigned char*>(GetModuleHandleW(nullptr));
    if (!base) return false;
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0 ||
        dos->e_lfanew > 0x100000) return false;
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE ||
        nt->FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64) return false;
    g_imageBase = reinterpret_cast<std::uintptr_t>(base);
    g_imageEnd = g_imageBase + nt->OptionalHeader.SizeOfImage;
    const auto* sec = IMAGE_FIRST_SECTION(nt);
    for (WORD i=0;i<nt->FileHeader.NumberOfSections;++i) {
        if (memcmp(sec[i].Name, ".text", 5) == 0) {
            g_textStart = g_imageBase + sec[i].VirtualAddress;
            g_textEnd = g_textStart + sec[i].Misc.VirtualSize;
            return g_textEnd <= g_imageEnd;
        }
    }
    return false;
}

void __fastcall observeGrab(void* self) {
    if (g_tracker.observe(self))
        g_status.store(LocatorCaptured, std::memory_order_release);
    auto fn = g_originalGrab.load(std::memory_order_acquire);
    if (fn) fn(self);
}
void __fastcall observeRelease(void* self) {
    if (g_tracker.observe(self))
        g_status.store(LocatorCaptured, std::memory_order_release);
    auto fn = g_originalRelease.load(std::memory_order_acquire);
    if (fn) fn(self);
}

void removeHooks() {
    if (g_grabTarget) {
        MH_DisableHook(g_grabTarget);
        MH_RemoveHook(g_grabTarget);
    }
    if (g_releaseTarget) {
        MH_DisableHook(g_releaseTarget);
        MH_RemoveHook(g_releaseTarget);
    }
    g_grabTarget = g_releaseTarget = nullptr;
    g_originalGrab.store(nullptr);
    g_originalRelease.store(nullptr);
    g_enabled = false;
    if (g_minhookInitialized) {
        MH_Uninitialize();
        g_minhookInitialized = false;
    }
    g_tracker.clear();
}

unsigned start() {
    std::lock_guard<std::mutex> lock(g_control);
    if (g_enabled) return g_status.load();
    g_tracker.clear();
    g_status.store(LocatorStarting);
    bedrock::Resolver resolver;
    std::string error;
    if (!resolver.loadJson(bedrock::generated::kSymbolDatabase, error) ||
        !resolver.attachWindowsMainModule(error) ||
        !resolver.exactBuild() || !calculateImageSections()) {
        trace(("BedrockClientLocator: unsupported client executable: "+error+"\n").c_str());
        g_status.store(LocatorUnsupportedBuild);
        return LocatorUnsupportedBuild;
    }
    const auto grab = resolver.resolve("ClientInstance::grabCursor", true);
    const auto release = resolver.resolve("ClientInstance::releaseCursor", true);
    // Both methods were manually inspected as x64 thiscall (RCX = this)
    // for 1.26.52.3. For any unknown PE hash, this code refuses to run.
    if (!grab || !release || !grab->exact_build || !release->exact_build ||
        grab->method != "exact-rva" || release->method != "exact-rva" ||
        grab->address == release->address) {
        trace("BedrockClientLocator: missing verified-build method anchors\n");
        g_status.store(LocatorUnsupportedBuild);
        return LocatorUnsupportedBuild;
    }
    MH_STATUS hookStatus = MH_Initialize();
    if (hookStatus != MH_OK && hookStatus != MH_ERROR_ALREADY_INITIALIZED) {
        g_status.store(LocatorHookFailure);
        return LocatorHookFailure;
    }
    g_minhookInitialized = true;
    g_grabTarget = reinterpret_cast<void*>(grab->address);
    g_releaseTarget = reinterpret_cast<void*>(release->address);
    Method originalGrab = nullptr;
    Method originalRelease = nullptr;
    if (MH_CreateHook(g_grabTarget, reinterpret_cast<void*>(&observeGrab),
                      reinterpret_cast<void**>(&originalGrab)) != MH_OK ||
        MH_CreateHook(g_releaseTarget, reinterpret_cast<void*>(&observeRelease),
                      reinterpret_cast<void**>(&originalRelease)) != MH_OK) {
        removeHooks();
        g_status.store(LocatorHookFailure);
        return LocatorHookFailure;
    }
    g_originalGrab.store(originalGrab, std::memory_order_release);
    g_originalRelease.store(originalRelease, std::memory_order_release);
    // Pin before exposing a trampoline; the OS must never unload code to
    // which another thread is currently being redirected.
    HMODULE pinned = nullptr;
    if (!GetModuleHandleExW(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
            reinterpret_cast<LPCWSTR>(&observeGrab), &pinned)) {
        removeHooks();
        g_status.store(LocatorHookFailure);
        return LocatorHookFailure;
    }
    if (MH_QueueEnableHook(g_grabTarget) != MH_OK ||
        MH_QueueEnableHook(g_releaseTarget) != MH_OK) {
        removeHooks();
        g_status.store(LocatorHookFailure);
        return LocatorHookFailure;
    }
    // Advertise waiting *before* enabling hooks. An immediately dispatched
    // cursor event may set LocatorCaptured and must not be overwritten.
    g_status.store(LocatorWaitingForClientEvent, std::memory_order_release);
    if (MH_ApplyQueued() != MH_OK) {
        removeHooks();
        g_status.store(LocatorHookFailure);
        return LocatorHookFailure;
    }
    g_enabled = true;
    trace("BedrockClientLocator: hooked cursor methods; waiting for normal cursor event.\n");
    return g_status.load(std::memory_order_acquire);
}
unsigned stop() {
    std::lock_guard<std::mutex> lock(g_control);
    removeHooks();
    g_status.store(LocatorStopped);
    trace("BedrockClientLocator: stopped and cleared pointer.\n");
    return LocatorStopped;
}
DWORD WINAPI worker(void*) {
    start();
    return 0;
}
} // namespace

BEDROCK_EXPORT unsigned BedrockLocator_Start() { return start(); }
BEDROCK_EXPORT unsigned BedrockLocator_Stop() { return stop(); }
BEDROCK_EXPORT unsigned BedrockLocator_GetStatus() {
    const auto status = g_status.load(std::memory_order_acquire);
    if (status == LocatorCaptured && !g_tracker.get()) {
        g_status.store(LocatorWaitingForClientEvent, std::memory_order_release);
        return LocatorWaitingForClientEvent;
    }
    return status;
}
BEDROCK_EXPORT void* BedrockLocator_GetClientInstance() { return g_tracker.get(); }
BEDROCK_EXPORT std::uint64_t BedrockLocator_CaptureCount() { return g_tracker.captures(); }

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(instance);
        // Avoid MinHook and PE parsing inside the loader lock.
        HANDLE thread = CreateThread(nullptr, 0, worker, nullptr, 0, nullptr);
        if (thread) CloseHandle(thread);
    }
    // No MinHook cleanup in DLL_PROCESS_DETACH: the loaded DLL is pinned after
    // hooks activate. Call BedrockLocator_Stop explicitly to remove hooks.
    return TRUE;
}
#endif
