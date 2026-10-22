#include <bedrock/console.hpp>
#include <bedrock/logger.hpp>
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
    using bedrock::LogLevel;
    using bedrock::Logger;
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
        assert(fileSink.open(logPath)); // The default mode clears previous runs.
        fileSink.attach(log);
        log.info("current run only");
        fileSink.close();
    }
    std::ifstream saved(logPath, std::ios::binary);
    const std::string savedText{std::istreambuf_iterator<char>(saved), {}};
    assert(savedText.find("previous run history") == std::string::npos);
    assert(savedText.find("current run only") != std::string::npos);
    std::filesystem::remove(logPath);

#ifndef _WIN32
    assert(!bedrock::Console::isOpen());
    assert(!bedrock::Console::open());
#endif
}
