#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <MinHook.h>
#include <bedrock/hook_runtime.h>

#include <array>
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
using LevelFn = void(__fastcall*)(void*);

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
std::array<State, 7> states{};
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
constexpr DWORD kSharedAbiVersion = 2;
HANDLE sharedMapping{};
SharedApi* sharedApi{};
std::mutex sharedApiMutex;

SharedApi* getSharedApi() {
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
        MemoryBarrier();
        InterlockedExchange(&sharedApi->state, 2);
    } else {
        for (unsigned i = 0; i < 500; ++i) {
            const auto state = InterlockedCompareExchange(&sharedApi->state, 0, 0);
            if (state != 0 && state != 1) break;
            Sleep(10);
        }
        if (InterlockedCompareExchange(&sharedApi->state, 0, 0) != 2 ||
            sharedApi->abiVersion != kSharedAbiVersion ||
            sharedApi->ownerPid != GetCurrentProcessId()) {
            UnmapViewOfFile(sharedApi); sharedApi = nullptr;
            CloseHandle(sharedMapping); sharedMapping = nullptr;
            return nullptr;
        }
    }
    return sharedApi;
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
    case BDS_HOOK_KEYBOARD_FEED: {
        auto& a = *static_cast<BDS_KeyboardArgs*>(args);
        a.result = original ? static_cast<KeyboardFn>(original)(a.key, a.down != 0) : 0;
        return static_cast<uintptr_t>(a.result);
    }
    case BDS_HOOK_MOUSE_FEED: {
        auto& a = *static_cast<BDS_MouseArgs*>(args);
        if (original) static_cast<MouseFn>(original)(a.self, a.action, a.state,
            a.x, a.y, a.dx, a.dy, a.force_motionless_pointer != 0);
        return 0;
    }
    case BDS_HOOK_GRAB_CURSOR:
    case BDS_HOOK_RELEASE_CURSOR: {
        auto& a = *static_cast<BDS_CursorArgs*>(args);
        if (original) static_cast<CursorFn>(original)(a.self);
        return 0;
    }
    case BDS_HOOK_GET_FOV: {
        auto& a = *static_cast<BDS_FovArgs*>(args);
        a.result = original ? static_cast<FovFn>(original)(a.self, a.input_fov, a.a3, a.a4)
                            : a.input_fov;
        return 0;
    }
    case BDS_HOOK_LEVEL_SUB_TICK: {
        auto& a = *static_cast<BDS_LevelArgs*>(args);
        if (original) static_cast<LevelFn>(original)(a.level);
        return 0;
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

void* detourFor(BDS_HookPoint point) {
    switch (point) {
    case BDS_HOOK_KEYBOARD_FEED: return reinterpret_cast<void*>(&keyboardDetour);
    case BDS_HOOK_MOUSE_FEED: return reinterpret_cast<void*>(&mouseDetour);
    case BDS_HOOK_GRAB_CURSOR: return reinterpret_cast<void*>(&grabDetour);
    case BDS_HOOK_RELEASE_CURSOR: return reinterpret_cast<void*>(&releaseDetour);
    case BDS_HOOK_GET_FOV: return reinterpret_cast<void*>(&fovDetour);
    case BDS_HOOK_LEVEL_SUB_TICK: return reinterpret_cast<void*>(&levelDetour);
    default: return nullptr;
    }
}
bool validPoint(BDS_HookPoint point) {
    return point >= BDS_HOOK_KEYBOARD_FEED && point <= BDS_HOOK_LEVEL_SUB_TICK;
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
    return point == BDS_HOOK_KEYBOARD_FEED
        ? static_cast<uintptr_t>(static_cast<BDS_KeyboardArgs*>(args)->result) : 0;
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
        for (unsigned point = BDS_HOOK_KEYBOARD_FEED; point <= BDS_HOOK_LEVEL_SUB_TICK; ++point) {
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

const char* BDS_HOOK_CALL leaderVersion() { return "2.0.0"; }

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

extern "C" int BDS_HOOK_CALL BDS_HookSubscribe(BDS_HookPoint point, void* target,
    BDS_HookCallback callback, void* user, BDS_HookToken* tokenOut) {
    auto* api = getSharedApi(); return api && api->subscribe
        ? api->subscribe(point, target, callback, user, tokenOut) : 0;
}
extern "C" void BDS_HOOK_CALL BDS_HookUnsubscribe(BDS_HookToken token) {
    auto* api = getSharedApi(); if (api && api->unsubscribe) api->unsubscribe(token);
}
extern "C" int BDS_HOOK_CALL BDS_HookOwnsTarget(BDS_HookPoint point, void* target) {
    auto* api = getSharedApi(); return api && api->owns ? api->owns(point, target) : 0;
}
extern "C" uintptr_t BDS_HOOK_CALL BDS_HookDispatch(BDS_HookPoint point, void* args) {
    auto* api = getSharedApi(); return api && api->dispatch ? api->dispatch(point, args) : 0;
}
extern "C" uintptr_t BDS_HOOK_CALL BDS_HookCallOriginal(BDS_HookPoint point, void* args) {
    auto* api = getSharedApi(); return api && api->original ? api->original(point, args) : 0;
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
