#if defined(_WIN32)
#include <MinHook.h>
#include "bedrock/client_instance_tracker.hpp"
#include <cassert>
#include <cstdio>
#include <windows.h>

static volatile LONG g_last = 0;
using Fn = int (__fastcall*)(void*);
static Fn g_trampoline = nullptr;
static bedrock::ClientInstanceTracker g_tracker;
extern "C" __declspec(noinline) int __fastcall targetMethod(void* self) {
    auto old = InterlockedIncrement(&g_last);
    // Enough independent instructions so this is a genuine MinHook trampoline.
    return static_cast<int>(old) + (self != nullptr ? 13 : 7);
}
static int __fastcall replacement(void* self) {
    g_tracker.observe(self);
    return g_trampoline(self);
}
int main() {
    int object = 42;
    assert(MH_Initialize() == MH_OK);
    assert(MH_CreateHook(reinterpret_cast<void*>(&targetMethod),
                          reinterpret_cast<void*>(&replacement),
                          reinterpret_cast<void**>(&g_trampoline)) == MH_OK);
    assert(MH_EnableHook(reinterpret_cast<void*>(&targetMethod)) == MH_OK);
    // Volatile function pointer prevents compiler directly folding the call.
    volatile Fn invoke = &targetMethod;
    assert(invoke(&object) >= 14);
    assert(g_tracker.get() == &object);
    assert(g_tracker.captures() == 1);
    assert(MH_DisableHook(reinterpret_cast<void*>(&targetMethod)) == MH_OK);
    assert(MH_RemoveHook(reinterpret_cast<void*>(&targetMethod)) == MH_OK);
    assert(MH_Uninitialize() == MH_OK);
    g_tracker.clear();
    assert(g_tracker.get() == nullptr);
    std::puts("windows_minhook_test: PASS");
}
#endif
