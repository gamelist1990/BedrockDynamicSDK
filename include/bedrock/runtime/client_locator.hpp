#pragma once
#include <cstdint>

// C ABI exported by BedrockClientLocator.dll.
// This DLL is a standalone runtime module, not a process injector.
// ClientInstance* is non-owning and can become stale when the game reloads.
#ifdef _WIN32
#define BEDROCK_EXPORT extern "C" __declspec(dllexport)
#else
#define BEDROCK_EXPORT extern "C"
#endif

enum BedrockLocatorStatus : unsigned {
    LocatorNotStarted = 0,
    LocatorStarting = 1,
    LocatorWaitingForClientEvent = 2,
    LocatorCaptured = 3,
    LocatorUnsupportedBuild = 4,
    LocatorHookFailure = 5,
    LocatorStopped = 6,
};

BEDROCK_EXPORT unsigned BedrockLocator_Start();
BEDROCK_EXPORT unsigned BedrockLocator_Stop();
BEDROCK_EXPORT unsigned BedrockLocator_GetStatus();
BEDROCK_EXPORT void* BedrockLocator_GetClientInstance();
BEDROCK_EXPORT std::uint64_t BedrockLocator_CaptureCount();
