#include <bedrock/sdk/client_host.hpp>

namespace bedrock::sdk {
struct ClientHost::Impl {};
ClientHost::ClientHost() = default;
ClientHost::~ClientHost() = default;
Result<void> ClientHost::start(Mod&, ErrorHandler) {
    return Result<void>::error(ErrorCode::Unavailable, "Native client host was not built");
}
Result<void> ClientHost::stop() { return Result<void>::ok(); }
Result<void> ClientHost::attach(Mod&) {
    return Result<void>::error(ErrorCode::Unavailable, "Native client host was not built");
}
Result<void> ClientHost::detach(Mod&) { return Result<void>::ok(); }
Result<void> ClientHost::synchronize(const std::function<void()>&) {
    return Result<void>::error(ErrorCode::Unavailable, "Native client host was not built");
}
} // namespace bedrock::sdk
