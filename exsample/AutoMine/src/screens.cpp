#include "auto_mine.hpp"
#include "screen_policy.hpp"

namespace automine::client {

void AutoMineMod::loadScreen(const bedrock::RuntimeProfile &profile) {
    try {
        const auto root = bedrock::parseNativeProfile(profile.json());
        topScreenFn_ = reinterpret_cast<GetTopScreenName>(bedrock::native_profile::checked(
            root, nlohmann::json{{"symbol", "ClientInstance::getTopScreenName"}}));
        memDeallocate_ = bedrock::native_profile::checked(
            root, nlohmann::json{{"symbol", "Bedrock::Memory::deallocate"}});
        screenReady_ = true;
    } catch (...) {
        screenReady_ = false;
    }
}

std::string AutoMineMod::topScreenName() {
    if (!screenReady_ || !client_.load())
        return {};
    alignas(8) std::byte buf[32]{};
    __try {
        topScreenFn_(client_.load(), buf);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return {};
    }
    std::uint64_t size = 0, cap = 0;
    std::memcpy(&size, buf + 16, sizeof(size));
    std::memcpy(&cap, buf + 24, sizeof(cap));
    const char *data = reinterpret_cast<const char *>(buf);
    void *heap = nullptr;
    if (cap > 15) {
        std::memcpy(&heap, buf, sizeof(heap));
        data = static_cast<const char *>(heap);
    }
    std::string out;
    if (data && size <= 256)
        out.assign(data, static_cast<std::size_t>(size));
    if (heap && memDeallocate_ && cap < 0x800)
        reinterpret_cast<void(__fastcall *)(void *)>(memDeallocate_)(heap);
    return out;
}

bool AutoMineMod::inventoryScreenUp() {
    if (!screenReady_)
        return false;
    return contains(topScreenName(), "inventory");
}

bool AutoMineMod::screenUp(const char *needle) {
    if (!screenReady_)
        return !game_.gameplayActive();
    return contains(topScreenName(), needle);
}

bool AutoMineMod::anyScreenUp() {
    if (screenReady_) {
        const std::string screen = topScreenName();
        return automine::userScreenActive(screen);
    }
    return true; // no screen evidence: do not open/close containers
}

bool AutoMineMod::automationScreenUp() {
    if (!screenReady_)
        return true;
    return automine::screenBlocksMovement(topScreenName(), continueInChat_);
}

} // namespace automine::client
