#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <tlhelp32.h>
#include <MinHook.h>
#include <bedrock/runtime/hook_runtime.h>

#include <array>
#include <atomic>
#include <cstdio>
#include <algorithm>
#include <condition_variable>
#include <cstdint>
#include <cwchar>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

int BDS_HOOK_CALL leaderSubscribe(BDS_HookPoint, void*, BDS_HookCallback,
    void*, BDS_HookToken*);
void BDS_HOOK_CALL leaderUnsubscribe(BDS_HookToken);
int BDS_HOOK_CALL leaderOwns(BDS_HookPoint, void*);
uintptr_t BDS_HOOK_CALL leaderDispatch(BDS_HookPoint, void*);
uintptr_t BDS_HOOK_CALL leaderOriginal(BDS_HookPoint, void*);
const char* BDS_HOOK_CALL leaderVersion();
int BDS_HOOK_CALL leaderDirectInstall(void*, void*, void**, BDS_HookToken*);
int BDS_HOOK_CALL leaderDirectRemove(BDS_HookToken);

namespace {
using KeyboardFn = bool(__fastcall*)(int, bool);
using MouseFn = void(__fastcall*)(void*, std::int8_t, std::int8_t,
    std::int16_t, std::int16_t, std::int16_t, std::int16_t, bool);
using CursorFn = void(__fastcall*)(void*);
using FovFn = float(__fastcall*)(void*, float, void*, void*);
using RenderLevelFn = void(__fastcall*)(void*, void*, void*);
using LevelFn = void(__fastcall*)(void*);
using RuntimeActorListFn = void(__fastcall*)(void*, void*);
using ClientPlayerLoadedFn = void(__fastcall*)(void*, void*);
using ActorNameTagFn = const std::string&(__fastcall*)(void*);
using GameCoreKeyboardEventFn = void*(__fastcall*)(void*, void*, void*);

struct Entry {
    BDS_HookToken token{};
    BDS_HookCallback callback{};
    void* user{};
    std::mutex mutex;
    std::condition_variable idle;
    unsigned active{};
    bool accepting{true};
};
struct State {
    void* target{};
    void* original{};
    std::vector<std::shared_ptr<Entry>> entries;
};
struct Frame {
    BDS_HookPoint point{};
    std::size_t index{};
    std::vector<std::shared_ptr<Entry>> entries;
};

std::mutex registryMutex;
std::array<State, 19> states{};
bool minHookInitialized{};
bool modulePinned{};
bool clientModulePinned{};

struct DirectEntry {
    BDS_HookToken token{};
    void* target{};
    void* detour{};
    void* original{};
};
std::vector<std::shared_ptr<DirectEntry>> directEntries;

using SubscribeFn = int(BDS_HOOK_CALL*)(BDS_HookPoint, void*, BDS_HookCallback,
    void*, BDS_HookToken*);
using UnsubscribeFn = void(BDS_HOOK_CALL*)(BDS_HookToken);
using OwnsFn = int(BDS_HOOK_CALL*)(BDS_HookPoint, void*);
using DispatchFn = uintptr_t(BDS_HOOK_CALL*)(BDS_HookPoint, void*);
using OriginalFn = uintptr_t(BDS_HOOK_CALL*)(BDS_HookPoint, void*);
using VersionFn = const char*(BDS_HOOK_CALL*)();
using DirectInstallFn = int(BDS_HOOK_CALL*)(void*, void*, void**, BDS_HookToken*);
using DirectRemoveFn = int(BDS_HOOK_CALL*)(BDS_HookToken);

struct SharedApi {
    volatile LONG state; // 0 empty, 1 initializing, 2 ready, -1 incompatible/failed
    DWORD ownerPid;
    DWORD abiVersion;
    SubscribeFn subscribe;
    UnsubscribeFn unsubscribe;
    OwnsFn owns;
    DispatchFn dispatch;
    OriginalFn original;
    VersionFn version;
    DirectInstallFn directInstall;
    DirectRemoveFn directRemove;
};
// New points require a new owner capability version. An older process owner
// must reject them, rather than silently installing a second hook broker.
// The v3/v4 table prefix and argument records are binary-identical. Keep ABI
// stable for additive releases; runtime version and supported points are separate.
constexpr DWORD kSharedAbiVersion = 4;
constexpr DWORD kMinimumCompatibleAbi = 3;
constexpr unsigned kBuildVersion = 0x00040700u; // major/minor/patch: 4.7.0
SharedApi* getSharedApi(bool discover = true);
SharedApi* bootstrapPreferredRuntime();
std::mutex diagnosticMutex;
std::string diagnosticText = "Hook runtime has not initialized.";

std::wstring modulePath(HMODULE module) {
    wchar_t path[32768]{};
    const auto length = GetModuleFileNameW(module, path, 32768);
    return length && length < 32768 ? std::wstring(path, length) : std::wstring{};
}
HMODULE localModule() {
    HMODULE module{};
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
        GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCWSTR>(&BDS_HookSubscribe), &module);
    return module;
}
// Separate, versioned metadata leaves the legacy SharedApi allocation and
// function-table prefix unchanged. Future additive hooks change only the mask.
struct Capabilities {
    volatile LONG state;
    DWORD magic;
    DWORD size;
    DWORD pid;
    DWORD abi;
    unsigned buildVersion;
    std::uintptr_t ownerSubscribe;
    std::uint64_t points;
};
constexpr DWORD kCapabilitiesMagic=0x42444331; // BDC1
HANDLE capabilitiesMapping{};
Capabilities* capabilities{};
void capabilitiesName(wchar_t* name,std::size_t size) {
    swprintf_s(name,size,L"Local\\BedrockDynamicSDK.HookCapabilities.%lu",GetCurrentProcessId());
}
void publishCapabilities(const SharedApi* owner) {
    wchar_t name[96]{};capabilitiesName(name,96);
    capabilitiesMapping=CreateFileMappingW(INVALID_HANDLE_VALUE,nullptr,PAGE_READWRITE,
        0,sizeof(Capabilities),name);
    if(!capabilitiesMapping) return;
    capabilities=static_cast<Capabilities*>(MapViewOfFile(capabilitiesMapping,
        FILE_MAP_READ|FILE_MAP_WRITE,0,0,sizeof(Capabilities)));
    if(!capabilities)return;
    InterlockedExchange(&capabilities->state,1);
    capabilities->magic=kCapabilitiesMagic;capabilities->size=sizeof(Capabilities);
    capabilities->pid=GetCurrentProcessId();capabilities->abi=owner->abiVersion;
    capabilities->buildVersion=kBuildVersion;
    capabilities->ownerSubscribe=reinterpret_cast<std::uintptr_t>(owner->subscribe);
    capabilities->points=(std::uint64_t{1}<<(BDS_HOOK_PACKET_READ+1))-2;
    MemoryBarrier();InterlockedExchange(&capabilities->state,2);
}
const Capabilities* ownerCapabilities(const SharedApi* owner) {
    static std::once_flag once;
    std::call_once(once,[]{
        // Main owner initialization completed before capability lookup. An old
        // owner has no metadata; do not create a misleading new descriptor.
        if(capabilities)return;
        wchar_t name[96]{};capabilitiesName(name,96);
        capabilitiesMapping=OpenFileMappingW(FILE_MAP_READ,FALSE,name);
        if(capabilitiesMapping) capabilities=static_cast<Capabilities*>(
            MapViewOfFile(capabilitiesMapping,FILE_MAP_READ,0,0,sizeof(Capabilities)));
    });
    const auto* value=capabilities;
    if(!value || value->state!=2)return nullptr;
    MemoryBarrier();
    return value->magic==kCapabilitiesMagic && value->size==sizeof(Capabilities) &&
        value->pid==GetCurrentProcessId() && value->abi==owner->abiVersion &&
        value->ownerSubscribe==reinterpret_cast<std::uintptr_t>(owner->subscribe) ? value : nullptr;
}
HANDLE sharedMapping{};
SharedApi* sharedApi{};
std::mutex sharedApiMutex;
std::atomic<SharedApi*> readyApi{};

