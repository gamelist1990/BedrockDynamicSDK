#pragma once
#include <bedrock/packets/packet_read_events.hpp>
#include <bedrock/packets/wire_packet_events.hpp>
#include <atomic>
#include <algorithm>
#include <functional>
#include <memory>
#include <mutex>
#include <vector>
#include <map>
#include <set>

namespace bedrock {
// Mod-facing filtered packet subscriptions. Receive runs after successful game
// decoding and borrows its copied payload for this callback only. Send runs
// before batching and may edit event.packet() or cancel the packet.
// Callbacks may be on networking threads: retain copies, never native Actors.
class PacketApi {
  public:
    using Subscription = std::uint64_t;
    using ReceiveHandler = std::function<void(const PacketReadEvent &)>;
    using SendHandler = std::function<void(PacketWireSendEvent &)>;
    Subscription onReceive(PacketId id, ReceiveHandler handler) {
        return add(static_cast<int>(id), std::move(handler), {});
    }
    Subscription onAnyReceive(ReceiveHandler handler) {
        return add(-1, std::move(handler), {});
    }
    Subscription onSend(PacketId id, SendHandler handler) {
        return add(static_cast<int>(id), {}, std::move(handler));
    }
    Subscription onAnySend(SendHandler handler) {
        return add(-1, {}, std::move(handler));
    }
    void unsubscribe(Subscription id) {
        std::lock_guard lock(mutex_);
        auto next = std::make_shared<std::vector<Entry>>(*handlers_.load());
        std::erase_if(*next, [&](const Entry &e) { return e.token == id; });
        handlers_.store(std::move(next));
    }
    bool installReceive(std::string_view selected, std::string_view embedded, std::string &error) {
        if (receiveReady_)
            return true;
        try {
            reads_.load(selected, embedded);
            refreshReceiveRoutes();
            receiveReady_ = true;
            error.clear();
            return true;
        } catch (const std::exception &e) {
            clearReceiveHooks();
            error = e.what();
            return false;
        }
    }
    bool installReceive(std::string_view profile, std::string &error) {
        if (receiveReady_)
            return true;
        try {
            reads_.load(parseNativeProfile(profile));
            refreshReceiveRoutes();
            receiveReady_ = true;
            error.clear();
            return true;
        } catch (const std::exception &e) {
            clearReceiveHooks();
            error = e.what();
            return false;
        }
    }
    PacketReadEvents::Statistics receiveStatistics() const noexcept {
        return reads_.statistics();
    }
    std::size_t receiveTargetCount() const noexcept {
        return receiveTargetCount_.load();
    }
    bool installSend(std::string_view profile, std::string &error) {
        if (sendToken_)
            return true;
        try {
            sends_.load(parseNativeProfile(profile));
            if (!sends_.subscribe(&send, this, &sendToken_))
                throw std::runtime_error("Packet send subscription failed");
            error.clear();
            return true;
        } catch (const std::exception &e) {
            error = e.what();
            return false;
        }
    }
    bool receiveReady() const noexcept {
        return receiveReady_.load();
    }
    // Explicit stop outside DllMain. An in-flight callback may finish; owners
    // must retain this object until callbacks no longer use it (Mods pin it).
    void stop() {
        receiveReady_.store(false);
        {
            std::lock_guard lock(mutex_);
            handlers_.store(std::make_shared<const std::vector<Entry>>());
        }
        clearReceiveHooks();
        if (sendToken_)
            BDS_HookUnsubscribe(sendToken_);
        sendToken_ = nullptr;
    }

