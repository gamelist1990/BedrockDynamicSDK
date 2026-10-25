#include <bedrock/hook_runtime.h>

extern "C" __declspec(dllexport) int Client_SubscribePoint(BDS_HookPoint point,
    void* target, BDS_HookCallback callback, void* user, BDS_HookToken* token) {
    return BDS_HookSubscribe(point, target, callback, user, token);
}

extern "C" __declspec(dllexport) int ClientA_Subscribe(void* target,
    BDS_HookCallback callback, BDS_HookToken* token) {
    return BDS_HookSubscribe(BDS_HOOK_KEYBOARD_FEED, target, callback, nullptr, token);
}
extern "C" __declspec(dllexport) int ClientB_Subscribe(void* target,
    BDS_HookCallback callback, BDS_HookToken* token) {
    return BDS_HookSubscribe(BDS_HOOK_KEYBOARD_FEED, target, callback, nullptr, token);
}
extern "C" __declspec(dllexport) int ClientA_SubscribeActorTag(void* target,
    BDS_HookCallback callback, BDS_HookToken* token) {
    return BDS_HookSubscribe(BDS_HOOK_ACTOR_GET_NAME_TAG, target, callback, nullptr, token);
}
extern "C" __declspec(dllexport) int ClientB_SubscribeActorTag(void* target,
    BDS_HookCallback callback, BDS_HookToken* token) {
    return BDS_HookSubscribe(BDS_HOOK_ACTOR_GET_NAME_TAG, target, callback, nullptr, token);
}
extern "C" __declspec(dllexport) int ClientB_Owns(void* target) {
    return BDS_HookOwnsTarget(BDS_HOOK_KEYBOARD_FEED, target);
}
extern "C" __declspec(dllexport) void ClientA_Unsubscribe(BDS_HookToken token) {
    BDS_HookUnsubscribe(token);
}
extern "C" __declspec(dllexport) void ClientB_Unsubscribe(BDS_HookToken token) {
    BDS_HookUnsubscribe(token);
}
extern "C" __declspec(dllexport) int ClientB_DirectInstall(void* target,
    void* detour, void** original, BDS_HookToken* token) {
    return BDS_HookDirectInstall(target, detour, original, token);
}
extern "C" __declspec(dllexport) int ClientA_DirectRemove(BDS_HookToken token) {
    return BDS_HookDirectRemove(token);
}