SharedApi* getSharedApi(bool discover) {
    if (auto* ready = readyApi.load(std::memory_order_acquire)) return ready;
    // Start(false) never enters discovery or another provider's bootstrap lock.
    if (discover) return bootstrapPreferredRuntime();
    std::lock_guard lock(sharedApiMutex);
    if (sharedApi) return sharedApi;
    if (!clientModulePinned) {
        HMODULE pinned{};
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                GET_MODULE_HANDLE_EX_FLAG_PIN,
                reinterpret_cast<LPCWSTR>(&BDS_HookSubscribe), &pinned)) return nullptr;
        clientModulePinned = true;
    }
    wchar_t name[96]{};
    swprintf_s(name, L"Local\\BedrockDynamicSDK.HookRuntime.%lu",
        GetCurrentProcessId());
    sharedMapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE,
        0, sizeof(SharedApi), name);
    if (!sharedMapping) return nullptr;
    const bool creator = GetLastError() != ERROR_ALREADY_EXISTS;
    sharedApi = static_cast<SharedApi*>(MapViewOfFile(sharedMapping,
        FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, sizeof(SharedApi)));
    if (!sharedApi) { CloseHandle(sharedMapping); sharedMapping = nullptr; return nullptr; }
    if (creator) {
        ZeroMemory(sharedApi, sizeof(*sharedApi));
        InterlockedExchange(&sharedApi->state, 1);
        HMODULE pinned{};
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                GET_MODULE_HANDLE_EX_FLAG_PIN,
                reinterpret_cast<LPCWSTR>(&leaderSubscribe), &pinned)) {
            InterlockedExchange(&sharedApi->state, -1);
            UnmapViewOfFile(sharedApi); sharedApi = nullptr;
            CloseHandle(sharedMapping); sharedMapping = nullptr;
            return nullptr;
        }
        modulePinned = true;
        sharedApi->ownerPid = GetCurrentProcessId();
        sharedApi->abiVersion = kSharedAbiVersion;
        sharedApi->subscribe = &leaderSubscribe;
        sharedApi->unsubscribe = &leaderUnsubscribe;
        sharedApi->owns = &leaderOwns;
        sharedApi->dispatch = &leaderDispatch;
        sharedApi->original = &leaderOriginal;
        sharedApi->version = &leaderVersion;
        sharedApi->directInstall = &leaderDirectInstall;
        sharedApi->directRemove = &leaderDirectRemove;
        publishCapabilities(sharedApi);
        MemoryBarrier();
        InterlockedExchange(&sharedApi->state, 2);
    } else {
        for (unsigned i = 0; i < 500; ++i) {
            const auto state = InterlockedCompareExchange(&sharedApi->state, 0, 0);
            if (state != 0 && state != 1) break;
            Sleep(10);
        }
        if (InterlockedCompareExchange(&sharedApi->state, 0, 0) != 2 ||
            (sharedApi->abiVersion < kMinimumCompatibleAbi ||
             sharedApi->abiVersion > kSharedAbiVersion) ||
            sharedApi->ownerPid != GetCurrentProcessId()) {
            UnmapViewOfFile(sharedApi); sharedApi = nullptr;
            CloseHandle(sharedMapping); sharedMapping = nullptr;
            return nullptr;
        }
    }
    readyApi.store(sharedApi, std::memory_order_release);
    return sharedApi;
}

