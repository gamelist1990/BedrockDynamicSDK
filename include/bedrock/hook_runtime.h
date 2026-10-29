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
    BDS_HOOK_LEVEL_SUB_TICK = 6,
    BDS_HOOK_ACTOR_GET_NAME_TAG = 7,
    BDS_HOOK_GAMECORE_KEYBOARD_EVENT = 8,
    BDS_HOOK_CLIENT_PLAYER_LOADED = 9,
    BDS_HOOK_CLIENT_LEVEL_EXIT = 10,
    BDS_HOOK_CLIENT_DIMENSION_CHANGED = 11,
    BDS_HOOK_CLIENT_JOIN_CANCELLED = 12,
    BDS_HOOK_RENDER_LEVEL = 13,
    BDS_HOOK_PACKET_SEND_TO_SERVER = 14,
    BDS_HOOK_LEVEL_RUNTIME_ACTOR_LIST = 15,
    BDS_HOOK_COMMAND_OUTPUT_READ = 16,
    BDS_HOOK_PACKET_WIRE_SEND = 17,
    BDS_HOOK_PACKET_READ = 18
} BDS_HookPoint;

typedef struct BDS_KeyboardArgs { int key; int down; int result; } BDS_KeyboardArgs;
typedef struct BDS_MouseArgs {
    void* self; int8_t action; int8_t state; int16_t x; int16_t y;
    int16_t dx; int16_t dy; int force_motionless_pointer;
} BDS_MouseArgs;
#define BDS_MOUSE_FLAG_FORCE_MOTIONLESS 0x1
#define BDS_MOUSE_FLAG_SYNTHETIC 0x40000000
typedef struct BDS_CursorArgs { void* self; } BDS_CursorArgs;
typedef struct BDS_FovArgs { void* self; float input_fov; void* a3; void* a4; float result; } BDS_FovArgs;
typedef struct BDS_PacketSendArgs { void* sender; void* packet; } BDS_PacketSendArgs;
// Before batch length-prefixing/compression. bytes is a borrowed const MSVC
// std::string containing the packet header and payload. Replace the pointer
// only for the synchronous continuation; the game copies its contents.
typedef struct BDS_PacketWireArgs {
    void* stage; const void* bytes; uintptr_t a3; int flags; void* sender;
} BDS_PacketWireArgs;
// CommandOutputPacket slot 16: hidden Result<void>* return, stream, context.
typedef struct BDS_CommandOutputReadArgs {
    void* packet; void* output; void* stream; void* context; void* result;
} BDS_CommandOutputReadArgs;
// Common Packet::read: hidden Result<void>*, ReadOnlyBinaryStream*, context.
typedef BDS_CommandOutputReadArgs BDS_PacketReadArgs;
typedef struct BDS_RenderLevelArgs { void* renderer; void* screen; void* context; } BDS_RenderLevelArgs;
typedef struct BDS_LevelArgs { void* level; } BDS_LevelArgs;
// output points to the MSVC x64 std::vector<Actor*> hidden return buffer.
typedef struct BDS_RuntimeActorListArgs { void* level; void* output; } BDS_RuntimeActorListArgs;
typedef struct BDS_ClientPlayerLoadedArgs { void* self; void* player; } BDS_ClientPlayerLoadedArgs;
// For LEVEL_EXIT, self is the GameCallbacks subobject, not ClientInstance.
// Normalize it using the exact-build native_world_events profile layout.
typedef struct BDS_ClientWorldArgs { void* self; } BDS_ClientWorldArgs;
typedef struct BDS_ActorNameTagArgs { void* actor; const void* name_tag; } BDS_ActorNameTagArgs;
typedef struct BDS_GameCoreKeyboardEventArgs {
    void* self; void* output; void* context; void* result;
} BDS_GameCoreKeyboardEventArgs;

typedef uintptr_t (BDS_HOOK_CALL *BDS_HookNext)(void* frame, void* args);
typedef void (BDS_HOOK_CALL *BDS_HookCallback)(void* user, void* args,
                                                BDS_HookNext next, void* frame);

// target must be the exact-profile address for point in the current Minecraft
// process, with its entry guard and function boundary checked. Re-registering
// the same point/address adds a middleware subscriber;
// a conflicting address is rejected. Unregister waits for in-flight callbacks.
BDS_HOOK_API int BDS_HOOK_CALL BDS_HookSubscribe(
    BDS_HookPoint point, void* target, BDS_HookCallback callback,
    void* user, BDS_HookToken* token_out);
BDS_HOOK_API void BDS_HOOK_CALL BDS_HookUnsubscribe(BDS_HookToken token);
BDS_HOOK_API int BDS_HOOK_CALL BDS_HookOwnsTarget(BDS_HookPoint point, void* target);
BDS_HOOK_API uintptr_t BDS_HOOK_CALL BDS_HookDispatch(BDS_HookPoint point, void* args);
BDS_HOOK_API uintptr_t BDS_HOOK_CALL BDS_HookCallOriginal(BDS_HookPoint point, void* args);
BDS_HOOK_API const char* BDS_HOOK_CALL BDS_HookRuntimeVersion(void);
// Returns owner capabilities, not the version embedded in the calling DLL.
BDS_HOOK_API int BDS_HOOK_CALL BDS_HookRuntimeSupports(BDS_HookPoint point);
BDS_HOOK_API unsigned BDS_HOOK_CALL BDS_HookRuntimeAbi(void);
// Initializes discovery if needed; UTF-8 snapshot valid until this thread's
// next diagnostic call. No disk logging or background thread is created.
BDS_HOOK_API const char* BDS_HOOK_CALL BDS_HookRuntimeDiagnostic(void);


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
    // Exact-build candidate entries can be used only when their recorded RVA
    // still passes the profile's entry guard. Never promote an AOB-only hit
    // to a hook target: it may be a different function with a similar body.
    if (auto guarded = resolver.resolve(symbol, true);
        guarded && guarded->exact_build && guarded->method == "exact-rva") {
        guarded->method = "guarded-exact-rva";
        return guarded;
    }
    // Another embedded SDK copy may already have patched the prologue. Reuse
    // that exact profile RVA only when our broker owns the target and point.
    auto candidate = resolver.resolveRvaForSharedHook(symbol);
    if (candidate && BDS_HookOwnsTarget(
            point, reinterpret_cast<void*>(candidate->address))) return candidate;
    return std::nullopt;
}

inline bool isUsableSharedHookResolution(const Resolution& resolution) noexcept {
    return resolution.exact_build &&
        (resolution.verified || resolution.method == "guarded-exact-rva" ||
         resolution.method == "shared-hook-exact-rva");
}
} // namespace bedrock
#endif
