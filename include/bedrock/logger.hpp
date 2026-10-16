#pragma once

// Small, opt-in, header-only logger. Works without Bedrock resolver or hooks.
// By default on Windows logs go to OutputDebugStringA, and to the console
// ONLY if one was explicitly opened by the application.
#include "bedrock/console.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <functional>
#include <mutex>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>

#ifdef _WIN32
#include <windows.h>
#endif

namespace bedrock {

enum class LogLevel : unsigned char { Debug = 0, Info = 1, Warn = 2, Error = 3, Off = 4 };

inline constexpr std::string_view logLevelName(LogLevel level) noexcept {
    switch (level) {
        case LogLevel::Debug: return "DEBUG";
        case LogLevel::Info:  return "INFO";
        case LogLevel::Warn:  return "WARN";
        case LogLevel::Error: return "ERROR";
        case LogLevel::Off:   return "OFF";
    }
    return "UNKNOWN";
}

class Logger final {
public:
    // Sink can be used to connect another logger or capture logs in unit tests.
    // When set, the caller owns its lifetime. The sink replaces default output.
    using Sink = void (*)(LogLevel, std::string_view line, void* userdata);

    explicit Logger(std::string tag = "Bedrock", LogLevel minimum = LogLevel::Debug)
        : tag_(std::move(tag)), minimum_(minimum) {}

    void setLevel(LogLevel level) noexcept { minimum_.store(level, std::memory_order_relaxed); }
    [[nodiscard]] LogLevel level() const noexcept {
        return minimum_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] std::string_view tag() const noexcept { return tag_; }

    void setSink(Sink sink, void* userdata = nullptr) noexcept {
        sinkUserdata_.store(userdata, std::memory_order_relaxed);
        sink_.store(sink, std::memory_order_release);
    }

    template <class... Args> void debug(Args&&... args) const {
        log(LogLevel::Debug, std::forward<Args>(args)...);
    }
    template <class... Args> void info(Args&&... args) const {
        log(LogLevel::Info, std::forward<Args>(args)...);
    }
    template <class... Args> void warn(Args&&... args) const {
        log(LogLevel::Warn, std::forward<Args>(args)...);
    }
    template <class... Args> void error(Args&&... args) const {
        log(LogLevel::Error, std::forward<Args>(args)...);
    }

    template <class... Args> void log(LogLevel level, Args&&... args) const {
        if (level < minimum_.load(std::memory_order_relaxed) ||
            level == LogLevel::Off) return;

        // Deferred formatting: filtered debug logs cause no string allocations.
        std::ostringstream message;
        (message << ... << std::forward<Args>(args));
        const std::string line = makeLine(level, tag_, message.str());

        if (auto sink = sink_.load(std::memory_order_acquire)) {
            sink(level, line, sinkUserdata_.load(std::memory_order_acquire));
            return;
        }
        defaultOutput(line);
    }

    static std::string makeLine(LogLevel level, std::string_view tag,
                                std::string_view message) {
        const auto now = std::chrono::system_clock::now();
        const auto time = std::chrono::system_clock::to_time_t(now);
        std::tm local{};
#ifdef _WIN32
        localtime_s(&local, &time);
#else
        localtime_r(&time, &local);
#endif
        char stamp[12]{};
        std::snprintf(stamp, sizeof(stamp), "%02d:%02d:%02d",
                      local.tm_hour, local.tm_min, local.tm_sec);
        std::string line;
        line.reserve(tag.size() + message.size() + 35);
        line += '[';
        line += stamp;
        line += "] [";
        line += logLevelName(level);
        line += "] [";
        line += tag;
        line += "] ";
        line += message;
        line += '\n';
        return line;
    }

private:
    static void defaultOutput(const std::string& line) noexcept {
        std::lock_guard<std::mutex> lock(outputMutex());
#ifdef _WIN32
        OutputDebugStringA(line.c_str());
        Console::write(line);
#else
        std::fwrite(line.data(), 1, line.size(), stderr);
        std::fflush(stderr);
#endif
    }
    static std::mutex& outputMutex() noexcept {
        static std::mutex instance;
        return instance;
    }

    std::string tag_;
    std::atomic<LogLevel> minimum_;
    std::atomic<Sink> sink_{nullptr};
    std::atomic<void*> sinkUserdata_{nullptr};
};

} // namespace bedrock