// Only initialization and explicit diagnostic queries format/output text.
void reportRuntime(const SharedApi* owner, unsigned selectedVersion,
    const std::wstring& selectedPath, const std::wstring& discovery) {
    std::wstring text = L"BedrockHookRuntime: " + discovery + L"; selected provider=" +
        std::to_wstring((selectedVersion >> 16) & 255) + L"." +
        std::to_wstring((selectedVersion >> 8) & 255) + L"." +
        std::to_wstring(selectedVersion & 255) + L" path=" + selectedPath;
    if (owner) {
        HMODULE module{};
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
            GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(owner->subscribe), &module);
        const char* version = owner->version ? owner->version() : "unknown";
        std::wstring wideVersion;
        for (const char* c = version; *c; ++c) wideVersion += static_cast<unsigned char>(*c);
        text += L"; actual owner=" + wideVersion + L" ABI=" +
            std::to_wstring(owner->abiVersion) + L" path=" +
            (module ? modulePath(module) : L"unknown");
        unsigned actual{}, major{}, minor{}, patch{};
        if (const auto* info = ownerCapabilities(owner)) actual = info->buildVersion;
        else if (sscanf_s(version, "%u.%u.%u", &major, &minor, &patch) == 3)
            actual = (major << 16) | (minor << 8) | patch;
        if (!actual || actual < selectedVersion || owner->abiVersion != kSharedAbiVersion)
            text += L"; existing older/unknown owner retained: restart Minecraft with the newest provider loaded before first runtime use";
    } else text += L"; owner unavailable (existing mapping retained; no competing broker); initialization may be retried; an incompatible active owner requires a Minecraft restart";
    text += L"\n";
    const int size = WideCharToMultiByte(CP_UTF8, 0, text.data(),
        static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    std::string utf8(size > 0 ? size : 0, '\0');
    if (size > 0) WideCharToMultiByte(CP_UTF8, 0, text.data(),
        static_cast<int>(text.size()), utf8.data(), size, nullptr, nullptr);
    { std::lock_guard lock(diagnosticMutex); diagnosticText = std::move(utf8); }
    OutputDebugStringW(text.c_str());
}

