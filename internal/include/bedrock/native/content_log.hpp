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
#include <cstdio>
#include <string>
#include <string_view>

namespace bedrock::native {

// Bridges the SDK logger to the game's own Content Log write path. The call
// target and its ABI are defined by the version profile, never by this file.
// Resolution is fail-closed: if the exact-build symbol is missing, has no RVA,
// or its entry guard does not match, nothing is installed and the existing
// file/debug sinks remain the log surface.
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
                error = "Content Log write symbol is not defined in the exact-build profile";
                return false;
            }
            const auto& def = *it;
            if (def.value("rva", std::string{}).empty() ||
                def.value("entry_check", std::string{}).empty()) {
                error = "Content Log write symbol has no exact-build RVA/guard (unresolved)";
                return false;
            }
            void* target = native_profile::checked(root, def);
            if (!target) {
                error = "Content Log write symbol guard mismatch";
                return false;
            }
            const auto callName = def.value("call", "level_area_message");
            call_.store(callName == "message_level_area" ? Call::MessageLevelArea
                        : callName == "level_area_stringref" ? Call::LevelAreaStringRef
                        : Call::LevelAreaMessage,
                        std::memory_order_relaxed);
            area_.store(def.value("area", 45u), std::memory_order_relaxed);
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

        const int gameLevel = mapLevel(level);
        // Serialise across DLLs: every client module embeds its own copy of the
        // SDK runtime, so a module-local mutex would not protect the shared
        // game function when DynamicClient and ScriptClient log concurrently.
        // Process-lifetime named mutex: one handle, no per-line allocation.
        static HANDLE mutex = ::CreateMutexW(nullptr, FALSE, L"Local\\BedrockSdkContentLog");
        if (mutex) ::WaitForSingleObject(mutex, INFINITE);
        __try {
            const auto call = self->call_.load(std::memory_order_relaxed);
            if (call == Call::LevelAreaStringRef) {
                // The game's public log dispatch (profile symbol
                // ContentLog::write, 1.26.65.1): wrapped level/area
                // values plus the message as a std::string&. The
                // callee only reads the string, so a caller-owned
                // std::string is safe; no game allocator is involved.
                struct LevelWrap { std::uint16_t value; std::uint8_t hasValue; std::uint8_t pad; };
                struct AreaWrap { std::uint32_t value; std::uint8_t hasValue; std::uint8_t pad[3]; };
                const std::string text(buffer, static_cast<std::size_t>(length));
                const LevelWrap level{static_cast<std::uint16_t>(gameLevel), 1, 0};
                const AreaWrap area{self->area_.load(std::memory_order_relaxed), 1, {0, 0, 0}};
                using Fn = void(__fastcall*)(const LevelWrap*, const AreaWrap*, const void*);
                reinterpret_cast<Fn>(target)(&level, &area, &text);
            } else if (call == Call::MessageLevelArea) {
                using Fn = void(__fastcall*)(const char*, int, const char*);
                reinterpret_cast<Fn>(target)(buffer, gameLevel, kArea);
            } else {
                using Fn = void(__fastcall*)(int, const char*, const char*);
                reinterpret_cast<Fn>(target)(gameLevel, kArea, buffer);
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            // Never propagate a native fault from a logging call.
            std::atomic_signal_fence(std::memory_order_seq_cst);
        }
        if (mutex) ::ReleaseMutex(mutex);
    }

private:
    static constexpr const char* kSymbol = "ContentLog::write";
    static constexpr const char* kArea = "ContentLog";
    enum class Call : unsigned char { LevelAreaMessage, MessageLevelArea, LevelAreaStringRef };

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
    std::atomic<Call> call_{Call::LevelAreaMessage};
    std::atomic<std::uint32_t> area_{45};
};

} // namespace bedrock::native
