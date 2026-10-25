#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

// Provider exports live in the embedded runtime object in every consumer.
// Loading this optional sidecar alone does not initialize hooks.
BOOL WINAPI DllMain(HINSTANCE module,DWORD reason,LPVOID) {
    if(reason==DLL_PROCESS_ATTACH) DisableThreadLibraryCalls(module);
    return TRUE;
}
#endif