SharedApi* bootstrapPreferredRuntime() {
    static std::mutex bootstrapMutex;
    std::lock_guard lock(bootstrapMutex);
    if (auto* ready = readyApi.load(std::memory_order_acquire)) return ready;
    using Query = unsigned(__cdecl*)();
    using Start = int(__cdecl*)();
    struct Provider {
        std::shared_ptr<void> reference;
        std::wstring path;
        unsigned version;
        Start start;
        bool sidecar;
    };
    std::vector<Provider> providers;
    auto inspect = [&](HMODULE module) {
        // Own this reference until selection and synchronous Start finish.
        std::shared_ptr<void> reference(module, [](void* value) {
            if (value) FreeLibrary(static_cast<HMODULE>(value));
        });
        if (!module) return;
        const auto version = reinterpret_cast<Query>(GetProcAddress(module, "BedrockHookRuntime_BuildVersion"));
        const auto abi = reinterpret_cast<Query>(GetProcAddress(module, "BedrockHookRuntime_BuildAbi"));
        const auto start = reinterpret_cast<Start>(GetProcAddress(module, "BedrockHookRuntime_Start"));
        if (!version || !abi || !start || abi() != kSharedAbiVersion) return;
        const unsigned build = version();
        if (build < kBuildVersion) return; // The caller is always a compatible baseline.
        auto path = modulePath(module);
        if (path.empty()) return;
        const auto separator = path.find_last_of(L"\\/");
        const bool sidecar = _wcsicmp(path.c_str() +
            (separator == std::wstring::npos ? 0 : separator + 1), L"BedrockHookRuntime.dll") == 0;
        providers.push_back({std::move(reference), std::move(path), build, start, sidecar});
    };
    const auto selfPath = modulePath(localModule());
    std::wstring discovery;
    const auto separator = selfPath.find_last_of(L"\\/");
    if (separator != std::wstring::npos &&
        _wcsicmp(selfPath.c_str() + separator + 1, L"BedrockHookRuntime.dll") != 0) {
        const auto sidecarPath = selfPath.substr(0, separator + 1) + L"BedrockHookRuntime.dll";
        const auto sidecar = LoadLibraryExW(sidecarPath.c_str(), nullptr,
            LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
        const DWORD error = sidecar ? ERROR_SUCCESS : GetLastError();
        discovery = L"sidecar path=" + sidecarPath + L" LoadLibraryEx GetLastError=" + std::to_wstring(error);
        inspect(sidecar);
    } else discovery = L"sidecar probe skipped (self is sidecar or path unavailable)";

    HANDLE snapshot = INVALID_HANDLE_VALUE;
    for (unsigned attempt = 0; attempt < 3; ++attempt) {
        snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32,
            GetCurrentProcessId());
        if (snapshot != INVALID_HANDLE_VALUE || GetLastError() != ERROR_BAD_LENGTH) break;
    }
    if (snapshot != INVALID_HANDLE_VALUE) {
        MODULEENTRY32W entry{};
        entry.dwSize = sizeof(entry);
        if (Module32FirstW(snapshot, &entry)) do {
            HMODULE referenced{};
            if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                    reinterpret_cast<LPCWSTR>(entry.modBaseAddr), &referenced)) {
                // A snapshot does not hold references: reject unload/address reuse.
                if (_wcsicmp(modulePath(referenced).c_str(), entry.szExePath) == 0)
                    inspect(referenced);
                else FreeLibrary(referenced);
            }
        } while (Module32NextW(snapshot, &entry));
        else discovery += L"; Module32First GetLastError=" + std::to_wstring(GetLastError());
        CloseHandle(snapshot);
    } else discovery += L"; module snapshot GetLastError=" + std::to_wstring(GetLastError());
    std::sort(providers.begin(), providers.end(), [](const Provider& a, const Provider& b) {
        if (a.version != b.version) return a.version > b.version;
        if (a.sidecar != b.sidecar) return a.sidecar;
        const int order = _wcsicmp(a.path.c_str(), b.path.c_str());
        if (order) return order < 0;
        return a.path < b.path;
    });
    unsigned selectedVersion = kBuildVersion;
    std::wstring selectedPath = selfPath;
    if (!providers.empty()) {
        const auto& selected = providers.front();
        selectedVersion = selected.version;
        selectedPath = selected.path;
        // 4.4 providers use getSharedApi(false), so A -> B -> A cannot occur.
        discovery += L"; selected Start=" + std::to_wstring(selected.start());
    } else discovery += L"; using local provider";
    // Attach to the one published table, even if Start failed or an older mod
    // won election. Never replace/reset that table or create another namespace.
    auto* owner = getSharedApi(false);
    reportRuntime(owner, selectedVersion, selectedPath, discovery);
    // Failure leaves readyApi null, allowing the next API call to retry.
    return owner;
}

bool pinOwnerModule() {
    if (modulePinned) return true;
    HMODULE pinned{};
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
            GET_MODULE_HANDLE_EX_FLAG_PIN,
            reinterpret_cast<LPCWSTR>(&leaderSubscribe), &pinned)) return false;
    modulePinned = true;
    return true;
}
bool ensureMinHook() {
    if (!pinOwnerModule()) return false;
    if (minHookInitialized) return true;
    const auto result = MH_Initialize();
    if (result != MH_OK && result != MH_ERROR_ALREADY_INITIALIZED) return false;
    minHookInitialized = true;
    return true;
}

State& stateFor(BDS_HookPoint point) { return states[static_cast<unsigned>(point)]; }

bool acquire(const std::shared_ptr<Entry>& entry) {
    std::lock_guard lock(entry->mutex);
    if (!entry->accepting) return false;
    ++entry->active;
    return true;
}
void release(const std::shared_ptr<Entry>& entry) {
    std::lock_guard lock(entry->mutex);
    if (--entry->active == 0) entry->idle.notify_all();
}

uintptr_t dispatch(Frame& frame, void* args);
uintptr_t BDS_HOOK_CALL nextMiddleware(void* opaque, void* args) {
    auto continuation = *static_cast<Frame*>(opaque);
    return dispatch(continuation, args);
}

