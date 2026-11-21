#include <bedrock/sdk/logging.hpp>

#include <bedrock/enums/logging.hpp>
#include <bedrock/logging/content_log.hpp>
#include <bedrock/logging/logger.hpp>

#include <cstdint>
#include <filesystem>

namespace bedrock::sdk {

struct Logger::Impl { bedrock::Logger logger; explicit Impl(std::string tag, LogLevel level) : logger(std::move(tag), static_cast<bedrock::LogLevel>(level)) {} };
void emitContentLog(LogLevel level, std::string_view tag, std::string_view message) noexcept { bedrock::emitContentLog(static_cast<bedrock::LogLevel>(level), tag, message); }
Logger::Logger(std::string tag, LogLevel minimum) : impl_(std::make_unique<Impl>(std::move(tag), minimum)) {}
Logger::~Logger() = default;
void Logger::setLevel(LogLevel level) noexcept { impl_->logger.setLevel(static_cast<bedrock::LogLevel>(level)); }
LogLevel Logger::level() const noexcept { return static_cast<LogLevel>(impl_->logger.level()); }
void Logger::debug(std::string_view message) { impl_->logger.debug(message); }
void Logger::info(std::string_view message) { impl_->logger.info(message); }
void Logger::warn(std::string_view message) { impl_->logger.warn(message); }
void Logger::error(std::string_view message) { impl_->logger.error(message); }

struct FileLogSink::Impl { bedrock::FileLogSink sink; };
FileLogSink::FileLogSink() : impl_(std::make_unique<Impl>()) {}
FileLogSink::~FileLogSink() = default;
bool FileLogSink::open(const std::filesystem::path& path, OpenMode mode) { return impl_->sink.open(path, mode == OpenMode::Append ? bedrock::FileLogSink::OpenMode::Append : bedrock::FileLogSink::OpenMode::ClearOnOpen); }
void FileLogSink::attach(Logger& logger) noexcept {
    impl_->sink.attach(logger.impl_->logger);
}
void FileLogSink::close() noexcept { impl_->sink.close(); }
bool FileLogSink::isOpen() const noexcept { return impl_->sink.isOpen(); }

} // namespace bedrock::sdk
