#include <bedrock/sdk/content_log.hpp>

#include <bedrock/logging/content_log.hpp>

namespace bedrock::sdk {

bool ContentLog::available() noexcept {
    return bedrock::contentLogWriter() != nullptr;
}

void ContentLog::write(LogLevel level, std::string_view tag, std::string_view message) noexcept {
    if (level == LogLevel::Off) return;

    // A default-constructed string_view has null data; the game adapter
    // formats both arguments as strings, including when their length is zero.
    bedrock::sdk::emitContentLog(level, tag.empty() ? std::string_view{""} : tag,
                                message.empty() ? std::string_view{""} : message);
}

} // namespace bedrock::sdk
