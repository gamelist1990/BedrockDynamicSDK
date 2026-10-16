#ifdef _WIN32
// Opt-in, read-only DLL demo. No game-function invocation or memory modification.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "bedrock/resolver.hpp"
#include "bedrock/generated_symbols.hpp"
#include <cstdio>
#include <string>
#include <string_view>
#include <nlohmann/json.hpp>

namespace {
DWORD WINAPI inspectGame(void*) {
    bedrock::Resolver resolver;
    std::string error;
    if (!resolver.loadJson(bedrock::generated::kSymbolDatabase, error)) {
        OutputDebugStringA(("BedrockSymbolProbe: invalid database: " + error).c_str());
        return 1;
    }
    if (!resolver.attachWindowsMainModule(error)) {
        OutputDebugStringA(("BedrockSymbolProbe: PE image not recognized: " + error).c_str());
        return 2;
    }
    if (!resolver.exactBuild()) {
        OutputDebugStringA("BedrockSymbolProbe: unsupported EXE hash; no symbols accepted.");
        return 3;
    }
    auto data = nlohmann::json::parse(bedrock::generated::kSymbolDatabase);
    unsigned int found = 0;
    for (auto it = data["symbols"].begin(); it != data["symbols"].end(); ++it) {
        auto resolved = resolver.resolve(it.key(), true);
        if (resolved && resolved->exact_build) ++found;
        // resolve(..., true) is read-only. A match is NOT proof of callable ABI.
    }
    char buffer[160];
    std::snprintf(buffer, sizeof(buffer),
        "BedrockSymbolProbe: %u/%zu candidate RVAs match in loaded game (ABI not verified).",
        found, resolver.count());
    OutputDebugStringA(buffer);
    return 0;
}
} // namespace

extern "C" __declspec(dllexport) DWORD WINAPI BedrockProbe_Initialize(void*) {
    return inspectGame(nullptr);
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(instance);
        // Optional automatic smoke test, without heavy code inside DllMain.
        HANDLE thread = CreateThread(nullptr, 0, inspectGame, nullptr, 0, nullptr);
        if (thread) CloseHandle(thread);
    }
    return TRUE;
}
#endif