uintptr_t callOriginal(BDS_HookPoint point, void* args) {
    const auto original = stateFor(point).original;
    switch (point) {
    case BDS_HOOK_PACKET_READ:
    case BDS_HOOK_COMMAND_OUTPUT_READ: {
        auto& a = *static_cast<BDS_CommandOutputReadArgs*>(args);
        using ReadFn = void*(__fastcall*)(void*, void*, void*, void*);
        a.result = original ? reinterpret_cast<ReadFn>(original)(a.packet, a.output, a.stream, a.context) : a.output;
        return reinterpret_cast<uintptr_t>(a.result);
    }
    case BDS_HOOK_KEYBOARD_FEED: {
        auto& a = *static_cast<BDS_KeyboardArgs*>(args);
        a.result = original ? reinterpret_cast<KeyboardFn>(original)(a.key, a.down != 0) : 0;
        return static_cast<uintptr_t>(a.result);
    }
    case BDS_HOOK_MOUSE_FEED: {
        auto& a = *static_cast<BDS_MouseArgs*>(args);
        if (original) reinterpret_cast<MouseFn>(original)(a.self, a.action, a.state,
            a.x, a.y, a.dx, a.dy,
            (a.force_motionless_pointer & BDS_MOUSE_FLAG_FORCE_MOTIONLESS) != 0);
        return 0;
    }
    case BDS_HOOK_GRAB_CURSOR:
    case BDS_HOOK_RELEASE_CURSOR: {
        auto& a = *static_cast<BDS_CursorArgs*>(args);
        if (original) reinterpret_cast<CursorFn>(original)(a.self);
        return 0;
    }
    case BDS_HOOK_GET_FOV: {
        auto& a = *static_cast<BDS_FovArgs*>(args);
        a.result = original ? reinterpret_cast<FovFn>(original)(a.self, a.input_fov, a.a3, a.a4)
                            : a.input_fov;
        return 0;
    }
    case BDS_HOOK_PACKET_WIRE_SEND: {
        auto& a=*static_cast<BDS_PacketWireArgs*>(args);
        using WireFn=void(__fastcall*)(void*,const void*,uintptr_t,int);
        if(original)reinterpret_cast<WireFn>(original)(a.stage,a.bytes,a.a3,a.flags);
        return 0;
    }
    case BDS_HOOK_PACKET_SEND_TO_SERVER: {
        auto& a=*static_cast<BDS_PacketSendArgs*>(args);
        if(original) reinterpret_cast<ClientPlayerLoadedFn>(original)(a.sender,a.packet);
        return 0;
    }
    case BDS_HOOK_RENDER_LEVEL: {
        auto& a = *static_cast<BDS_RenderLevelArgs*>(args);
        if (original) reinterpret_cast<RenderLevelFn>(original)(a.renderer, a.screen, a.context);
        return 0;
    }
    case BDS_HOOK_LEVEL_SUB_TICK: {
        auto& a = *static_cast<BDS_LevelArgs*>(args);
        if (original) reinterpret_cast<LevelFn>(original)(a.level);
        return 0;
    }
    case BDS_HOOK_LEVEL_RUNTIME_ACTOR_LIST: {
        auto& a = *static_cast<BDS_RuntimeActorListArgs*>(args);
        if (original) reinterpret_cast<RuntimeActorListFn>(original)(a.level, a.output);
        return 0;
    }
    case BDS_HOOK_CLIENT_PLAYER_LOADED: {
        auto& a = *static_cast<BDS_ClientPlayerLoadedArgs*>(args);
        if (original) reinterpret_cast<ClientPlayerLoadedFn>(original)(a.self, a.player);
        return 0;
    }
    case BDS_HOOK_CLIENT_LEVEL_EXIT:
    case BDS_HOOK_CLIENT_DIMENSION_CHANGED:
    case BDS_HOOK_CLIENT_JOIN_CANCELLED: {
        auto& a = *static_cast<BDS_ClientWorldArgs*>(args);
        if (original) reinterpret_cast<CursorFn>(original)(a.self);
        return 0;
    }
    case BDS_HOOK_ACTOR_GET_NAME_TAG: {
        auto& a = *static_cast<BDS_ActorNameTagArgs*>(args);
        if (!original) return 0;
        const auto& value = reinterpret_cast<ActorNameTagFn>(original)(a.actor);
        a.name_tag = &value;
        return reinterpret_cast<uintptr_t>(&value);
    }
    case BDS_HOOK_GAMECORE_KEYBOARD_EVENT: {
        auto& a = *static_cast<BDS_GameCoreKeyboardEventArgs*>(args);
        a.result = original ? reinterpret_cast<GameCoreKeyboardEventFn>(original)(
            a.self, a.output, a.context) : a.output;
        return reinterpret_cast<uintptr_t>(a.result);
    }
    default: return 0;
    }
}

uintptr_t dispatch(Frame& frame, void* args) {
    while (frame.index < frame.entries.size()) {
        const auto entry = frame.entries[frame.index++];
        if (!acquire(entry)) continue;
        entry->callback(entry->user, args, &nextMiddleware, &frame);
        release(entry);
        return 0;
    }
    return callOriginal(frame.point, args);
}

std::vector<std::shared_ptr<Entry>> snapshot(BDS_HookPoint point) {
    std::lock_guard lock(registryMutex);
    return stateFor(point).entries;
}
void invoke(BDS_HookPoint point, void* args) {
    Frame frame{point, 0, snapshot(point)};
    dispatch(frame, args);
}

