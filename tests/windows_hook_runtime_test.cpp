#include <bedrock/hook_runtime.h>
#include <cassert>
#include <string>

#include <windows.h>

namespace {
int firstCalls{};
int secondCalls{};
int actorNameTagCalls{};
__declspec(noinline) const std::string& __fastcall actorNameTagTarget(void*) {
    static const std::string name = "\xC2\xA7" "aSampleActor";
    return name;
}
__declspec(noinline) bool __fastcall target(int key, bool down) {
    volatile int preservePatchRoom = key + (down ? 3 : 4);
    return preservePatchRoom > 0;
}
__declspec(noinline) int directOriginal(int value) {
    volatile int preservePatchRoom = value;
    return preservePatchRoom + 1;
}
using DirectFn = int(*)(int);
DirectFn directTrampoline{};
int directDetour(int value) { return directTrampoline(value) + 10; }
void BDS_HOOK_CALL first(void*, void* raw, BDS_HookNext next, void* frame) {
    ++firstCalls; next(frame, raw); next(frame, raw);
}
void BDS_HOOK_CALL second(void*, void* raw, BDS_HookNext next, void* frame) {
    ++secondCalls; next(frame, raw);
}
void BDS_HOOK_CALL actorNameTagSubscriber(void*, void* raw,
    BDS_HookNext next, void* frame) {
    next(frame, raw);
    const auto& args = *static_cast<BDS_ActorNameTagArgs*>(raw);
    assert(args.actor != nullptr && args.name_tag != nullptr);
    assert(*static_cast<const std::string*>(args.name_tag) == "\xC2\xA7" "aSampleActor");
    ++actorNameTagCalls;
}
}

int main() {
    const auto clientA = LoadLibraryW(L"windows_hook_runtime_client_a.dll");
    const auto clientB = LoadLibraryW(L"windows_hook_runtime_client_b.dll");
    assert(clientA && clientB);
    using Subscribe = int(*)(void*, BDS_HookCallback, BDS_HookToken*);
    using Owns = int(*)(void*);
    using Unsubscribe = void(*)(BDS_HookToken);
    using DirectInstall = int(*)(void*, void*, void**, BDS_HookToken*);
    using SubscribeActorTag = int(*)(void*, BDS_HookCallback, BDS_HookToken*);
    const auto subscribeA = reinterpret_cast<Subscribe>(GetProcAddress(clientA, "ClientA_Subscribe"));
    const auto subscribeB = reinterpret_cast<Subscribe>(GetProcAddress(clientB, "ClientB_Subscribe"));
    const auto ownsB = reinterpret_cast<Owns>(GetProcAddress(clientB, "ClientB_Owns"));
    const auto unsubscribeA = reinterpret_cast<Unsubscribe>(GetProcAddress(clientA, "ClientA_Unsubscribe"));
    const auto unsubscribeB = reinterpret_cast<Unsubscribe>(GetProcAddress(clientB, "ClientB_Unsubscribe"));
    const auto directInstall = reinterpret_cast<DirectInstall>(GetProcAddress(clientB, "ClientB_DirectInstall"));
    const auto directRemove = reinterpret_cast<int(*)(BDS_HookToken)>(
        GetProcAddress(clientA, "ClientA_DirectRemove"));
    const auto subscribeActorA = reinterpret_cast<SubscribeActorTag>(
        GetProcAddress(clientA, "ClientA_SubscribeActorTag"));
    const auto subscribeActorB = reinterpret_cast<SubscribeActorTag>(
        GetProcAddress(clientB, "ClientB_SubscribeActorTag"));
    assert(subscribeA && subscribeB && ownsB && unsubscribeA && unsubscribeB &&
        directInstall && directRemove && subscribeActorA && subscribeActorB);
    BDS_HookToken a{}, b{};
    auto* address = reinterpret_cast<void*>(&target);
    assert(subscribeA(address, first, &a));
    assert(ownsB(address));
    assert(subscribeB(address, second, &b));
    assert(target(7, true));
    assert(firstCalls == 1 && secondCalls == 2);
    unsubscribeA(a);
    firstCalls = secondCalls = 0;
    assert(target(7, true));
    assert(firstCalls == 0 && secondCalls == 1);
    unsubscribeB(b);

    BDS_HookToken actorA{}, actorB{};
    auto* actorTarget = reinterpret_cast<void*>(&actorNameTagTarget);
    assert(subscribeActorA(actorTarget, actorNameTagSubscriber, &actorA));
    assert(subscribeActorB(actorTarget, actorNameTagSubscriber, &actorB));
    assert(actorNameTagTarget(reinterpret_cast<void*>(0x1234)) == "\xC2\xA7" "aSampleActor");
    assert(actorNameTagCalls == 2);
    unsubscribeA(actorA);
    unsubscribeB(actorB);

    BDS_HookToken directToken{};
    assert(directInstall(reinterpret_cast<void*>(&directOriginal),
        reinterpret_cast<void*>(&directDetour),
        reinterpret_cast<void**>(&directTrampoline), &directToken));
    assert(directOriginal(1) == 12);
    assert(!subscribeA(reinterpret_cast<void*>(&directOriginal), first, &a));
    assert(directRemove(directToken));
    assert(directOriginal(1) == 2);
}
