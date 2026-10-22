#pragma once

// C ABI hook multiplexer. The implementation (including MinHook) is embedded
// into each consumer DLL. A per-process shared API table elects one embedded
// copy as owner and routes all other copies through it.
#ifdef _WIN32
#  define BDS_HOOK_API
#  define BDS_HOOK_CALL __fastcall
#else
#  define BDS_HOOK_API
#  define BDS_HOOK_CALL
#endif

#include <stdint.h>
#ifdef __cplusplus
#include <optional>
#include <bedrock/resolver.hpp>
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef void* BDS_HookToken;
typedef enum BDS_HookPoint {
    BDS_HOOK_KEYBOARD_FEED = 1,
    BDS_HOOK_MOUSE_FEED = 2,
    BDS_HOOK_GRAB_CURSOR = 3,
    BDS_HOOK_RELEASE_CURSOR = 4,
    BDS_HOOK_GET_FOV = 5,
    BDS_HOOK_LEVEL_SUB_TICK = 6
} BDS_HookPoint;

typedef struct BDS_KeyboardArgs { int key; int down; int result; } BDS_KeyboardArgs;
typedef struct BDS_MouseArgs {
    void* self; int8_t action; int8_t state; int16_t x; int16_t y;
    int16_t dx; int16_t dy; int force_motionless_pointer;
} BDS_MouseArgs;
typedef struct BDS_CursorArgs { void* self; } BDS_CursorArgs;
typedef struct BDS_FovArgs { void* self; float input_fov; void* a3; void* a4; float result; } BDS_FovArgs;
typedef struct BDS_LevelArgs { void* level; } BDS_LevelArgs;

typedef uintptr_t (BDS_HOOK_CALL *BDS_HookNext)(void* frame, void* args);
typedef void (BDS_HOOK_CALL *BDS_HookCallback)(void* user, void* args,
                                                BDS_HookNext next, void* frame);

// target must be the verified address for point in the current Minecraft
// process. Re-registering the same point/address adds a middleware subscriber;
// a conflicting address is rejected. Unregister waits for in-flight callbacks.
BDS_HOOK_API int BDS_HOOK_CALL BDS_HookSubscribe(
    BDS_HookPoint point, void* target, BDS_HookCallback callback,
    void* user, BDS_HookToken* token_out);
BDS_HOOK_API void BDS_HOOK_CALL BDS_HookUnsubscribe(BDS_HookToken token);
BDS_HOOK_API int BDS_HOOK_CALL BDS_HookOwnsTarget(BDS_HookPoint point, void* target);
BDS_HOOK_API uintptr_t BDS_HOOK_CALL BDS_HookDispatch(BDS_HookPoint point, void* args);
BDS_HOOK_API uintptr_t BDS_HOOK_CALL BDS_HookCallOriginal(BDS_HookPoint point, void* args);
BDS_HOOK_API const char* BDS_HOOK_CALL BDS_HookRuntimeVersion(void);

// Escape hatch for hook points not covered by the middleware API. This still
// uses the SDK's single process-wide MinHook owner and rejects targets already
// owned by another direct hook or a middleware point.
BDS_HOOK_API int BDS_HOOK_CALL BDS_HookDirectInstall(
    void* target, void* detour, void** original_out, BDS_HookToken* token_out);
BDS_HOOK_API int BDS_HOOK_CALL BDS_HookDirectRemove(BDS_HookToken token);

#ifdef __cplusplus
}
#endif

#ifdef __cplusplus
namespace bedrock {
// Accept a guarded profile entry normally. If the guard is already replaced,
// accept it only when this runtime confirms that it owns the exact target.
inline std::optional<Resolution> resolveSharedHook(
    const Resolver& resolver, std::string_view symbol, BDS_HookPoint point) {
    if (auto resolved = resolver.resolve(symbol)) return resolved;
    auto candidate = resolver.resolveVerifiedRvaForSharedHook(symbol);
    if (candidate && BDS_HookOwnsTarget(
            point, reinterpret_cast<void*>(candidate->address))) return candidate;
    return std::nullopt;
}
} // namespace bedrock
#endif
