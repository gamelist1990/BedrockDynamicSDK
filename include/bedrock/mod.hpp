#pragma once
#include "bedrock/client.hpp"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

// The entire mod entrypoint can be:
//   #include <bedrock/mod.hpp>
//   BEDROCK_MOD([](bedrock::Client& client) { client.input().autoSprint(); });
//
// This macro defines one DLL entrypoint and exported lifecycle helpers; use once
// per DLL. Heavy initialization runs after the loader lock on a worker thread.
#define BEDROCK_MOD(...)                                                     \
    namespace {                                                              \
        ::bedrock::ModRuntime bedrock_module_runtime;                       \
        void bedrock_module_tick(::bedrock::Client& instance) {              \
            (__VA_ARGS__)(instance);                                         \
        }                                                                    \
    }                                                                        \
    extern "C" __declspec(dllexport) unsigned BedrockMod_GetStatus() {      \
        return static_cast<unsigned>(bedrock_module_runtime.status());       \
    }                                                                        \
    extern "C" __declspec(dllexport) void BedrockMod_Stop() {                \
        bedrock_module_runtime.stop();                                       \
    }                                                                        \
    BOOL WINAPI DllMain(HINSTANCE module, DWORD reason, LPVOID) {           \
        if (reason == DLL_PROCESS_ATTACH) {                                  \
            DisableThreadLibraryCalls(module);                              \
            bedrock_module_runtime.start(&bedrock_module_tick);             \
        } else if (reason == DLL_PROCESS_DETACH) {                           \
            bedrock_module_runtime.stop();                                   \
        }                                                                    \
        return TRUE;                                                         \
    }
#else
#define BEDROCK_MOD(...)
#endif