bool __fastcall keyboardDetour(int key, bool down) {
    BDS_KeyboardArgs args{key, down ? 1 : 0, 0}; invoke(BDS_HOOK_KEYBOARD_FEED, &args);
    return args.result != 0;
}
void __fastcall mouseDetour(void* self, std::int8_t action, std::int8_t state,
    std::int16_t x, std::int16_t y, std::int16_t dx, std::int16_t dy, bool force) {
    BDS_MouseArgs args{self, action, state, x, y, dx, dy, force ? 1 : 0};
    invoke(BDS_HOOK_MOUSE_FEED, &args);
}
void __fastcall grabDetour(void* self) {
    BDS_CursorArgs args{self}; invoke(BDS_HOOK_GRAB_CURSOR, &args);
}
void __fastcall releaseDetour(void* self) {
    BDS_CursorArgs args{self}; invoke(BDS_HOOK_RELEASE_CURSOR, &args);
}
float __fastcall fovDetour(void* self, float fov, void* a3, void* a4) {
    BDS_FovArgs args{self, fov, a3, a4, fov}; invoke(BDS_HOOK_GET_FOV, &args);
    return args.result;
}
void __fastcall levelDetour(void* level) {
    BDS_LevelArgs args{level}; invoke(BDS_HOOK_LEVEL_SUB_TICK, &args);
}
void __fastcall runtimeActorListDetour(void* level, void* output) {
    BDS_RuntimeActorListArgs args{level, output};
    invoke(BDS_HOOK_LEVEL_RUNTIME_ACTOR_LIST, &args);
}
void __fastcall clientPlayerLoadedDetour(void* self, void* player) {
    BDS_ClientPlayerLoadedArgs args{self, player}; invoke(BDS_HOOK_CLIENT_PLAYER_LOADED, &args);
}
void __fastcall clientLevelExitDetour(void* self) {
    BDS_ClientWorldArgs args{self}; invoke(BDS_HOOK_CLIENT_LEVEL_EXIT, &args);
}
void __fastcall clientDimensionChangedDetour(void* self) {
    BDS_ClientWorldArgs args{self}; invoke(BDS_HOOK_CLIENT_DIMENSION_CHANGED, &args);
}
void __fastcall clientJoinCancelledDetour(void* self) {
    BDS_ClientWorldArgs args{self}; invoke(BDS_HOOK_CLIENT_JOIN_CANCELLED, &args);
}
const std::string& __fastcall actorNameTagDetour(void* actor) {
    static const std::string empty;
    BDS_ActorNameTagArgs args{actor, nullptr};
    invoke(BDS_HOOK_ACTOR_GET_NAME_TAG, &args);
    return args.name_tag ? *static_cast<const std::string*>(args.name_tag) : empty;
}
void* __fastcall gameCoreKeyboardEventDetour(void* self, void* output, void* context) {
    BDS_GameCoreKeyboardEventArgs args{self, output, context, output};
    invoke(BDS_HOOK_GAMECORE_KEYBOARD_EVENT, &args);
    return args.result;
}

void __fastcall renderLevelDetour(void* renderer, void* screen, void* context) {
    BDS_RenderLevelArgs args{renderer, screen, context}; invoke(BDS_HOOK_RENDER_LEVEL, &args);
}
thread_local void* wireSendingClient{};
void __fastcall packetSendDetour(void* sender,void* packet) {
    // Preserve nested sends; no sender attribution crosses an asynchronous queue.
    struct Scope {void* previous;~Scope(){wireSendingClient=previous;}} scope{wireSendingClient};
    wireSendingClient=sender;
    BDS_PacketSendArgs args{sender,packet};invoke(BDS_HOOK_PACKET_SEND_TO_SERVER,&args);
}
void __fastcall packetWireDetour(void* stage,const void* bytes,uintptr_t a3,int flags) {
    BDS_PacketWireArgs args{stage,bytes,a3,flags,wireSendingClient};
    invoke(BDS_HOOK_PACKET_WIRE_SEND,&args);
}
void* __fastcall commandOutputReadDetour(void* packet, void* output, void* stream, void* context) {
    BDS_CommandOutputReadArgs args{packet, output, stream, context, output};
    invoke(BDS_HOOK_COMMAND_OUTPUT_READ, &args);
    return args.result;
}
void* __fastcall packetReadDetour(void* packet,void* output,void* stream,void* context) {
    BDS_PacketReadArgs args{packet,output,stream,context,output};
    invoke(BDS_HOOK_PACKET_READ,&args);return args.result;
}
void* detourFor(BDS_HookPoint point) {
    switch (point) {
    case BDS_HOOK_PACKET_READ: return reinterpret_cast<void*>(&packetReadDetour);
    case BDS_HOOK_PACKET_WIRE_SEND: return reinterpret_cast<void*>(&packetWireDetour);
    case BDS_HOOK_COMMAND_OUTPUT_READ: return reinterpret_cast<void*>(&commandOutputReadDetour);
    case BDS_HOOK_PACKET_SEND_TO_SERVER: return reinterpret_cast<void*>(&packetSendDetour);
    case BDS_HOOK_RENDER_LEVEL: return reinterpret_cast<void*>(&renderLevelDetour);
    case BDS_HOOK_KEYBOARD_FEED: return reinterpret_cast<void*>(&keyboardDetour);
    case BDS_HOOK_MOUSE_FEED: return reinterpret_cast<void*>(&mouseDetour);
    case BDS_HOOK_GRAB_CURSOR: return reinterpret_cast<void*>(&grabDetour);
    case BDS_HOOK_RELEASE_CURSOR: return reinterpret_cast<void*>(&releaseDetour);
    case BDS_HOOK_GET_FOV: return reinterpret_cast<void*>(&fovDetour);
    case BDS_HOOK_LEVEL_SUB_TICK: return reinterpret_cast<void*>(&levelDetour);
    case BDS_HOOK_LEVEL_RUNTIME_ACTOR_LIST:
        return reinterpret_cast<void*>(&runtimeActorListDetour);
    case BDS_HOOK_CLIENT_PLAYER_LOADED: return reinterpret_cast<void*>(&clientPlayerLoadedDetour);
    case BDS_HOOK_CLIENT_LEVEL_EXIT: return reinterpret_cast<void*>(&clientLevelExitDetour);
    case BDS_HOOK_CLIENT_DIMENSION_CHANGED: return reinterpret_cast<void*>(&clientDimensionChangedDetour);
    case BDS_HOOK_CLIENT_JOIN_CANCELLED: return reinterpret_cast<void*>(&clientJoinCancelledDetour);
    case BDS_HOOK_ACTOR_GET_NAME_TAG: return reinterpret_cast<void*>(&actorNameTagDetour);
    case BDS_HOOK_GAMECORE_KEYBOARD_EVENT:
        return reinterpret_cast<void*>(&gameCoreKeyboardEventDetour);
    default: return nullptr;
    }
}
bool validPoint(BDS_HookPoint point) {
    return point >= BDS_HOOK_KEYBOARD_FEED && point <= BDS_HOOK_PACKET_READ;
}
} // namespace

