#pragma once
#include <string>
#include <cstdint>
namespace bedrock {
struct ServerAddress {
    std::string ip;
    std::uint16_t port{};

    [[nodiscard]] bool valid() const noexcept {
        return !ip.empty() && port != 0;
    }
    [[nodiscard]] std::string address() const {
        if (ip.find(':') != std::string::npos && (ip.empty() || ip.front() != '['))
            return "[" + ip + "]:" + std::to_string(port);
        return ip + ":" + std::to_string(port);
    }
};

} // namespace bedrock
