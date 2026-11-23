#include <bedrock/logging/console.hpp>
#include <bedrock/logging/logger.hpp>
#include <bedrock/sdk/content_log.hpp>
#include <bedrock/sdk/logging.hpp>
#include <cassert>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <chrono>
#include <vector>

struct Captured {
    std::vector<std::pair<bedrock::LogLevel, std::string>> lines;
};
static void sink(bedrock::LogLevel level, std::string_view line, void* ctx) {
    static_cast<Captured*>(ctx)->lines.emplace_back(level, std::string(line));
}
int main() {
#ifdef _WIN32
    std::fprintf(stderr, "logging-test: start\n");
#endif
    using bedrock::LogLevel;
    using bedrock::Logger;
    Logger defaults;
    assert(defaults.level() == LogLevel::Info);
    Logger log("AutoSprint", LogLevel::Info);
    Captured capture;
    log.setSink(&sink, &capture);
    log.debug("filtered ", 12);
    assert(capture.lines.empty());

    log.info("Loaded version ", "1.26.52.3");
    log.warn("Input mode = ", 2);
    log.error("Error code: ", 42);
    assert(capture.lines.size() == 3);
    assert(capture.lines[0].first == LogLevel::Info);
    assert(capture.lines[0].second.find("[INFO] [AutoSprint] Loaded version 1.26.52.3\n") != std::string::npos);
    assert(capture.lines[1].second.find("[WARN] [AutoSprint] Input mode = 2\n") != std::string::npos);
    assert(capture.lines[2].second.find("[ERROR] [AutoSprint] Error code: 42\n") != std::string::npos);

    log.setLevel(LogLevel::Debug);
    log.debug("status=", true, " rva=", 123);
    assert(capture.lines.size() == 4);
    assert(capture.lines[3].second.find("status=1 rva=123") != std::string::npos);
    log.setLevel(LogLevel::Off);
    log.error("must be filtered");
    assert(capture.lines.size() == 4);

    const auto logPath = std::filesystem::temp_directory_path() /
        ("bedrock-sdk-log-" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()) + ".log");
    {
        std::ofstream previous(logPath, std::ios::binary | std::ios::trunc);
        previous << "previous run history\n";
    }
    log.setLevel(LogLevel::Info);
    {
        bedrock::FileLogSink fileSink;
#ifdef _WIN32
        std::fprintf(stderr, "logging-test: before open\n");
#endif
        assert(fileSink.open(logPath)); // The default mode clears previous runs.
#ifdef _WIN32
        std::fprintf(stderr, "logging-test: opened\n");
#endif
        fileSink.attach(log);
        log.info("current run only");
#ifdef _WIN32
        std::fprintf(stderr, "logging-test: wrote\n");
#endif
        log.setSink(nullptr);
        fileSink.close();
#ifdef _WIN32
        std::fprintf(stderr, "logging-test: closed\n");
#endif
    }
    std::ifstream saved(logPath, std::ios::binary);
    const std::string savedText{std::istreambuf_iterator<char>(saved), {}};
#ifdef _WIN32
    std::fprintf(stderr, "logging-test: read %zu bytes\n", savedText.size());
#endif
    assert(savedText.find("previous run history") == std::string::npos);
    assert(savedText.find("current run only") != std::string::npos);
    saved.close();
    std::error_code removeError;
    std::filesystem::remove(logPath, removeError);
    assert(!removeError);
#ifdef _WIN32
    std::fprintf(stderr, "logging-test: done\n");
#endif

    // Public SDK facade regression: variadic calls and std::string calls must
    // select the string_view overload instead of recursing through the template.
    {
        bedrock::sdk::Logger facade("FacadeProbe", bedrock::sdk::LogLevel::Debug);
        const auto facadePath = std::filesystem::temp_directory_path() /
            ("bedrock-facade-" + std::to_string(
                std::chrono::steady_clock::now().time_since_epoch().count()) + ".log");
        bedrock::sdk::FileLogSink facadeSink;
        assert(facadeSink.open(facadePath, bedrock::sdk::FileLogSink::OpenMode::ClearOnOpen));
        facadeSink.attach(facade);
        facade.debug("alpha ", "beta");
        const std::string single = "gamma";
        facade.info(single);
        facade.warn("delta ", 1);
        facade.error("epsilon ", 2);
        facadeSink.close();
        std::ifstream saved(facadePath, std::ios::binary);
        const std::string savedText{std::istreambuf_iterator<char>(saved), {}};
        assert(savedText.find("alpha beta") != std::string::npos);
        assert(savedText.find("gamma") != std::string::npos);
        assert(savedText.find("delta 1") != std::string::npos);
        assert(savedText.find("epsilon 2") != std::string::npos);
        saved.close();
        std::error_code removeError;
        std::filesystem::remove(facadePath, removeError);
        assert(!removeError);
    }

    // Content-log bridge: the SDK logger feeds the vanilla Content Log in
    // addition to its existing sink, with level and mod tag preserved.
    struct ContentLine {
        bedrock::LogLevel level;
        std::string tag;
        std::string text;
    };
    static std::vector<ContentLine> contentLines;
    contentLines.clear();
    const auto contentWriter = +[](bedrock::LogLevel level, std::string_view tag,
                                   std::string_view text, void*) noexcept {
        contentLines.push_back({level, std::string(tag), std::string(text)});
    };
    bedrock::setContentLogWriter(contentWriter);
    log.setLevel(LogLevel::Info);
    log.info("creator-info");
    log.warn("creator-warn");
    log.error("creator-error");
    log.debug("creator-filtered");
    assert(contentLines.size() == 3);
    assert(contentLines[0].level == LogLevel::Info && contentLines[0].tag == "AutoSprint" &&
           contentLines[0].text == "creator-info");
    assert(contentLines[1].level == LogLevel::Warn && contentLines[1].text == "creator-warn");
    assert(contentLines[2].level == LogLevel::Error && contentLines[2].text == "creator-error");
    // The bridge is independent of the ordinary sink (file/debug output).
    Captured ordinary;
    log.setSink(&sink, &ordinary);
    log.info("with-sink");
    assert(ordinary.lines.size() == 1 && contentLines.size() == 4);
    log.setSink(nullptr);
    using bedrock::sdk::ContentLog;
    using SdkLevel = bedrock::sdk::LogLevel;
    assert(ContentLog::available());
    ContentLog::write(SdkLevel::Debug, "CppMod", "direct");
    ContentLog::write(SdkLevel::Off, "CppMod", "not a severity");
    bedrock::sdk::emitContentLog(SdkLevel::Warn, "ScriptMod", "legacy");
    ContentLog::write(SdkLevel::Info, {}, {});
    assert(contentLines.size() == 7 && ordinary.lines.size() == 1);
    assert(contentLines[4].level == LogLevel::Debug && contentLines[4].tag == "CppMod" &&
           contentLines[4].text == "direct");
    assert(contentLines[5].level == LogLevel::Warn && contentLines[5].tag == "ScriptMod" &&
           contentLines[5].text == "legacy");
    assert(contentLines[6].tag.empty() && contentLines[6].text.empty());
    bedrock::setContentLogWriter(nullptr);
    assert(!ContentLog::available());
    ContentLog::write(SdkLevel::Error, "CppMod", "no writer");
    log.info("after-clear");
    assert(contentLines.size() == 7); // no writer => no content-log lines

#ifndef _WIN32
    assert(!bedrock::Console::isOpen());
    assert(!bedrock::Console::open());
#endif
}
