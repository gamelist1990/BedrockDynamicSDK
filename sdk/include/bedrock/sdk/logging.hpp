#pragma once
#include <filesystem>
#include <memory>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
namespace bedrock::sdk {
enum class LogLevel : unsigned char { Debug=0, Info=1, Warn=2, Error=3, Off=4 };
// Compatibility entry point. For new code, see ContentLog in <bedrock/sdk/content_log.hpp>.
void emitContentLog(LogLevel level, std::string_view tag, std::string_view message) noexcept;
// Sends messages at or above the minimum level to the ordinary sink and, if
// installed, to Creator Content Log. The default sink is debugger/console on
// Windows (console only if opened) and stderr on other hosts.
class Logger { public:
 explicit Logger(std::string tag="Bedrock", LogLevel minimum=LogLevel::Info); ~Logger();
 Logger(const Logger&) = delete; Logger& operator=(const Logger&) = delete;
 void setLevel(LogLevel level) noexcept; LogLevel level() const noexcept;
 void debug(std::string_view message); void info(std::string_view message);
 void warn(std::string_view message); void error(std::string_view message);
 template<class... Parts> void debug(Parts&&... parts) { debug(std::string_view{join(std::forward<Parts>(parts)...)}); }
 template<class... Parts> void info(Parts&&... parts) { info(std::string_view{join(std::forward<Parts>(parts)...)}); }
 template<class... Parts> void warn(Parts&&... parts) { warn(std::string_view{join(std::forward<Parts>(parts)...)}); }
 template<class... Parts> void error(Parts&&... parts) { error(std::string_view{join(std::forward<Parts>(parts)...)}); }
 private:
 template<class T> static void append(std::ostringstream& out, T&& value) { out << std::forward<T>(value); }
 template<class First, class... Rest> static std::string join(First&& first, Rest&&... rest) {
  std::ostringstream out; append(out, std::forward<First>(first));
  (append(out, std::forward<Rest>(rest)), ...); return out.str();
 }
 struct Impl; std::unique_ptr<Impl> impl_; friend class FileLogSink;
};
class FileLogSink { public:
 enum class OpenMode { ClearOnOpen, Append }; FileLogSink(); ~FileLogSink();
 FileLogSink(const FileLogSink&) = delete; FileLogSink& operator=(const FileLogSink&) = delete;
 bool open(const std::filesystem::path& path, OpenMode mode=OpenMode::ClearOnOpen);
 // Replaces the ordinary sink, not Content Log. Keep the sink alive for as
 // long as the logger uses it; close() closes the file but does not detach.
 void attach(Logger& logger) noexcept; void close() noexcept; bool isOpen() const noexcept;
 private: struct Impl; std::unique_ptr<Impl> impl_;
};
} // namespace bedrock::sdk
