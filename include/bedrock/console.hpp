#pragma once

// Optional, header-only console utility for an injected Windows DLL.
// It NEVER opens automatically. Call open() only after DllMain/loader lock.
#include <mutex>
#include <string>
#include <string_view>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace bedrock {

class Console final {
public:
    // Attach to an existing process console, or allocate a new one.
    // If an existing console is detected, it is *never owned* by this utility.
    static bool open(std::wstring_view title = L"Bedrock Debug") noexcept {
#ifdef _WIN32
        std::lock_guard<std::mutex> lock(mutex());
        if (GetConsoleWindow() != nullptr) return true;
        if (!AllocConsole()) return GetConsoleWindow() != nullptr;
        owned() = true;
        SetConsoleOutputCP(CP_UTF8);
        SetConsoleCP(CP_UTF8);
        if (!title.empty()) {
            try {
                const std::wstring name(title);
                SetConsoleTitleW(name.c_str());
            } catch (...) {
                // Console was created, but the optional title allocation failed.
            }
        }
        return true;
#else
        (void)title;
        return false;
#endif
    }

    // Only detach/free a console created by this DLL. Do not steal Minecraft's
    // or another module's parent console.
    static void close() noexcept {
#ifdef _WIN32
        std::lock_guard<std::mutex> lock(mutex());
        if (owned()) {
            FreeConsole();
            owned() = false;
        }
#endif
    }

    static bool isOpen() noexcept {
#ifdef _WIN32
        return GetConsoleWindow() != nullptr;
#else
        return false;
#endif
    }

    static void write(std::string_view utf8Text) noexcept {
#ifdef _WIN32
        if (utf8Text.empty()) return;
        std::lock_guard<std::mutex> lock(mutex());
        if (GetConsoleWindow() == nullptr) return;
        // Open a dedicated console output handle; never replace the host game's
        // stdin/stdout/stderr or call freopen/SetStdHandle on its behalf.
        HANDLE output = CreateFileW(L"CONOUT$", GENERIC_WRITE,
                                    FILE_SHARE_READ | FILE_SHARE_WRITE,
                                    nullptr, OPEN_EXISTING, 0, nullptr);
        if (output == INVALID_HANDLE_VALUE) return;
        constexpr std::size_t chunk = 16 * 1024;
        for (std::size_t offset = 0; offset < utf8Text.size();) {
            const DWORD count = static_cast<DWORD>(
                (utf8Text.size() - offset < chunk) ?
                utf8Text.size() - offset : chunk);
            DWORD written = 0;
            if (!WriteFile(output, utf8Text.data() + offset, count, &written, nullptr) ||
                written == 0) break;
            offset += written;
        }
        CloseHandle(output);
#else
        (void)utf8Text;
#endif
    }

private:
    static std::mutex& mutex() noexcept {
        static std::mutex instance;
        return instance;
    }
    static bool& owned() noexcept {
        static bool createdByThisModule = false;
        return createdByThisModule;
    }
};

} // namespace bedrock