int BDS_HOOK_CALL leaderSubscribe(BDS_HookPoint point, void* target,
    BDS_HookCallback callback, void* user, BDS_HookToken* tokenOut) {
    if (!validPoint(point) || !target || !callback || !tokenOut) return 0;
    std::lock_guard lock(registryMutex);
    if (!ensureMinHook()) return 0;
    auto& state = stateFor(point);
    if (state.target && state.target != target) return 0;
    if (std::any_of(directEntries.begin(), directEntries.end(),
        [target](const auto& entry) { return entry->target == target; })) return 0;
    const bool first = !state.target;
    if (first) {
        state.target = target;
        if (MH_CreateHook(target, detourFor(point), &state.original) != MH_OK) {
            state = {};
            return 0;
        }
        if (MH_EnableHook(target) != MH_OK) {
            MH_RemoveHook(target);
            state = {};
            return 0;
        }
    }
    auto entry = std::make_shared<Entry>();
    entry->token = entry.get();
    entry->callback = callback;
    entry->user = user;
    state.entries.push_back(entry);
    *tokenOut = entry->token;
    return 1;
}

int BDS_HOOK_CALL leaderOwns(BDS_HookPoint point, void* target) {
    if (!validPoint(point) || !target) return 0;
    std::lock_guard lock(registryMutex);
    return stateFor(point).target == target ? 1 : 0;
}

uintptr_t BDS_HOOK_CALL leaderDispatch(BDS_HookPoint point, void* args) {
    if (!validPoint(point) || !args) return 0;
    invoke(point, args);
    if (point == BDS_HOOK_KEYBOARD_FEED)
        return static_cast<uintptr_t>(static_cast<BDS_KeyboardArgs*>(args)->result);
    if (point == BDS_HOOK_ACTOR_GET_NAME_TAG)
        return reinterpret_cast<uintptr_t>(
            static_cast<BDS_ActorNameTagArgs*>(args)->name_tag);
    if (point == BDS_HOOK_GAMECORE_KEYBOARD_EVENT)
        return reinterpret_cast<uintptr_t>(
            static_cast<BDS_GameCoreKeyboardEventArgs*>(args)->result);
    return 0;
}
uintptr_t BDS_HOOK_CALL leaderOriginal(BDS_HookPoint point, void* args) {
    if (!validPoint(point) || !args) return 0;
    return callOriginal(point, args);
}

void BDS_HOOK_CALL leaderUnsubscribe(BDS_HookToken token) {
    if (!token) return;
    std::shared_ptr<Entry> removed;
    {
        std::lock_guard lock(registryMutex);
        for (unsigned point = BDS_HOOK_KEYBOARD_FEED; point <= BDS_HOOK_PACKET_READ; ++point) {
            auto& state = states[point];
            auto it = std::find_if(state.entries.begin(), state.entries.end(),
                [token](const auto& entry) { return entry->token == token; });
            if (it == state.entries.end()) continue;
            removed = *it;
            state.entries.erase(it);
            break;
        }
    }
    if (!removed) return;
    {
        std::unique_lock lock(removed->mutex);
        removed->accepting = false;
        removed->idle.wait(lock, [&] { return removed->active == 0; });
    }
}

const char* BDS_HOOK_CALL leaderVersion() { return "4.7.0"; }

int BDS_HOOK_CALL leaderDirectInstall(void* target, void* detour,
    void** originalOut, BDS_HookToken* tokenOut) {
    if (!target || !detour || !originalOut || !tokenOut) return 0;
    std::lock_guard lock(registryMutex);
    if (!ensureMinHook()) return 0;
    if (std::any_of(states.begin(), states.end(), [target](const State& state) {
            return state.target == target;
        }) || std::any_of(directEntries.begin(), directEntries.end(),
            [target](const auto& entry) { return entry->target == target; })) return 0;
    auto entry = std::make_shared<DirectEntry>();
    entry->target = target; entry->detour = detour; entry->token = entry.get();
    if (MH_CreateHook(target, detour, &entry->original) != MH_OK) return 0;
    // A native thread can enter the detour as soon as EnableHook resumes it.
    // Publish its continuation before the target becomes callable.
    *originalOut = entry->original;
    *tokenOut = entry->token;
    MemoryBarrier();
    if (MH_EnableHook(target) != MH_OK) {
        *originalOut = nullptr;
        *tokenOut = nullptr;
        MH_RemoveHook(target);
        return 0;
    }
    directEntries.push_back(entry);
    return 1;
}

