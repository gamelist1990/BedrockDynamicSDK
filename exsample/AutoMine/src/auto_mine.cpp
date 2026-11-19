#include "auto_mine.hpp"

namespace automine::client {
AutoMineMod mod;

DWORD WINAPI start(void *) {
    HMODULE pinned{};
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                       reinterpret_cast<LPCWSTR>(&start), &pinned);
    mod.run();
    return 0;
}
} // namespace automine::client

extern "C" __declspec(dllexport) unsigned AutoMine_GetStatus() {
    return automine::client::mod.status.load();
}
extern "C" __declspec(dllexport) void AutoMine_Stop() {
    automine::client::mod.stop();
}
BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(instance);
        if (const auto thread =
                CreateThread(nullptr, 0, automine::client::start, nullptr, 0, nullptr))
            CloseHandle(thread);
    }
    return TRUE;
}
