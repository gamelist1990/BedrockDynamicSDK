#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include "bedrock/client.hpp"
#include "bedrock/remote_catalog.hpp"
#include <string>

namespace bedrock {
namespace {
bool sendSprint(bool pressed) noexcept {
    INPUT event{};
    event.type = INPUT_KEYBOARD;
    event.ki.wScan = 0x1D;  // standard Windows x86 left Ctrl scan code
    event.ki.dwFlags = KEYEVENTF_SCANCODE | (pressed ? 0 : KEYEVENTF_KEYUP);
    return SendInput(1, &event, sizeof(event)) == 1;
}
bool currentProcessForeground() noexcept {
    const HWND window = GetForegroundWindow();
    if (!window) return false;
    DWORD processId = 0;
    GetWindowThreadProcessId(window, &processId);
    return processId == GetCurrentProcessId();
}
bool gameplayCursorInvisible() noexcept {
    CURSORINFO cursor{};
    cursor.cbSize = sizeof(cursor);
    return GetCursorInfo(&cursor) && !(cursor.flags & CURSOR_SHOWING);
}
} // namespace

bool Input::down(Key key) const noexcept {
    return (GetAsyncKeyState(static_cast<int>(key)) & 0x8000) != 0;
}
bool Input::sneaking() const noexcept {
    return (GetAsyncKeyState(VK_LSHIFT) & 0x8000) ||
           (GetAsyncKeyState(VK_RSHIFT) & 0x8000);
}
void Input::sprint(bool wantSprint) noexcept {
    if (wantSprint) {
        if (!currentProcessForeground() || !gameplayCursorInvisible()) {
            release();
            return;
        }
        if (!ownSprint_ && !down(Key::Sprint) && sendSprint(true))
            ownSprint_ = true;
    } else {
        release();
    }
}
void Input::autoSprint() noexcept {
    sprint(shouldSprint(
        currentProcessForeground() && gameplayCursorInvisible(),
        forward(), sneaking()));
}
void Input::release() noexcept {
    if (!ownSprint_) return;
    // Clear our ownership even if SendInput failed so stop is bounded.
    ownSprint_ = false;
    sendSprint(false);
}

bool Client::focused() const noexcept { return currentProcessForeground(); }
bool Client::inGame() const noexcept {
    return currentProcessForeground() && gameplayCursorInvisible();
}

bool ModRuntime::start(TickHandler handler) noexcept {
    ModStatus expected = ModStatus::NotStarted;
    if (!handler || !status_.compare_exchange_strong(expected, ModStatus::Starting))
        return false;
    callback_ = handler;
    stopping_.store(false, std::memory_order_release);
    HANDLE thread = CreateThread(nullptr, 0,
        reinterpret_cast<LPTHREAD_START_ROUTINE>(&ModRuntime::threadEntry),
        this, 0, nullptr);
    if (!thread) {
        status_.store(ModStatus::Failed);
        return false;
    }
    CloseHandle(thread);
    return true;
}

unsigned long __stdcall ModRuntime::threadEntry(void* state) {
    static_cast<ModRuntime*>(state)->run();
    return 0;
}

void ModRuntime::run() noexcept {
    // Ensure callbacks and SDK code cannot be unloaded while worker is active.
    // This intentionally pins the DLL until process exit.
    HMODULE module{};
    if (!GetModuleHandleExW(
        GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
        reinterpret_cast<LPCWSTR>(&ModRuntime::threadEntry), &module)) {
        status_.store(ModStatus::Failed);
        return;
    }

    std::string json, error;
    ProfileChoice choice;
    if (stopping_.load() || !fetchSupportedProfile(json, choice, error)) {
        if (!error.empty())
            OutputDebugStringA(("Bedrock mod inactive: " + error + "\n").c_str());
        status_.store(stopping_.load() ? ModStatus::Stopped : ModStatus::Unsupported);
        return;
    }
    Resolver resolver;
    if (!resolver.loadJson(json, error) ||
        !resolver.attachWindowsMainModule(error) || !resolver.exactBuild()) {
        OutputDebugStringA(("Bedrock mod inactive: " + error + "\n").c_str());
        status_.store(ModStatus::Unsupported);
        return;
    }
    Client client(resolver, choice.version);
    status_.store(ModStatus::Running);
    OutputDebugStringA(("Bedrock mod ready: " + choice.version + "\n").c_str());
    try {
        while (!stopping_.load(std::memory_order_acquire)) {
            callback_(client);
            Sleep(20);
        }
    } catch (...) {
        OutputDebugStringA("Bedrock mod stopped: unhandled callback exception\n");
        status_.store(ModStatus::Failed);
        client.input().release();
        return;
    }
    client.input().release();
    status_.store(ModStatus::Stopped);
}
} // namespace bedrock
#endif
