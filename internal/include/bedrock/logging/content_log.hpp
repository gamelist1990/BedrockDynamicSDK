#pragma once

#include <bedrock/enums/logging.hpp>

#include <atomic>
#include <string_view>

namespace bedrock {

// Bridge from the SDK's normal logging API to the game's vanilla Content Log
// (Settings -> Creator -> Content Log). The native core installs one writer
// once the exact-build game profile has been validated; until then (or when
// the symbol cannot be resolved) no writer is installed and logging simply
// keeps using the existing sinks.
//
// This declaration is deliberately free of Windows/native dependencies so the
// header-only logger and its host tests build on every platform. The writer
// runs on the caller's thread and must be noexcept; the native implementation
// serialises access to the game function itself.
using ContentLogWriter = void (*)(LogLevel level, std::string_view tag,
                                  std::string_view message, void* userdata) noexcept;

namespace detail {
inline std::atomic<ContentLogWriter> contentLogWriter{nullptr};
inline std::atomic<void*> contentLogUserdata{nullptr};
} // namespace detail

// Install (or clear, with writer == nullptr) the process-local writer. The
// caller owns `userdata` and must keep it alive for as long as the writer is
// installed.
inline void setContentLogWriter(ContentLogWriter writer, void* userdata = nullptr) noexcept {
    detail::contentLogUserdata.store(userdata, std::memory_order_relaxed);
    detail::contentLogWriter.store(writer, std::memory_order_release);
}

[[nodiscard]] inline ContentLogWriter contentLogWriter() noexcept {
    return detail::contentLogWriter.load(std::memory_order_acquire);
}

// Called by Logger::log and by the ScriptClient LogSink. No-op until the
// native core has installed a writer; never throws and allocates nothing.
inline void emitContentLog(LogLevel level, std::string_view tag,
                           std::string_view message) noexcept {
    if (const auto writer = contentLogWriter())
        writer(level, tag, message, detail::contentLogUserdata.load(std::memory_order_acquire));
}

} // namespace bedrock