  private:
    struct Entry {
        Subscription token{};
        int id{-1};
        ReceiveHandler receive;
        SendHandler send;
    };
    Subscription add(int id, ReceiveHandler receive, SendHandler send) {
        const bool needsReceive = static_cast<bool>(receive);
        Subscription token{};
        {
            std::lock_guard lock(mutex_);
            auto next = std::make_shared<std::vector<Entry>>(*handlers_.load());
            token = ++serial_;
            next->push_back({token, id, std::move(receive), std::move(send)});
            handlers_.store(std::move(next));
        }
        if (needsReceive && receiveReady_)
            try {
                refreshReceiveRoutes();
            } catch (...) {
                unsubscribe(token);
                throw;
            }
        return token;
    }
    void refreshReceiveRoutes() {
        std::lock_guard lock(receiveMutex_);
        if (!reads_.routed()) {
            if (!receiveToken_ && !reads_.subscribe(&receive, this, &receiveToken_))
                throw std::runtime_error("common Packet receiver subscription failed");
            receiveTargetCount_ = 1;
            return;
        }
        std::set<void *> targets;
        const auto handlers = handlers_.load();
        for (const auto &handler : *handlers) {
            if (!handler.receive)
                continue;
            if (handler.id < 0) {
                for (const auto &[id, route] : reads_.routes())
                    targets.insert(route.targets.begin(), route.targets.end());
            } else {
                const auto route = reads_.routes().find(static_cast<unsigned>(handler.id));
                if (route == reads_.routes().end())
                    throw std::runtime_error(
                        "Packet decoder vtable unavailable or ambiguous for ID " +
                        std::to_string(handler.id));
                targets.insert(route->second.targets.begin(), route->second.targets.end());
            }
        }
        if (targets.empty())
            throw std::runtime_error("register receive handlers before installing PacketApi");
        for (auto *target : targets)
            if (!receiveRoutes_.contains(target)) {
                BDS_HookToken token{};
                if (!reads_.subscribeTarget(target, &receive, this, &token))
                    throw std::runtime_error("Packet decoder subscription conflict or route limit");
                receiveRoutes_[target] = token;
            }
        receiveTargetCount_ = receiveRoutes_.size();
    }
    void clearReceiveHooks() {
        std::lock_guard lock(receiveMutex_);
        receiveReady_ = false;
        if (receiveToken_)
            BDS_HookUnsubscribe(receiveToken_);
        receiveToken_ = nullptr;
        for (const auto &[target, token] : receiveRoutes_)
            BDS_HookUnsubscribe(token);
        receiveRoutes_.clear();
        receiveTargetCount_ = 0;
    }
    static void BDS_HOOK_CALL receive(void *user, void *raw, BDS_HookNext next, void *frame) {
        auto &self = *static_cast<PacketApi *>(user);
        const auto handlers = self.handlers_.load();
        self.reads_.dispatchFiltered(
            *static_cast<BDS_PacketReadArgs *>(raw),
            [&](const PacketReadEvent &event) {
                for (const auto &entry : *handlers)
                    if (entry.receive &&
                        (entry.id < 0 || entry.id == static_cast<int>(event.rawId())))
                        try {
                            entry.receive(event);
                        } catch (...) {
                        }
            },
            next, frame,
            [&](unsigned id) {
                return std::any_of(handlers->begin(), handlers->end(), [&](const Entry &e) {
                    return e.receive && (e.id < 0 || e.id == static_cast<int>(id));
                });
            });
    }
    static void BDS_HOOK_CALL send(void *user, void *raw, BDS_HookNext next, void *frame) {
        auto &self = *static_cast<PacketApi *>(user);
        auto &args = *static_cast<BDS_PacketWireArgs *>(raw);
        if (!self.sends_.dispatch(
                args,
                [&](PacketWireSendEvent &event) {
                    const auto handlers = self.handlers_.load();
                    for (const auto &entry : *handlers)
                        if (entry.send &&
                            (entry.id < 0 || entry.id == static_cast<int>(event.id())))
                            try {
                                entry.send(event);
                            } catch (...) {
                            }
                },
                next, frame))
            next(frame, &args);
    }
    PacketReadEvents reads_;
    PacketWireEvents sends_;
    BDS_HookToken receiveToken_{}, sendToken_{};
    std::atomic<bool> receiveReady_{};
    std::atomic<std::size_t> receiveTargetCount_{};
    std::mutex receiveMutex_;
    std::map<void *, BDS_HookToken> receiveRoutes_;
    std::mutex mutex_;
    Subscription serial_{};
    std::atomic<std::shared_ptr<const std::vector<Entry>>> handlers_{
        std::make_shared<const std::vector<Entry>>()};
};
} // namespace bedrock
