#include "auto_mine.hpp"
#include "screen_policy.hpp"

namespace automine::client {

void AutoMineMod::loadScreen(const bedrock::sdk::Profile &profile,
                             const bedrock::sdk::SymbolResolver &resolver) {
    (void)profile;
    try {
        auto resolve = [&resolver](const char *name) -> std::uintptr_t {
            if (const auto hit = resolver.resolve(name))
                return hit->address;
            if (const auto hit = resolver.resolveCandidate(name))
                return hit->address;
            return 0;
        };
        const auto fn = resolve("ClientInstance::getTopScreenName");
        const auto dealloc = resolve("Bedrock::Memory::deallocate");
        if (!fn || !dealloc) {
            screenReady_ = false;
            return;
        }
        topScreenFn_ = reinterpret_cast<GetTopScreenName>(fn);
        memDeallocate_ = reinterpret_cast<void *>(dealloc);
        screenReady_ = true;
    } catch (...) {
        screenReady_ = false;
    }
}

// MSVC C2712: a function containing __try must not require object unwinding,
// so the SEH-wrapped game call lives in this POD-only helper and
// topScreenName (which builds std::string results) stays __try-free.
bool AutoMineMod::callTopScreenNameSeh(GetTopScreenName fn, void *client, void *buf) {
    __try {
        fn(client, buf);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

std::string AutoMineMod::topScreenName() {
    if (!screenReady_ || !client_.load())
        return {};
    alignas(8) std::byte buf[32]{};
    if (!callTopScreenNameSeh(topScreenFn_, client_.load(), buf))
        return {};
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
