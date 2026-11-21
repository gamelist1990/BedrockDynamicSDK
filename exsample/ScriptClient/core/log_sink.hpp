#pragma once

#include "common.hpp"
#include <bedrock/sdk/logging.hpp>
#include <fstream>
#include <mutex>

namespace stageb {

// TS console.log/info/warn/error levels as produced by ModuleLoader.
inline bedrock::sdk::LogLevel contentLogLevel(const char* level) noexcept {
    if (level && *level) {
        if (level[0] == 'e' || level[0] == 'E') return bedrock::sdk::LogLevel::Error;
        if (level[0] == 'w' || level[0] == 'W') return bedrock::sdk::LogLevel::Warn;
        if (level[0] == 'd' || level[0] == 'D') return bedrock::sdk::LogLevel::Debug;
    }
    return bedrock::sdk::LogLevel::Info;
}

// Per-Mod log (§1.2 LogSink): Documents/PEXData/<mod>/<mod>.log on the game
// side; tests can attach any std::ostream or use the in-memory lines.
class LogSink {
public:
    explicit LogSink(std::string modName) : modName_(std::move(modName)) {}

    bool open(const fs::path& path) {
        std::lock_guard lock(mutex_);
        stream_.open(path, std::ios::app);
        return stream_.is_open();
    }
    void write(const char* level, const std::string& message) {
        std::lock_guard lock(mutex_);
        const std::string line = std::string("[") + modName_ + "] [" + level + "] " + message;
        lines_.push_back(line);
        // Flush so a line survives a crash and is observable while the game
        // is still running (logs are read by mod developers live on BOL).
        if (stream_.is_open()) stream_ << line << '\n' << std::flush;
        // Primary surface: the mod's own console output goes to the vanilla
        // in-game Content Log too, with the mod name as the area/prefix. The
        // bridge is a no-op until the native core resolves the game function.
        bedrock::sdk::emitContentLog(contentLogLevel(level), modName_, message);
    }
    const std::vector<std::string>& lines() const { return lines_; }

private:
    std::string modName_;
    mutable std::mutex mutex_;
    std::ofstream stream_;
    std::vector<std::string> lines_;
};

} // namespace stageb
