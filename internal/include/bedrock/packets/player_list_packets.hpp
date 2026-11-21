#pragma once
#include <bedrock/packets/wire_packet.hpp>
#include <bedrock/packets/protocol_codec.hpp>
#include <algorithm>
#include <array>
#include <map>
#include <mutex>
#include <vector>
#include <atomic>
#include <memory>

namespace bedrock {
// PlayerRecord/Skin/SkinImage are decoded from the selected data protocol.
// No native PlayerList fields or version-specific wire offsets are used.
struct PacketPlayer {
    std::array<std::byte, 16> uuid{};
    std::int64_t uniqueId{};
    std::string name;
    std::array<std::uint32_t, 64> face{};
    bool hasFace{};
};
class PlayerListPackets {
  public:
    static bool supports(std::string_view profile) {
        const auto wire =
            nlohmann::json::parse(profile).value("protocol", nlohmann::json::object());
        return wire.value("wire_roster_available", false) &&
               wire.value("sha256", std::string{}) == protocol::sourceSha256;
    }
    static bool supports(std::string_view selected, std::string_view embedded) {
        const auto current = nlohmann::json::parse(selected);
        if (current.contains("protocol"))
            return supports(selected);
        const auto fallback = nlohmann::json::parse(embedded);
        for (const auto *field :
             {"sha256", "text_sha256", "size_of_image", "text_rva", "text_size"})
            if (current.at("target").at(field) != fallback.at("target").at(field))
                return false;
        return supports(embedded);
    }
    struct Statistics {
        std::uint64_t packets{}, added{}, removed{}, skinUpdates{}, rejected{}, generation{};
        std::size_t players{};
        std::string error;
    };
    void observe(unsigned packetId, std::string_view payload) noexcept {
        try {
            if (packetId == static_cast<unsigned>(PacketId::StartGame) ||
                packetId == static_cast<unsigned>(PacketId::Disconnect)) {
                clear();
                return;
            }
            if (packetId != static_cast<unsigned>(PacketId::PlayerList) &&
                packetId != static_cast<unsigned>(PacketId::PlayerSkin))
                return;
            std::uint64_t resetEpoch{};
            {
                std::lock_guard lock(mutex_);
                resetEpoch = resetEpoch_;
            }
            const auto decoded = ProtocolCodec{}.decode(static_cast<PacketId>(packetId), payload);
            std::vector<Update> updates;
            if (packetId == static_cast<unsigned>(PacketId::PlayerSkin)) {
                Update update;
                update.skinOnly = true;
                readUuid(decoded.at("uuid"), update.player.uuid);
                readSkin(decoded.at("skin"), update.player);
                updates.push_back(std::move(update));
            } else {
                const auto &records = decoded.at("records");
                if (records.size() > 512)
                    throw std::runtime_error("PlayerList count limit");
                updates.reserve(records.size());
                for (const auto &record : records) {
                    Update update;
                    const auto type = record.at("type").get<std::string>();
                    if (type != "remove" && type != "add")
                        throw std::runtime_error("unknown PlayerRecord type");
                    update.remove = type == "remove";
                    readUuid(record.at("uuid"), update.player.uuid);
                    if (!update.remove) {
                        update.player.uniqueId = record.at("entity_unique_id").get<std::int64_t>();
                        update.player.name = record.at("username").get<std::string>();
                        if (update.player.name.size() > 512)
                            throw std::runtime_error("PlayerList name limit");
                        readSkin(record.at("skin_data"), update.player);
                    }
                    updates.push_back(std::move(update));
                }
            }
            std::lock_guard lock(mutex_);
            if (resetEpoch != resetEpoch_)
                return;
            ++stats_.packets;
            // Apply the entire decoded batch transactionally. An oversized or
            // malformed batch preserves the previously published roster.
            auto next = players_;
            for (auto &update : updates) {
                if (update.remove)
                    next.erase(update.player.uuid);
                else if (update.skinOnly) {
                    const auto it = next.find(update.player.uuid);
                    if (it != next.end()) {
                        it->second.face = update.player.face;
                        it->second.hasFace = update.player.hasFace;
                    }
                } else
                    next.insert_or_assign(update.player.uuid, std::move(update.player));
            }
            if (next.size() > 512)
                throw std::runtime_error("packet roster exceeds 512 players");
            auto snapshot = std::make_shared<std::vector<PacketPlayer>>();
            snapshot->reserve(next.size());
            for (const auto &[id, player] : next)
                snapshot->push_back(player);
            players_.swap(next);
            published_.store(std::move(snapshot), std::memory_order_release);
            for (const auto &update : updates) {
                if (update.remove)
                    ++stats_.removed;
                else if (update.skinOnly)
                    ++stats_.skinUpdates;
                else
                    ++stats_.added;
            }
            stats_.players = players_.size();
            ++stats_.generation;
            stats_.error.clear();
        } catch (const std::exception &e) {
            std::lock_guard lock(mutex_);
            ++stats_.rejected;
            stats_.error = e.what();
        } catch (...) {
            std::lock_guard lock(mutex_);
            ++stats_.rejected;
            stats_.error = "packet roster decode failed";
        }
    }
    std::shared_ptr<const std::vector<PacketPlayer>> snapshotShared() const noexcept {
        return published_.load(std::memory_order_acquire);
    }
    std::vector<PacketPlayer> snapshot() const {
        return *snapshotShared();
    }
    Statistics statistics() const {
        std::lock_guard lock(mutex_);
        return stats_;
    }
    void clear() {
        std::lock_guard lock(mutex_);
        players_.clear();
        published_.store(emptySnapshot_, std::memory_order_release);
        ++resetEpoch_;
        stats_.players = 0;
        ++stats_.generation;
        stats_.error.clear();
    }

