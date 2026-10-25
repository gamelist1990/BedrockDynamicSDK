#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <bedrock/hook_runtime.h>

// LoadLibrary alone has no hook side effects. Updated consumers call Start
// synchronously before creating their first shared owner. Load/start this DLL
// explicitly before legacy consumers to give them the preferred owner too.
extern "C" __declspec(dllexport) unsigned BedrockHookRuntime_BuildVersion() {return 0x00040200u;}
extern "C" __declspec(dllexport) unsigned BedrockHookRuntime_BuildAbi() {return 4;}
extern "C" __declspec(dllexport) int BedrockHookRuntime_Start() {
    return BDS_HookRuntimeAbi()!=0;
}
extern "C" __declspec(dllexport) const char* BedrockHookRuntime_Version() {
    return BDS_HookRuntimeVersion();
}
extern "C" __declspec(dllexport) int BedrockHookRuntime_Supports(unsigned point) {
    return BDS_HookRuntimeSupports(static_cast<BDS_HookPoint>(point));
}
BOOL WINAPI DllMain(HINSTANCE module,DWORD reason,LPVOID) {
    if(reason==DLL_PROCESS_ATTACH) DisableThreadLibraryCalls(module);
    return TRUE;
}
#endif
