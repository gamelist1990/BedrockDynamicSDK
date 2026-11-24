#pragma once

#include "common.hpp"
#include <atomic>

namespace stageb::http {

using Headers = std::vector<std::pair<std::string, std::string>>;

struct Request {
    std::string url, method, body;
    Headers headers;
    Clock::time_point deadline;
    std::size_t maxBytes;
};

struct Result {
    int status = 0;
    Headers headers;
    std::string body;
    const char* code = nullptr;
    std::string message;
};

Result transfer(const Request&, const std::atomic<bool>& cancelled);
void parseHeader(std::string_view line, Headers& headers);
inline Result failure(const char* code, std::string message) {
    Result result;
    result.code = code;
    result.message = std::move(message);
    return result;
}

} // namespace stageb::http