  private:
    struct Update {
        bool remove{}, skinOnly{};
        PacketPlayer player;
    };
    struct Image {
        std::uint32_t width{}, height{};
        std::string_view bytes;
    };
    static void readUuid(const nlohmann::json &source, std::array<std::byte, 16> &id) {
        const auto &bytes = source.get_binary();
        if (bytes.size() != 16)
            throw std::runtime_error("invalid packet UUID");
        std::memcpy(id.data(), bytes.data(), 16);
    }
    static void face(const Image &skin, PacketPlayer &player) {
        if (skin.width < 64 || skin.height < 16 || skin.width % 8)
            return;
        const auto unit = skin.width / 8;
        if (unit * 2 > skin.height || unit * 6 > skin.width)
            return;
        const auto pixel = [&](unsigned x, unsigned y, unsigned channel) {
            return static_cast<unsigned char>(
                skin.bytes[(std::size_t(y) * skin.width + x) * 4 + channel]);
        };
        for (unsigned y = 0; y < 8; ++y)
            for (unsigned x = 0; x < 8; ++x) {
                const auto sx = x * unit / 8, sy = unit + y * unit / 8;
                const auto alpha = pixel(unit * 5 + sx, sy, 3);
                const auto blend = [&](unsigned c) {
                    return (pixel(unit * 5 + sx, sy, c) * alpha +
                            pixel(unit + sx, sy, c) * (255 - alpha)) /
                           255;
                };
                // SDK face snapshots use RGBA with R in the most significant byte.
                player.face[y * 8 + x] =
                    (blend(0) << 24) | (blend(1) << 16) | (blend(2) << 8) | 255u;
            }
        player.hasFace = true;
    }
    static void readSkin(const nlohmann::json &source, PacketPlayer &player) {
        const auto &skin = source.at("skin_data");
        const auto &bytes = skin.at("data").get_binary();
        Image image;
        image.width = skin.at("width").get<std::uint32_t>();
        image.height = skin.at("height").get<std::uint32_t>();
        image.bytes = {reinterpret_cast<const char *>(bytes.data()), bytes.size()};
        if (image.width > 4096 || image.height > 4096 ||
            std::uint64_t(image.width) * image.height * 4 != bytes.size())
            throw std::runtime_error("invalid wire skin image extent");
        face(image, player);
    }
    inline static const auto emptySnapshot_ = std::make_shared<const std::vector<PacketPlayer>>();
    std::atomic<std::shared_ptr<const std::vector<PacketPlayer>>> published_{emptySnapshot_};
    mutable std::mutex mutex_;
    std::map<std::array<std::byte, 16>, PacketPlayer> players_;
    Statistics stats_;
    std::uint64_t resetEpoch_{};
};
} // namespace bedrock
