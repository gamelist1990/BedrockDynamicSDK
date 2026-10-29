#pragma once
#include <bedrock/packets/packet_api.hpp>
#include <bedrock/packets/player_list_packets.hpp>
#include <condition_variable>
#include <deque>
#include <thread>
#include <utility>

namespace bedrock {
// A serial copied-data lane: network callbacks retain owned wire bytes only.
// Skin/protocol parsing runs on this worker, independently of row construction
// and Actor sampling. No native pointer crosses the queue.
class PlayerRosterService {
  public:
    void start(PacketApi &packets) {
        (void)ProtocolCodec::types();
        {
            std::lock_guard lock(mutex_);
            if (worker_.joinable())
                return;
            stopping_ = false;
            worker_ = std::thread([this] { run(); });
        }
        for (const auto id : {PacketId::StartGame, PacketId::Disconnect, PacketId::PlayerList,
                              PacketId::PlayerSkin})
            subscriptions_.push_back(
                packets.onReceive(id, [this](const PacketReadEvent &event) { enqueue(event); }));
    }
    void stop(PacketApi &packets) {
        // Owner stops PacketApi first to drain in-flight native callbacks.
        for (const auto token : subscriptions_)
            packets.unsubscribe(token);
        subscriptions_.clear();
        {
            std::lock_guard lock(mutex_);
            stopping_ = true;
            queue_.clear();
            queuedBytes_ = 0;
        }
        ready_.notify_all();
        if (worker_.joinable())
            worker_.join();
    }
    ~PlayerRosterService() {
        {
            std::lock_guard lock(mutex_);
            stopping_ = true;
        }
        ready_.notify_all();
        if (worker_.joinable())
            worker_.join();
    }
    std::vector<PacketPlayer> snapshot() const {
        return roster_.snapshot();
    }
    PlayerListPackets::Statistics statistics() const {
        return roster_.statistics();
    }
    std::uint64_t dropped() const noexcept {
        return dropped_.load();
    }

  private:
    struct Copy {
        unsigned id;
        std::shared_ptr<const std::string> bytes;
    };
    void enqueue(const PacketReadEvent &event) {
        auto bytes = event.ownedPayload();
        std::lock_guard lock(mutex_);
        if (stopping_)
            return;
        // Never silently continue an incomplete roster after queue overflow.
        if (queue_.size() >= 128 || bytes->size() > maxQueuedBytes - queuedBytes_) {
            ++dropped_;
            queue_.clear();
            queuedBytes_ = 0;
            resetPending_ = true;
            if (bytes->size() > maxQueuedBytes) {
                ready_.notify_one();
                return;
            }
        }
        queuedBytes_ += bytes->size();
        queue_.push_back({event.rawId(), std::move(bytes)});
        ready_.notify_one();
    }
    void run() {
        for (;;) {
            Copy item{};
            bool reset{};
            {
                std::unique_lock lock(mutex_);
                ready_.wait(lock, [&] { return stopping_ || resetPending_ || !queue_.empty(); });
                if (stopping_)
                    return;
                reset = std::exchange(resetPending_, false);
                if (!queue_.empty()) {
                    item = std::move(queue_.front());
                    queue_.pop_front();
                    queuedBytes_ -= item.bytes->size();
                }
            }
            if (reset)
                roster_.clear();
            if (item.bytes)
                roster_.observe(item.id, *item.bytes);
        }
    }
    static constexpr std::size_t maxQueuedBytes = 32 * 1024 * 1024;
    PlayerListPackets roster_;
    std::mutex mutex_;
    std::condition_variable ready_;
    std::deque<Copy> queue_;
    std::size_t queuedBytes_{};
    bool stopping_{}, resetPending_{};
    std::thread worker_;
    std::vector<PacketApi::Subscription> subscriptions_;
    std::atomic<std::uint64_t> dropped_{};
};
} // namespace bedrock
