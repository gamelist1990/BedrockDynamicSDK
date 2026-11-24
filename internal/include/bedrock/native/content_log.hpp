#pragma once
#if !defined(_WIN32) || !defined(_MSC_VER) || !defined(_WIN64)
#error Native content-log adapter requires Windows x64 MSVC.
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <bedrock/detail/native_profile_functions.hpp>
#include <bedrock/logging/content_log.hpp>
#include <bedrock/profiles/profile_manager.hpp>
#include <nlohmann/json.hpp>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>

namespace bedrock::native {

// Bridges the SDK logger to the game's Creator Content Log through the
// Creator listener front end (profile symbol ContentLog::emitCreator), the
// path proven to reach the Content Log GUI history and files. The call
// target and its ABI are defined by the version profile, never by this file.
// Resolution is fail-closed: if the exact-build symbol is missing, has no
// RVA, or its entry guard does not match, nothing is installed and the
// existing file/debug sinks remain the log surface.
class NativeContentLog {
public:
    static NativeContentLog& instance() {
        static NativeContentLog self;
        return self;
    }

    NativeContentLog() = default;
    NativeContentLog(const NativeContentLog&) = delete;
    NativeContentLog& operator=(const NativeContentLog&) = delete;

    bool load(const RuntimeProfile& profile, std::string& error) {
        try {
            const auto root = nlohmann::json::parse(profile.json());
            const auto& symbols = root.at("symbols");
            const auto it = symbols.find(kSymbol);
            if (it == symbols.end() || !it->contains("rva") || !it->contains("entry_check")) {
                error = "Content Log emit symbol is not defined in the exact-build profile";
                return false;
            }
            const auto& def = *it;
            if (def.value("rva", std::string{}).empty() ||
                def.value("entry_check", std::string{}).empty()) {
                error = "Content Log emit symbol has no exact-build RVA/guard (unresolved)";
                return false;
            }
            void* target = native_profile::checked(root, def);
            if (!target) {
                error = "Content Log emit symbol guard mismatch";
                return false;
            }
            const auto base = reinterpret_cast<std::uintptr_t>(::GetModuleHandleW(nullptr));
            const auto guardRva = native_profile::number(def.at("guard_rva"));
            const auto serviceRva = native_profile::number(def.at("service_rva"));
            const auto flags = def.at("flags").get<std::uint32_t>();
            const auto area = def.at("area").get<std::uint32_t>();
            guardAddr_.store(base + guardRva, std::memory_order_relaxed);
            serviceAddr_.store(base + serviceRva, std::memory_order_relaxed);
            flags_.store(flags, std::memory_order_relaxed);
            area_.store(area, std::memory_order_relaxed);
            target_.store(target, std::memory_order_release);
            error.clear();
            return true;
        } catch (const std::exception& e) {
            error = e.what();
            return false;
        }
    }

    void install() noexcept {
        if (!target_.load(std::memory_order_acquire)) return;
        bedrock::setContentLogWriter(&NativeContentLog::write, nullptr);
    }

    // Called from bedrock::Logger::log and from the ScriptClient LogSink.
    // No allocations beyond a small thread-local formatting buffer, and a
    // structured exception guard so a bad native ABI can never take down the
    // game process.
    static void write(LogLevel level, std::string_view tag, std::string_view message,
                      void*) noexcept {
        auto* self = &instance();
        void* target = self->target_.load(std::memory_order_acquire);
        if (!target) return;

        thread_local char buffer[1024];
        int length = std::snprintf(buffer, sizeof(buffer), "[%.*s] %.*s",
                                   static_cast<int>(tag.size()), tag.data(),
                                   static_cast<int>(message.size()), message.data());
        if (length < 0) return;
        if (static_cast<std::size_t>(length) >= sizeof(buffer)) length = sizeof(buffer) - 1;
        buffer[length] = '\0';

        // Serialise across DLLs: every client module embeds its own copy of the
        // SDK runtime, so a module-local mutex would not protect the shared
        // game function when DynamicClient and ScriptClient log concurrently.
        // Process-lifetime named mutex: one handle, no per-line allocation.
        static HANDLE mutex = ::CreateMutexW(nullptr, FALSE, L"Local\\BedrockSdkContentLog");
        if (mutex) ::WaitForSingleObject(mutex, INFINITE);
        // The native call lives in the SEH-isolated helper below: MSVC
        // forbids __try in any function that also holds C++ objects
        // requiring unwinding (C2712), so this function keeps none of
        // its own __try blocks and the helper stays POD-only. mapLevel's
        // sequential values are exactly the Creator severity ordinals
        // (0=verbose, 1=inform, 2=warning, 3=error).
        callCreatorEmit(target,
                        self->guardAddr_.load(std::memory_order_relaxed),
                        self->serviceAddr_.load(std::memory_order_relaxed),
                        self->flags_.load(std::memory_order_relaxed),
                        static_cast<std::uint32_t>(mapLevel(level)),
                        self->area_.load(std::memory_order_relaxed), buffer);
        if (mutex) ::ReleaseMutex(mutex);
    }

private:
    static constexpr const char* kSymbol = "ContentLog::emitCreator";

    // SEH-isolated call helper. It may not hold a C++ object that
    // requires unwinding (MSVC C2712); it takes POD arguments only
    // and never lets a native fault escape.
    //
    // Creator listener front end, printf-style with a fixed two-%s format:
    // the formatted line rides as data in the first argument and the second
    // fragment is empty. The service is borrowed from the game on every
    // call: the guard pointer must resolve to validity byte 1 and the
    // service pointer must be live with its enabled field (+0x18) equal
    // to 1, exactly the preconditions the game's own callsites run under.
    // Any failed check drops the line instead of calling into a dead
    // service.
    static void callCreatorEmit(void* target, std::uintptr_t guardAddr,
                                std::uintptr_t serviceAddr, std::uint32_t flags,
                                std::uint32_t severity, std::uint32_t area,
                                const char* text) noexcept {
        __try {
            const auto guardPtr = *reinterpret_cast<const std::uintptr_t*>(guardAddr);
            if (!guardPtr) return;
            if (*reinterpret_cast<const std::uint8_t*>(guardPtr) != 1) return;
            const auto service = *reinterpret_cast<const std::uintptr_t*>(serviceAddr);
            if (!service) return;
            if (*reinterpret_cast<const std::uint32_t*>(service + 0x18) != 1) return;
            const char* first = text;
            const char* second = "";
            using Fn = void(__fastcall*)(std::uint32_t, std::uint32_t, std::uint32_t,
                                         const char*, const char* const*, const char* const*);
            reinterpret_cast<Fn>(target)(flags, severity, area, "%s%s", &first, &second);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            std::atomic_signal_fence(std::memory_order_seq_cst);
        }
    }

    static int mapLevel(LogLevel level) noexcept {
        switch (level) {
            case LogLevel::Debug: return 0;
            case LogLevel::Info:  return 1;
            case LogLevel::Warn:  return 2;
            case LogLevel::Error: return 3;
            case LogLevel::Off:   return 1;
        }
        return 1;
    }

    std::atomic<void*> target_{nullptr};
    std::atomic<std::uint32_t> area_{0};
    std::atomic<std::uint32_t> flags_{0};
    std::atomic<std::uintptr_t> guardAddr_{0};
    std::atomic<std::uintptr_t> serviceAddr_{0};
};

} // namespace bedrock::native
