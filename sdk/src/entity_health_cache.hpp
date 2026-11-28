#pragma once

#include <cmath>
#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <nlohmann/json.hpp>

namespace bedrock::sdk::detail {

// Packet state, scoped to one connection. The owner serializes access.
class EntityHealthCache {
public:
    void setLocalRuntimeId(std::uint64_t id) {
        if (id) localId_ = id;
    }

    void setLocalHealth(int health) {
        // Keep the legacy SetHealth behavior: ignore nonpositive values.
        if (health > 0) update(localId_, static_cast<float>(health));
    }

    void observeAttributes(const nlohmann::json& packet) {
        readHealth(packet.at("runtime_entity_id").get<std::uint64_t>(),
                   packet.at("attributes"), "current");
    }

    void observeActor(const nlohmann::json& packet) {
        const auto id = packet.at("runtime_id").get<std::uint64_t>();
        spawn(packet.at("unique_id").get<std::int64_t>(), id);
        readHealth(id, packet.at("attributes"), "value");
    }

    void observePlayer(const nlohmann::json& packet) {
        const auto id = packet.at("runtime_id").get<std::uint64_t>();
        const auto it = playerIds_.find(uuidKey(packet.at("uuid")));
        if (it != playerIds_.end()) spawn(it->second, id);
        else forgetRuntime(id);
    }

    void observePlayerList(const nlohmann::json& packet) {
        for (const auto& record : packet.at("records")) {
            const auto& type = record.at("type");
            auto uuid = uuidKey(record.at("uuid"));
            if (type == "add")
                playerIds_.insert_or_assign(std::move(uuid), record.at("entity_unique_id").get<std::int64_t>());
            else if (type == "remove")
                playerIds_.erase(uuid);
        }
    }

    void remove(std::int64_t uniqueId) {
        if (const auto it = runtimeByUnique_.find(uniqueId); it != runtimeByUnique_.end())
            forgetRuntime(it->second);
    }

    void changeDimension() {
        std::erase_if(values_, [&](const auto& entry) { return entry.first != localId_; });
        runtimeByUnique_.clear();
        uniqueByRuntime_.clear();
    }

    [[nodiscard]] std::optional<float> health(std::uint64_t id) const {
        if (const auto it = values_.find(id); it != values_.end())
            return it->second;
        return std::nullopt;
    }

private:
    static std::string uuidKey(const nlohmann::json& uuid) {
        const auto& bytes = uuid.get_binary();
        return {bytes.begin(), bytes.end()};
    }

    void update(std::uint64_t id, float health) {
        if (id && std::isfinite(health) && health >= 0)
            values_.insert_or_assign(id, health);
    }

    void readHealth(std::uint64_t id, const nlohmann::json& attributes, const char* field) {
        for (const auto& attr : attributes) {
            if (!attr.is_object()) continue;
            const auto value = attr.find(field);
            if (attr.value("name", std::string{}) == "minecraft:health" &&
                value != attr.end() && value->is_number())
                update(id, value->get<float>());
        }
    }

    void forgetRuntime(std::uint64_t id) {
        values_.erase(id);
        if (const auto it = uniqueByRuntime_.find(id); it != uniqueByRuntime_.end()) {
            runtimeByUnique_.erase(it->second);
            uniqueByRuntime_.erase(it);
        }
    }

    void spawn(std::int64_t uniqueId, std::uint64_t id) {
        if (!id) return;
        remove(uniqueId);
        forgetRuntime(id);
        runtimeByUnique_.emplace(uniqueId, id);
        uniqueByRuntime_.emplace(id, uniqueId);
    }

    std::uint64_t localId_{};
    std::unordered_map<std::uint64_t, float> values_;
    std::unordered_map<std::int64_t, std::uint64_t> runtimeByUnique_;
    std::unordered_map<std::uint64_t, std::int64_t> uniqueByRuntime_;
    std::unordered_map<std::string, std::int64_t> playerIds_;
};

} // namespace bedrock::sdk::detail
