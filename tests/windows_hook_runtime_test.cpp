#include <bedrock/hook_runtime.h>
#include <cassert>
#include <string>

#include <windows.h>

namespace {
int firstCalls{};
int secondCalls{};
int actorNameTagCalls{};
unsigned worldOriginalCalls{};
void* worldThis{};
void* worldPlayer{};
__declspec(noinline) void __fastcall joinedTarget(void* self, void* player) {
    volatile auto room = reinterpret_cast<std::uintptr_t>(self);
    worldThis = reinterpret_cast<void*>(room);
    worldPlayer = player;
    ++worldOriginalCalls;
}
__declspec(noinline) void __fastcall exitedTarget(void* self) {
    volatile auto room = reinterpret_cast<std::uintptr_t>(self);
    worldThis = reinterpret_cast<void*>(room);
    ++worldOriginalCalls;
}
__declspec(noinline) void __fastcall dimensionTarget(void* self) {
    volatile auto room = reinterpret_cast<std::uintptr_t>(self);
    worldThis = reinterpret_cast<void*>(room);
    worldPlayer = nullptr;
    ++worldOriginalCalls;
}
__declspec(noinline) void __fastcall cancelledTarget(void* self) {
    volatile auto room = reinterpret_cast<std::uintptr_t>(self);
    worldThis = reinterpret_cast<void*>(room);
    worldPlayer = self;
    ++worldOriginalCalls;
}
void BDS_HOOK_CALL worldSubscriber(void* user, void* raw, BDS_HookNext next, void* frame) {
    ++*static_cast<unsigned*>(user);
    next(frame, raw);
}
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

    using SubscribePoint = int(*)(BDS_HookPoint, void*, BDS_HookCallback, void*, BDS_HookToken*);
    const auto pointA = reinterpret_cast<SubscribePoint>(GetProcAddress(clientA, "Client_SubscribePoint"));
    const auto pointB = reinterpret_cast<SubscribePoint>(GetProcAddress(clientB, "Client_SubscribePoint"));
    assert(pointA && pointB);
    void* const self = reinterpret_cast<void*>(0x2000);
    void* const player = reinterpret_cast<void*>(0x3000);
    const BDS_HookPoint worldPoints[]{BDS_HOOK_CLIENT_PLAYER_LOADED, BDS_HOOK_CLIENT_LEVEL_EXIT,
        BDS_HOOK_CLIENT_DIMENSION_CHANGED, BDS_HOOK_CLIENT_JOIN_CANCELLED};
    void* const worldTargets[]{reinterpret_cast<void*>(&joinedTarget),
        reinterpret_cast<void*>(&exitedTarget), reinterpret_cast<void*>(&dimensionTarget),
        reinterpret_cast<void*>(&cancelledTarget)};
    for (unsigned i = 0; i != 4; ++i) {
        unsigned observers{};
        BDS_HookToken wa{}, wb{};
        assert(pointA(worldPoints[i], worldTargets[i], worldSubscriber, &observers, &wa));
        assert(pointB(worldPoints[i], worldTargets[i], worldSubscriber, &observers, &wb));
        worldOriginalCalls = 0;
        if (i == 0) joinedTarget(self, player);
        else if (i == 1) exitedTarget(self);
        else if (i == 2) dimensionTarget(self);
        else cancelledTarget(self);
        assert(observers == 2 && worldOriginalCalls == 1 && worldThis == self);
        if (i == 0) assert(worldPlayer == player);
        unsubscribeA(wa);
        unsubscribeB(wb);
    }
}