int BDS_HOOK_CALL leaderDirectRemove(BDS_HookToken token) {
    if (!token) return 0;
    std::lock_guard lock(registryMutex);
    const auto it = std::find_if(directEntries.begin(), directEntries.end(),
        [token](const auto& entry) { return entry->token == token; });
    if (it == directEntries.end()) return 0;
    const auto target = (*it)->target;
    const auto disabled = MH_DisableHook(target);
    const auto removed = MH_RemoveHook(target);
    if (disabled != MH_OK && disabled != MH_ERROR_DISABLED) return 0;
    if (removed != MH_OK) return 0;
    directEntries.erase(it);
    return 1;
}

namespace {
bool ownerSupports(const SharedApi* api,BDS_HookPoint point) {
    if(!api)return false;
    const auto index=static_cast<unsigned>(point);
    if(index==0 || index>=64)return false;
    if(const auto* info=ownerCapabilities(api))return (info->points & (std::uint64_t{1}<<index))!=0;
    // Legacy owners predate metadata; their published ABI is a known ceiling.
    const unsigned maximum=api->abiVersion==3 ? 12u : 13u;
    return index<=maximum;
}
}
extern "C" __declspec(dllexport) unsigned __cdecl BedrockHookRuntime_BuildVersion() { return kBuildVersion; }
extern "C" __declspec(dllexport) unsigned __cdecl BedrockHookRuntime_BuildAbi() { return kSharedAbiVersion; }
extern "C" __declspec(dllexport) int __cdecl BedrockHookRuntime_Start() {
    auto* owner = getSharedApi(false);
    reportRuntime(owner, kBuildVersion, modulePath(localModule()), L"explicit provider Start (discovery bypassed)");
    return owner != nullptr;
}
extern "C" __declspec(dllexport) const char* __cdecl BedrockHookRuntime_Version() {
    return BDS_HookRuntimeVersion();
}
extern "C" __declspec(dllexport) int __cdecl BedrockHookRuntime_Supports(unsigned point) {
    return BDS_HookRuntimeSupports(static_cast<BDS_HookPoint>(point));
}
extern "C" const char* BDS_HOOK_CALL BDS_HookRuntimeDiagnostic(void) {
    getSharedApi();
    // A snapshot stays valid on this thread until its next diagnostic call.
    thread_local std::string snapshot;
    std::lock_guard lock(diagnosticMutex);
    snapshot = diagnosticText;
    return snapshot.c_str();
}
extern "C" int BDS_HOOK_CALL BDS_HookRuntimeSupports(BDS_HookPoint point) {
    return ownerSupports(getSharedApi(),point) ? 1 : 0;
}
extern "C" unsigned BDS_HOOK_CALL BDS_HookRuntimeAbi(void) {
    const auto* api=getSharedApi(); return api ? api->abiVersion : 0;
}
extern "C" int BDS_HOOK_CALL BDS_HookSubscribe(BDS_HookPoint point, void* target,
    BDS_HookCallback callback, void* user, BDS_HookToken* tokenOut) {
    auto* api = getSharedApi(); return ownerSupports(api,point) && api->subscribe
        ? api->subscribe(point, target, callback, user, tokenOut) : 0;
}
extern "C" void BDS_HOOK_CALL BDS_HookUnsubscribe(BDS_HookToken token) {
    auto* api = getSharedApi(); if (api && api->unsubscribe) api->unsubscribe(token);
}
extern "C" int BDS_HOOK_CALL BDS_HookOwnsTarget(BDS_HookPoint point, void* target) {
    auto* api = getSharedApi(); return ownerSupports(api,point) && api->owns ? api->owns(point, target) : 0;
}
extern "C" uintptr_t BDS_HOOK_CALL BDS_HookDispatch(BDS_HookPoint point, void* args) {
    auto* api = getSharedApi(); return ownerSupports(api,point) && api->dispatch ? api->dispatch(point, args) : 0;
}
extern "C" uintptr_t BDS_HOOK_CALL BDS_HookCallOriginal(BDS_HookPoint point, void* args) {
    auto* api = getSharedApi(); return ownerSupports(api,point) && api->original ? api->original(point, args) : 0;
}
extern "C" const char* BDS_HOOK_CALL BDS_HookRuntimeVersion(void) {
    auto* api = getSharedApi(); return api && api->version ? api->version() : "unavailable";
}
extern "C" int BDS_HOOK_CALL BDS_HookDirectInstall(void* target, void* detour,
    void** originalOut, BDS_HookToken* tokenOut) {
    auto* api = getSharedApi(); return api && api->directInstall
        ? api->directInstall(target, detour, originalOut, tokenOut) : 0;
}
extern "C" int BDS_HOOK_CALL BDS_HookDirectRemove(BDS_HookToken token) {
    auto* api = getSharedApi(); return api && api->directRemove
        ? api->directRemove(token) : 0;
}
#endif
