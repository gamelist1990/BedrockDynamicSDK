#include <bedrock/sdk/window_file_drop.hpp>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>

#include <atomic>
#include <mutex>
#include <utility>

namespace {
namespace fs = std::filesystem;
constexpr UINT_PTR subclassId = 1; // Scoped to the procedure address.
constexpr UINT maxPending = 32;
std::atomic<HWND> attached{}, attaching{};
std::mutex pendingMutex;
std::vector<fs::path> pending;

LRESULT CALLBACK dropProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam,
                          UINT_PTR id, DWORD_PTR) noexcept {
    if (message == WM_DROPFILES) {
        const auto drop = reinterpret_cast<HDROP>(wParam);
        try {
            const auto count = DragQueryFileW(drop, 0xffffffffu, nullptr, 0);
            std::lock_guard lock(pendingMutex);
            for (UINT i = 0; i < count && i < maxPending && pending.size() < maxPending; ++i) {
                const auto length = DragQueryFileW(drop, i, nullptr, 0);
                if (!length || length > 32767) continue;
                std::wstring path(length + 1, L'\0');
                if (DragQueryFileW(drop, i, path.data(), length + 1) != length) continue;
                path.resize(length);
                pending.emplace_back(std::move(path));
            }
        } catch (...) {} // No exception may cross the window procedure.
        DragFinish(drop);
        return 0;
    }
    if (message == WM_NCDESTROY) {
        RemoveWindowSubclass(window, dropProc, id);
        attached.store(nullptr);
    }
    return DefSubclassProc(window, message, wParam, lParam);
}

void attach(HWND window) {
    if (SetWindowSubclass(window, dropProc, subclassId, 0)) {
        DragAcceptFiles(window, TRUE);
        attached.store(window);
    }
}

LRESULT CALLBACK onWindowThread(int code, WPARAM wParam, LPARAM lParam) noexcept {
    if (code >= 0) {
        const auto& call = *reinterpret_cast<const CWPSTRUCT*>(lParam);
        if (call.message == WM_NULL && call.hwnd == attaching.load() && call.hwnd != attached.load())
            attach(call.hwnd);
    }
    return CallNextHookEx(nullptr, code, wParam, lParam);
}

struct Candidate {
    HWND window{};
    long long area{};
};

BOOL CALLBACK findGameWindow(HWND window, LPARAM data) noexcept {
    DWORD pid{};
    GetWindowThreadProcessId(window, &pid);
    if (pid != GetCurrentProcessId() || !IsWindowVisible(window) ||
        GetWindow(window, GW_OWNER) || (GetWindowLongPtrW(window, GWL_EXSTYLE) & WS_EX_TOOLWINDOW))
        return TRUE;
    RECT client{};
    if (!GetClientRect(window, &client) || client.right < 320 || client.bottom < 200) return TRUE;
    auto& best = *reinterpret_cast<Candidate*>(data);
    const auto area = static_cast<long long>(client.right) * client.bottom;
    if (area > best.area) best = {window, area};
    return TRUE;
}

void ensureAttached() {
    if (attached.load()) return;
    Candidate best;
    EnumWindows(findGameWindow, reinterpret_cast<LPARAM>(&best));
    if (!best.window) return;
    DWORD pid{};
    const auto thread = GetWindowThreadProcessId(best.window, &pid);
    if (!thread || pid != GetCurrentProcessId()) return;

    // The subclass outlives the caller. Pin its code before registering any
    // callback, and install SetWindowSubclass on the owning thread.
    static const bool pinned = [] {
        HMODULE module{};
        return GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                                  reinterpret_cast<LPCWSTR>(&dropProc), &module) != FALSE;
    }();
    if (!pinned) return;
    if (thread == GetCurrentThreadId()) {
        attach(best.window);
        return;
    }
    const auto hook = SetWindowsHookExW(WH_CALLWNDPROC, onWindowThread, nullptr, thread);
    if (!hook) return;
    attaching.store(best.window);
    SendMessageTimeoutW(best.window, WM_NULL, 0, 0, SMTO_ABORTIFHUNG | SMTO_BLOCK, 200, nullptr);
    attaching.store(nullptr);
    UnhookWindowsHookEx(hook);
}
} // namespace
#endif

namespace bedrock::sdk {
std::vector<std::filesystem::path> pollWindowFileDrops() {
#ifdef _WIN32
    ensureAttached();
    std::lock_guard lock(pendingMutex);
    return std::exchange(pending, {});
#else
    return {};
#endif
}
} // namespace bedrock::sdk
