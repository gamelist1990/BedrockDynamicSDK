#include <bedrock/sdk/client_info.hpp>

#include <atomic>
#include <chrono>
#include <cstring>
#include <mutex>
#include <stdexcept>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <bedrock/detail/native_profile_functions.hpp>
#include <bedrock/packets/packet_api.hpp>
#include <bedrock/profiles/profile_json.hpp>
#include <bedrock/runtime/hook_runtime.h>
#endif

namespace bedrock::sdk {
namespace {

std::uint64_t nowNs() noexcept {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

#ifdef _WIN32

// One request/reply correlation slot for NetworkStackLatencyPacket. The stamp
// is the client's own timestamp echoed by the reply.
struct PendingPing {
    bool used{};
    std::uint64_t stamp{};
    std::uint64_t sentNs{};
};

struct Sampler {
    std::mutex installMutex;
    std::atomic<int> refs{};
    std::atomic<bool> fpsReady{};
    std::atomic<bool> cpsReady{};
    std::atomic<bool> pingReady{};
    detail::FrameCounter frames;
    detail::ClickCounter clicks;
    detail::PingTracker pings;
    bedrock::PacketAPI packets;
    BDS_HookToken renderToken{};
    BDS_HookToken mouseToken{};
    std::mutex pendingMutex;
    std::array<PendingPing, 8> pending{};
    std::size_t pendingNext{};
    std::size_t pendingCount{};
    std::mutex deviceMutex;
    DeviceInfo device;
    std::atomic<std::uint64_t> deviceStampNs{};
};

Sampler& sampler() {
    static Sampler value;
    return value;
}

struct WindowSearch {
    DWORD pid{};
    HWND window{};
    LONG area{};
};

BOOL CALLBACK findGameWindow(HWND window, LPARAM parameter) {
    auto& search = *reinterpret_cast<WindowSearch*>(parameter);
    DWORD pid{};
    GetWindowThreadProcessId(window, &pid);
    if (pid != search.pid || !IsWindowVisible(window)) return TRUE;
    RECT rect{};
    if (!GetClientRect(window, &rect)) return TRUE;
    const auto area = (rect.right - rect.left) * (rect.bottom - rect.top);
    if (area > search.area) {
        search.window = window;
        search.area = area;
    }
    return TRUE;
}

bool queryWindowSize(unsigned& width, unsigned& height) noexcept {
    WindowSearch search{GetCurrentProcessId(), nullptr, 0};
    EnumWindows(&findGameWindow, reinterpret_cast<LPARAM>(&search));
    if (!search.window) return false;
    RECT rect{};
    if (!GetClientRect(search.window, &rect)) return false;
    if (rect.right <= rect.left || rect.bottom <= rect.top) return false;
    width = static_cast<unsigned>(rect.right - rect.left);
    height = static_cast<unsigned>(rect.bottom - rect.top);
    return true;
}

// The window can be resized at any time; refresh at most every 5 s and never
// wait for the lock on the reader path.
void refreshDevice(Sampler& state, bool force) noexcept {
    const auto now = nowNs();
    if (!force) {
        const auto stamp = state.deviceStampNs.load(std::memory_order_relaxed);
        if (stamp && now >= stamp && now - stamp < 5'000'000'000ull) return;
    }
    std::unique_lock lock(state.deviceMutex, std::try_to_lock);
    if (!lock.owns_lock()) return;
    state.deviceStampNs.store(now, std::memory_order_relaxed);
    unsigned width{};
    unsigned height{};
    if (queryWindowSize(width, height)) {
        state.device.screenWidth = width;
        state.device.screenHeight = height;
    }
}

// NetworkStackLatencyPacket payload: lu64 timestamp, u8 needs_response.
bool parseLatency(std::string_view payload, std::uint64_t& stamp,
                  bool& needsResponse) noexcept {
    if (payload.size() < 9) return false;
    std::memcpy(&stamp, payload.data(), sizeof(stamp));
    needsResponse = static_cast<unsigned char>(payload[8]) != 0;
    return true;
}

void recordRequest(Sampler& state, std::string_view payload) noexcept {
    std::uint64_t stamp{};
    bool needsResponse{};
    if (!parseLatency(payload, stamp, needsResponse) || !needsResponse) return;
    const auto at = nowNs();
    // Never block the game's network thread on a diagnostics slot.
    std::unique_lock lock(state.pendingMutex, std::try_to_lock);
    if (!lock.owns_lock()) return;
    state.pending[state.pendingNext] = PendingPing{true, stamp, at};
    state.pendingNext = (state.pendingNext + 1) % state.pending.size();
    if (state.pendingCount < state.pending.size()) ++state.pendingCount;
}

void recordReply(Sampler& state, std::string_view payload) noexcept {
    std::uint64_t stamp{};
    bool needsResponse{};
    if (!parseLatency(payload, stamp, needsResponse)) return;
    const auto at = nowNs();
    std::unique_lock lock(state.pendingMutex, std::try_to_lock);
    if (!lock.owns_lock()) return;
    for (auto& entry : state.pending) {
        if (!entry.used || entry.stamp != stamp) continue;
        if (at > entry.sentNs) {
            const auto roundTrip = at - entry.sentNs;
            state.pings.add(
                static_cast<unsigned>((roundTrip + 500'000ull) / 1'000'000ull), at);
        }
        entry = PendingPing{};
        if (state.pendingCount) --state.pendingCount;
        break;
    }
}

void BDS_HOOK_CALL onRenderLevel(void* user, void* raw, BDS_HookNext next,
                                 void* frame) noexcept {
    try {
        static_cast<Sampler*>(user)->frames.add(nowNs());
    } catch (...) {
    }
    next(frame, raw);
}

void BDS_HOOK_CALL onMouseFeed(void* user, void* raw, BDS_HookNext next,
                               void* frame) noexcept {
    try {
        const auto& args = *static_cast<BDS_MouseArgs*>(raw);
        const bool synthetic =
            (args.force_motionless_pointer & BDS_MOUSE_FLAG_SYNTHETIC) != 0;
        if (!synthetic && args.state != 0) {
            const auto at = nowNs();
            auto& state = *static_cast<Sampler*>(user);
            if (args.action == 1) state.clicks.press(false, at);
            else if (args.action == 2) state.clicks.press(true, at);
        }
    } catch (...) {
    }
    next(frame, raw);
}

#endif // _WIN32

} // namespace

bool ClientInfo::install(std::string_view selectedProfileJson,
                         std::string_view embeddedProfileJson, std::string& error) {
#ifdef _WIN32
    auto& state = sampler();
    std::lock_guard lock(state.installMutex);
    if (state.refs.load(std::memory_order_relaxed) > 0) {
        state.refs.fetch_add(1, std::memory_order_relaxed);
        error.clear();
        return true;
    }
    std::string failed;
    const auto note = [&failed](const char* part, const std::exception& failure) {
        if (!failed.empty()) failed += "; ";
        failed.append(part).append(": ").append(failure.what());
    };
    nlohmann::json root;
    try {
        root = bedrock::parseNativeProfile(selectedProfileJson);
    } catch (const std::exception& failure) {
        error = std::string("client info profile unavailable: ") + failure.what();
        return false;
    }

    {
        std::lock_guard deviceLock(state.deviceMutex);
        state.device = DeviceInfo{};
        state.device.available = true;
        state.device.platform = "windows-x64";
        try {
            state.device.clientVersion = root.at("target").at("version").get<std::string>();
        } catch (...) {
        }
    }
    refreshDevice(state, true);

    // FPS: the shared render-level hook on the same per-frame entry the world
    // renderer uses. The entry guard is checked against the exact build.
    try {
        const auto& function =
            root.at("layouts").at("LevelRenderer").at("functions").at("render_level");
        void* target = bedrock::native_profile::checked(root, function);
        if (!BDS_HookSubscribe(BDS_HOOK_RENDER_LEVEL, target, &onRenderLevel, &state,
                               &state.renderToken))
            throw std::runtime_error("shared render-level hook subscription failed");
        state.fpsReady.store(true, std::memory_order_relaxed);
    } catch (const std::exception& failure) {
        state.renderToken = {};
        note("fps", failure);
    }

    // CPS: the shared mouse input hook (the client input path).
    try {
        void* target = bedrock::native_profile::checked(
            root, root.at("symbols").at("MouseDevice::feed"));
        if (!BDS_HookSubscribe(BDS_HOOK_MOUSE_FEED, target, &onMouseFeed, &state,
                               &state.mouseToken))
            throw std::runtime_error("shared mouse input hook subscription failed");
        state.cpsReady.store(true, std::memory_order_relaxed);
    } catch (const std::exception& failure) {
        state.mouseToken = {};
        note("cps", failure);
    }

    // Ping: observe the client's own NetworkStackLatency request and match the
    // echoed timestamp on the reply. Both directions ride the existing packet
    // hooks; nothing is sent and no native packet object is built.
    try {
        // PacketId::Ping is the SDK's stable name for
        // NetworkStackLatencyPacket (protocol id 115).
        state.packets.onReceive(bedrock::PacketId::Ping,
                                [](const bedrock::PacketReadEvent& event) {
                                    recordReply(sampler(), event.payload());
                                });
        state.packets.onSend(bedrock::PacketId::Ping,
                             [](bedrock::PacketWireSendEvent& event) {
                                 recordRequest(sampler(), event.packet().payload());
                             });
        std::string packetError;
        if (!state.packets.installReceive(selectedProfileJson, embeddedProfileJson,
                                          packetError))
            throw std::runtime_error(packetError);
        if (!state.packets.installSend(selectedProfileJson, packetError))
            throw std::runtime_error(packetError);
        state.pingReady.store(true, std::memory_order_relaxed);
    } catch (const std::exception& failure) {
        state.packets.stop();
        note("ping", failure);
    }

    state.refs.store(1, std::memory_order_relaxed);
    error = failed;
    return true;
#else
    (void)selectedProfileJson;
    (void)embeddedProfileJson;
    error = "native client diagnostics unavailable on this host";
    return false;
#endif
}

void ClientInfo::stop() noexcept {
#ifdef _WIN32
    auto& state = sampler();
    std::lock_guard lock(state.installMutex);
    const auto refs = state.refs.load(std::memory_order_relaxed);
    if (refs == 0) return;
    if (refs > 1) {
        state.refs.store(refs - 1, std::memory_order_relaxed);
        return;
    }
    state.refs.store(0, std::memory_order_relaxed);
    if (state.renderToken) {
        BDS_HookUnsubscribe(state.renderToken);
        state.renderToken = {};
    }
    if (state.mouseToken) {
        BDS_HookUnsubscribe(state.mouseToken);
        state.mouseToken = {};
    }
    state.packets.stop();
    state.fpsReady.store(false, std::memory_order_relaxed);
    state.cpsReady.store(false, std::memory_order_relaxed);
    state.pingReady.store(false, std::memory_order_relaxed);
#endif
}

bool ClientInfo::installed() noexcept {
#ifdef _WIN32
    return sampler().refs.load(std::memory_order_relaxed) > 0;
#else
    return false;
#endif
}

ClientMetricsSnapshot ClientInfo::snapshot() noexcept {
    ClientMetricsSnapshot out;
#ifdef _WIN32
    auto& state = sampler();
    if (state.refs.load(std::memory_order_relaxed) <= 0) return out;
    const auto now = nowNs();
    out.fpsAvailable = state.fpsReady.load(std::memory_order_relaxed);
    out.fps = state.frames.fps(now);
    out.frameTimeMs = state.frames.frameTimeMs();
    out.ping = state.pings.snapshot(now);
    out.cps = state.clicks.cps(now);
    refreshDevice(state, false);
    {
        std::lock_guard deviceLock(state.deviceMutex);
        out.device = state.device;
    }
#endif
    return out;
}

double ClientInfo::fps() noexcept {
#ifdef _WIN32
    auto& state = sampler();
    if (state.refs.load(std::memory_order_relaxed) <= 0) return 0.0;
    return state.frames.fps(nowNs());
#else
    return 0.0;
#endif
}

PingSnapshot ClientInfo::ping() noexcept {
#ifdef _WIN32
    auto& state = sampler();
    if (state.refs.load(std::memory_order_relaxed) <= 0) return {};
    return state.pings.snapshot(nowNs());
#else
    return {};
#endif
}

CpsSnapshot ClientInfo::cps() noexcept {
#ifdef _WIN32
    auto& state = sampler();
    if (state.refs.load(std::memory_order_relaxed) <= 0) return {};
    return state.clicks.cps(nowNs());
#else
    return {};
#endif
}

DeviceInfo ClientInfo::device() noexcept {
#ifdef _WIN32
    auto& state = sampler();
    if (state.refs.load(std::memory_order_relaxed) <= 0) return {};
    refreshDevice(state, false);
    std::lock_guard deviceLock(state.deviceMutex);
    return state.device;
#else
    return {};
#endif
}

} // namespace bedrock::sdk
