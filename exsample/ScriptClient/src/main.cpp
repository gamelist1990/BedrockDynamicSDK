#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include "command_service.hpp"
#include "script_mod.hpp"

#include <bedrock/logging/file_log_sink.hpp>
#include <bedrock/profiles/profile_manager.hpp>
#include <bedrock/sdk.hpp>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace {
bedrock::Logger logger{"ScriptClient"};
bedrock::FileLogSink fileLog;

// §5.1: TS "seconds" -> C++ ticks. Infinity stays positive infinity.
double secondsToTicks(double seconds) {
    return seconds * 20.0;
}

stageb::BackendOutcome toOutcome(const bedrock::sdk::Result<void>& res) {
    if (res) return stageb::BackendOutcome::success();
    return stageb::BackendOutcome::error(
        stageb::errorCodeName(static_cast<int>(res.error().code)),
        res.error().message);
}

template<class T>
stageb::BackendOutcome toOutcome(const bedrock::sdk::Result<T>& res) {
    if (res) return stageb::BackendOutcome::success();
    return stageb::BackendOutcome::error(
        stageb::errorCodeName(static_cast<int>(res.error().code)),
        res.error().message);
}

stageb::DynamicPropertyData fromSdkProp(const bedrock::sdk::DynamicProperty& prop) {
    if (const auto* b = std::get_if<bool>(&prop)) return *b;
    if (const auto* d = std::get_if<double>(&prop)) return *d;
    if (const auto* s = std::get_if<std::string>(&prop)) return *s;
    if (const auto* v = std::get_if<bedrock::sdk::Vector3>(&prop))
        return stageb::Vector3Data{v->x, v->y, v->z};
    return false;
}

bedrock::sdk::DynamicProperty toSdkProp(const stageb::DynamicPropertyData& prop) {
    if (const auto* b = std::get_if<bool>(&prop)) return bedrock::sdk::DynamicProperty{*b};
    if (const auto* d = std::get_if<double>(&prop)) return bedrock::sdk::DynamicProperty{*d};
    if (const auto* s = std::get_if<std::string>(&prop)) return bedrock::sdk::DynamicProperty{*s};
    if (const auto* v = std::get_if<stageb::Vector3Data>(&prop))
        return bedrock::sdk::DynamicProperty{bedrock::sdk::Vector3{v->x, v->y, v->z}};
    return bedrock::sdk::DynamicProperty{false};
}

class GameWorldBackend final : public stageb::WorldBackend {
public:
    void attachMod(const std::string& modName, stageb::ScriptMod& mod) {
        std::lock_guard lock(mutex_);
        mods_[modName] = &mod;
        worlds_[modName] = &mod.world();
    }

    void detachMod(const std::string& modName) {
        std::lock_guard lock(mutex_);
        mods_.erase(modName);
        worlds_.erase(modName);
        playerCaches_.erase(modName);
        entityCaches_.erase(modName);
        removeGammaLockHeld(modName);
    }

    void connectBrightness(const bedrock::sdk::ClientHost& host) {
        std::lock_guard lock(mutex_);
        const auto res = brightness_.connect(host);
        brightnessConnected_ = static_cast<bool>(res);
        if (!res) {
            brightnessConnectError_ = res.error().message;
            logger.warn("BrightnessOverride connect unavailable: ", brightnessConnectError_);
        }
    }

    bool hasLocalPlayer() const override {
        return hasLocalPlayer({});
    }

    // Stage B live fix: a JS command runs on the owning tick while the active
    // ClientHost frame may be bound to a different Mod's World (the anchor, or
    // another ScriptMod that drained the queue). The instance World accessors
    // intentionally resolve only for the Mod bound to the frame, so use the
    // SDK's active-session resolvers at call time. `worldFor()` stays as the
    // per-Mod attach guard so Mods remain isolated (round-2 design).
    bool hasLocalPlayer(const std::string& modName) const override {
        return worldFor(modName) && bedrock::sdk::World::activeLocalPlayer().has_value();
    }

    std::optional<std::uint64_t> localPlayerId(const std::string& modName) const override {
        if (!worldFor(modName)) return std::nullopt;
        const auto lp = bedrock::sdk::World::activeLocalPlayer();
        if (!lp) return std::nullopt;
        refreshCache(modName, lp);
        return 1;
    }

    std::vector<std::uint64_t> allPlayerIds(const std::string& modName) const override {
        if (!worldFor(modName)) return {};
        refreshCache(modName, bedrock::sdk::World::activeLocalPlayer());
        std::vector<std::uint64_t> out;
        if (const auto it = playerCaches_.find(modName); it != playerCaches_.end()) {
            out.reserve(it->second.size());
            for (const auto& [id, player] : it->second) {
                (void)player;
                out.push_back(id);
            }
        }
        return out;
    }

    std::optional<std::string> currentDimensionId(const std::string& modName) const override {
        if (!worldFor(modName)) return std::nullopt;
        if (const auto dim = bedrock::sdk::World::activeDimension()) {
            return dim->typeId().value_or("minecraft:overworld");
        }
        return std::nullopt;
    }

    std::optional<std::string> getDimensionId(const std::string& modName,
                                              const std::string& typeId) const override {
        if (!worldFor(modName)) return std::nullopt;
        if (const auto dim = bedrock::sdk::World::activeDimension()) {
            if (dim->typeId() && *dim->typeId() == typeId) return dim->typeId();
        }
        return std::nullopt;
    }

    stageb::BackendOutcome worldSendMessage(const std::string& modName,
                                            const std::string& text) override {
        if (!worldFor(modName))
            return stageb::BackendOutcome::error("Unavailable", "No active client world");
        const auto player = bedrock::sdk::World::activeLocalPlayer();
        if (!player)
            return stageb::BackendOutcome::error("Unavailable", "No local player in the active session");
        return toOutcome(player->sendMessage(text));
    }

    std::optional<stageb::DynamicPropertyData> worldGetDynamicProperty(
        const std::string& modName, const std::string& key) const override {
        if (worldFor(modName)) {
            if (const auto prop = bedrock::sdk::World::activeWorldProperty(key))
                return fromSdkProp(*prop);
            // Fall back to per-Mod isolated store when WorldSession shares a map across Mods.
        }
        return stageb::WorldBackend::worldGetDynamicProperty(modName, key);
    }

    stageb::BackendOutcome worldSetDynamicProperty(
        const std::string& modName, const std::string& key,
        const std::optional<stageb::DynamicPropertyData>& value) override {
        if (key.empty())
            return stageb::BackendOutcome::error("Invalid", "Dynamic property key is empty");
        if (worldFor(modName)) {
            const std::string scopedKey = modName + ":" + key;
            std::optional<bedrock::sdk::DynamicProperty> sdkVal;
            if (value) sdkVal = toSdkProp(*value);
            const auto res = bedrock::sdk::World::setActiveWorldProperty(scopedKey, sdkVal);
            if (!res) return toOutcome(res);
        }
        return stageb::WorldBackend::worldSetDynamicProperty(modName, key, value);
    }

    // --- Entity / Player resolution ---
    bool entityIsValid(const std::string& modName, std::uint64_t id) const override {
        if (const auto* e = findEntity(modName, id)) return e->isValid();
        return false;
    }

    std::optional<std::string> entityTypeId(const std::string& modName,
                                            std::uint64_t id) const override {
        if (const auto* e = findEntity(modName, id)) return e->typeId();
        return std::nullopt;
    }

    std::optional<stageb::Vector3Data> entityLocation(const std::string& modName,
                                                      std::uint64_t id) const override {
        if (const auto* e = findEntity(modName, id)) {
            if (const auto loc = e->location())
                return stageb::Vector3Data{loc->x, loc->y, loc->z};
        }
        return std::nullopt;
    }

    std::optional<stageb::RotationData> entityRotation(const std::string& modName,
                                                       std::uint64_t id) const override {
        if (const auto* e = findEntity(modName, id)) {
            if (const auto rot = e->getRotation())
                return stageb::RotationData{rot->x, rot->y};
        }
        return std::nullopt;
    }

    std::optional<std::string> entityDimensionId(const std::string& modName,
                                                 std::uint64_t id) const override {
        if (const auto* e = findEntity(modName, id)) {
            if (const auto dim = e->dimension())
                return dim->typeId().value_or("minecraft:overworld");
        }
        return std::nullopt;
    }

    std::optional<stageb::ItemStackData> entityItemStack(const std::string& modName,
                                                         std::uint64_t id) const override {
        if (const auto* e = findEntity(modName, id)) {
            if (const auto item = e->getItemStack())
                return stageb::ItemStackData{item->typeId(), item->amount()};
        }
        return std::nullopt;
    }

    stageb::BackendOutcome entityTeleport(const std::string& modName,
                                          std::uint64_t id,
                                          stageb::Vector3Data pos) override {
        if (const auto* e = findEntity(modName, id))
            return toOutcome(e->teleport({pos.x, pos.y, pos.z}));
        if (!std::isfinite(pos.x) || !std::isfinite(pos.y) || !std::isfinite(pos.z))
            return stageb::BackendOutcome::error("Invalid", "Teleport coordinates must be finite");
        return stageb::BackendOutcome::error("Unavailable", "Entity is not loaded");
    }

    stageb::BackendOutcome entityAddTag(const std::string& modName,
                                        std::uint64_t id, const std::string& tag,
                                        bool& outAdded) override {
        outAdded = false;
        if (const auto* e = findEntity(modName, id)) {
            const auto res = e->addTag(tag);
            if (!res) return toOutcome(res);
            outAdded = res.value();
            return stageb::BackendOutcome::success();
        }
        return stageb::BackendOutcome::error("Unavailable", "Entity is not loaded");
    }

    std::optional<stageb::DynamicPropertyData> entityGetDynamicProperty(
        const std::string& modName, std::uint64_t id, const std::string& key) const override {
        if (const auto* e = findEntity(modName, id)) {
            const std::string scopedKey = modName + ":" + key;
            if (const auto prop = e->getDynamicProperty(scopedKey))
                return fromSdkProp(*prop);
        }
        return stageb::WorldBackend::entityGetDynamicProperty(modName, id, key);
    }

    stageb::BackendOutcome entitySetDynamicProperty(
        const std::string& modName, std::uint64_t id, const std::string& key,
        const std::optional<stageb::DynamicPropertyData>& value) override {
        if (key.empty())
            return stageb::BackendOutcome::error("Invalid", "Dynamic property key is empty");
        if (const auto* e = findEntity(modName, id)) {
            const std::string scopedKey = modName + ":" + key;
            std::optional<bedrock::sdk::DynamicProperty> sdkVal;
            if (value) sdkVal = toSdkProp(*value);
            return toOutcome(e->setDynamicProperty(scopedKey, sdkVal));
        }
        return stageb::WorldBackend::entitySetDynamicProperty(modName, id, key, value);
    }

    std::optional<float> entityHealth(const std::string& modName,
                                      std::uint64_t id) const override {
        if (const auto* e = findEntity(modName, id)) {
            if (const auto h = e->getComponent<bedrock::sdk::HealthComponent>())
                return h->currentValue();
        }
        return std::nullopt;
    }

    std::optional<float> entityFood(const std::string& modName,
                                    std::uint64_t id) const override {
        if (const auto* e = findEntity(modName, id)) {
            if (const auto f = e->getComponent<bedrock::sdk::FoodComponent>())
                return f->currentValue();
        }
        return std::nullopt;
    }

    std::optional<stageb::Vector3Data> entityMovementVelocity(const std::string& modName,
                                                              std::uint64_t id) const override {
        if (const auto* e = findEntity(modName, id)) {
            if (const auto m = e->getComponent<bedrock::sdk::MovementComponent>()) {
                const auto v = m->velocity();
                return stageb::Vector3Data{v.x, v.y, v.z};
            }
        }
        return std::nullopt;
    }

    std::optional<std::vector<std::optional<stageb::ItemStackData>>> entityInventory(
        const std::string& modName, std::uint64_t id) const override {
        if (const auto* e = findEntity(modName, id)) {
            if (const auto inv = e->getComponent<bedrock::sdk::InventoryComponent>()) {
                std::vector<std::optional<stageb::ItemStackData>> slots;
                slots.reserve(inv->size());
                for (std::size_t i = 0; i < inv->size(); ++i) {
                    if (const auto item = inv->getItem(i))
                        slots.push_back(stageb::ItemStackData{item->typeId(), item->amount()});
                    else
                        slots.push_back(std::nullopt);
                }
                return slots;
            }
        }
        return std::nullopt;
    }

    std::optional<std::array<std::optional<stageb::ItemStackData>, 6>> entityEquippable(
        const std::string& modName, std::uint64_t id) const override {
        if (const auto* e = findEntity(modName, id)) {
            if (const auto eq = e->getComponent<bedrock::sdk::EquippableComponent>()) {
                std::array<std::optional<stageb::ItemStackData>, 6> slots;
                constexpr bedrock::sdk::EquipmentSlot kSlots[6] = {
                    bedrock::sdk::EquipmentSlot::MainHand,
                    bedrock::sdk::EquipmentSlot::Offhand,
                    bedrock::sdk::EquipmentSlot::Head,
                    bedrock::sdk::EquipmentSlot::Chest,
                    bedrock::sdk::EquipmentSlot::Legs,
                    bedrock::sdk::EquipmentSlot::Feet
                };
                for (std::size_t i = 0; i < 6; ++i) {
                    if (const auto item = eq->getEquipment(kSlots[i]))
                        slots[i] = stageb::ItemStackData{item->typeId(), item->amount()};
                }
                return slots;
            }
        }
        return std::nullopt;
    }

    // --- Player operations ---
    std::optional<std::string> playerName(const std::string& modName,
                                          std::uint64_t id) const override {
        if (const auto* p = findPlayer(modName, id)) return p->name();
        return std::nullopt;
    }

    stageb::BackendOutcome playerSendMessage(const std::string& modName,
                                             std::uint64_t id,
                                             const std::string& text) override {
        if (const auto* p = findPlayer(modName, id))
            return toOutcome(p->sendMessage(text));
        return stageb::BackendOutcome::error("Unavailable", "Player is not loaded");
    }

    stageb::BackendOutcome playerAddEffect(const std::string& modName,
                                           std::uint64_t id,
                                           const std::string& typeId, double seconds,
                                           int amplifier, bool showParticles) override {
        if (const auto* p = findPlayer(modName, id)) {
            bedrock::sdk::EffectOptions options;
            options.amplifier = static_cast<std::uint8_t>(std::clamp(amplifier, 0, 255));
            options.showParticles = showParticles;
            const auto result = p->addEffect(typeId, secondsToTicks(seconds), options);
            if (!result) logger.error("addEffect: ", result.error().message);
            return toOutcome(result);
        }
        return stageb::BackendOutcome::error("Unavailable", "Effect operations require the local player in the owning tick");
    }

    stageb::BackendOutcome playerRemoveEffect(const std::string& modName,
                                              std::uint64_t id,
                                              const std::string& typeId,
                                              bool& outRemoved) override {
        outRemoved = false;
        if (const auto* p = findPlayer(modName, id)) {
            const auto res = p->removeEffect(typeId);
            if (!res) return toOutcome(res);
            outRemoved = res.value();
            return stageb::BackendOutcome::success();
        }
        return stageb::BackendOutcome::error("Unavailable", "Effect operations require the local player in the owning tick");
    }

    stageb::BackendOutcome playerClearEffects(const std::string& modName,
                                              std::uint64_t id) override {
        if (const auto* p = findPlayer(modName, id))
            return toOutcome(p->clearEffects());
        return stageb::BackendOutcome::error("Unavailable", "Effect operations require the local player in the owning tick");
    }

    stageb::BackendOutcome playerGetEffects(const std::string& modName,
                                            std::uint64_t id,
                                            std::vector<stageb::EffectData>& outEffects) override {
        outEffects.clear();
        if (const auto* p = findPlayer(modName, id)) {
            const auto effects = p->getEffects();
            if (!effects) return toOutcome(effects);
            for (const auto& effect : effects.value()) {
                outEffects.push_back(stageb::EffectData{
                    effect.typeId,
                    effect.duration / 20.0,
                    static_cast<int>(effect.options.amplifier),
                    effect.options.showParticles
                });
            }
            return stageb::BackendOutcome::success();
        }
        return stageb::BackendOutcome::error("Unavailable", "Effect operations require the local player in the owning tick");
    }

    // --- Dimension & Block ---
    std::optional<std::string> dimensionGetBlockType(
        const std::string& modName, const std::string& dimId,
        stageb::BlockPosData pos) const override {
        auto dim = resolveDimension(modName, dimId);
        if (!dim) return std::nullopt;
        if (const auto block = dim->getBlock({pos.x, pos.y, pos.z}))
            return block->typeId();
        return std::nullopt;
    }

    std::vector<std::uint64_t> dimensionGetEntityIds(
        const std::string& modName, const std::string& dimId) const override {
        auto* w = worldFor(modName);
        if (!w) return {};
        auto dim = resolveDimension(modName, dimId);
        if (!dim) return {};
        refreshCache(modName, bedrock::sdk::World::activeLocalPlayer());
        std::vector<std::uint64_t> out;
        if (const auto it = entityCaches_.find(modName); it != entityCaches_.end()) {
            out.reserve(it->second.size());
            for (const auto& [id, ent] : it->second) {
                (void)ent;
                out.push_back(id);
            }
        }
        return out;
    }

    stageb::BackendOutcome dimensionSpawnEntity(
        const std::string& modName, const std::string& dimId,
        const std::string& typeId, stageb::Vector3Data pos,
        std::uint64_t& outEntityId) override {
        outEntityId = 0;
        auto dim = resolveDimension(modName, dimId);
        if (!dim) return stageb::BackendOutcome::error("Unavailable", "Dimension is not active");
        const auto res = dim->spawnEntity(typeId, {pos.x, pos.y, pos.z});
        return toOutcome(res);
    }

    stageb::BackendOutcome blockSetType(
        const std::string& modName, const std::string& dimId,
        stageb::BlockPosData pos, const std::string& typeId) override {
        auto dim = resolveDimension(modName, dimId);
        if (!dim) return stageb::BackendOutcome::error("Unavailable", "Block is not in the active dimension");
        const auto block = dim->getBlock({pos.x, pos.y, pos.z});
        if (!block) return stageb::BackendOutcome::error("Unavailable", "Block is not loaded");
        return toOutcome(block->setType(typeId));
    }

    // --- Settings / Keybinds / Brightness bridges ---
    stageb::BackendOutcome registerSettingsPage(
        const std::string& /*modName*/, const bedrock::sdk::SettingsPage& page) override {
        return toOutcome(page.registerPage());
    }

    stageb::BackendOutcome unregisterSettingsPage(
        const std::string& /*modName*/, const bedrock::sdk::SettingsPage& page) override {
        return toOutcome(page.unregisterPage());
    }

    stageb::BackendOutcome keybindAdd(
        const std::string& modName, const std::string& id,
        const std::string& labelKey, bedrock::sdk::Key defaultKey) override {
        std::lock_guard lock(mutex_);
        if (const auto it = mods_.find(modName); it != mods_.end() && it->second) {
            return toOutcome(it->second->keybinds().add(id, labelKey, defaultKey));
        }
        return stageb::BackendOutcome::success();
    }

    void keybindLocale(
        const std::string& modName, const std::string& localeId,
        const std::vector<bedrock::sdk::Translation>& entries) override {
        std::lock_guard lock(mutex_);
        if (const auto it = mods_.find(modName); it != mods_.end() && it->second) {
            it->second->keybinds().locale(localeId, entries);
        }
    }

    std::optional<bedrock::sdk::Key> keybindKey(
        const std::string& modName, std::string_view id) const override {
        std::lock_guard lock(mutex_);
        if (const auto it = mods_.find(modName); it != mods_.end() && it->second) {
            return it->second->keybinds().key(id);
        }
        return std::nullopt;
    }

    stageb::BackendOutcome setBrightnessGamma(
        const std::string& modName, std::optional<float> gamma) override {
        if (gamma && (!std::isfinite(*gamma) || *gamma < 0.0f || *gamma > 25.0f))
            return stageb::BackendOutcome::error("Invalid", "Gamma must be finite and in 0..25");
        std::lock_guard lock(mutex_);
        if (!brightnessConnected_) {
            return stageb::BackendOutcome::error("Unavailable",
                brightnessConnectError_.empty() ? "Brightness override is not connected"
                                                : brightnessConnectError_);
        }
        if (gamma) {
            gammaStack_.erase(std::remove_if(gammaStack_.begin(), gammaStack_.end(),
                [&](const auto& p) { return p.first == modName; }), gammaStack_.end());
            gammaStack_.emplace_back(modName, *gamma);
            return toOutcome(brightness_.setGamma(*gamma));
        } else {
            return removeGammaLockHeld(modName);
        }
    }

private:
    bedrock::sdk::World* worldFor(const std::string& modName) const {
        std::lock_guard lock(mutex_);
        if (!modName.empty()) {
            const auto it = worlds_.find(modName);
            if (it != worlds_.end()) return it->second;
        }
        if (!worlds_.empty()) return worlds_.begin()->second;
        return nullptr;
    }

    std::optional<bedrock::sdk::Dimension> resolveDimension(
        const std::string& modName, const std::string& dimId) const {
        if (!worldFor(modName)) return std::nullopt;
        auto dim = bedrock::sdk::World::activeDimension();
        if (!dim) return std::nullopt;
        if (!dim->typeId() || *dim->typeId() == dimId) return dim;
        return std::nullopt;
    }

    void refreshCache(const std::string& modName,
                      const std::optional<bedrock::sdk::Player>& local) const {
        auto& pMap = playerCaches_[modName];
        auto& eMap = entityCaches_[modName];
        pMap.clear();
        eMap.clear();
        std::uint64_t nextId = 2;
        if (local) {
            pMap.emplace(1, *local);
            eMap.emplace(1, static_cast<bedrock::sdk::Entity>(*local));
        }
        const auto localName = local ? local->name() : std::nullopt;
        for (const auto& p : bedrock::sdk::World::activePlayers()) {
            if (local && p.name() == localName) continue;
            const auto id = nextId++;
            pMap.emplace(id, p);
            eMap.emplace(id, static_cast<bedrock::sdk::Entity>(p));
        }
        if (const auto dim = bedrock::sdk::World::activeDimension()) {
            for (const auto& e : dim->getEntities()) {
                if (e.typeId() == "minecraft:player") continue;
                const auto id = nextId++;
                eMap.emplace(id, e);
            }
        }
    }

    const bedrock::sdk::Player* findPlayer(const std::string& modName,
                                           std::uint64_t id) const {
        if (!worldFor(modName)) return nullptr;
        refreshCache(modName, bedrock::sdk::World::activeLocalPlayer());
        const auto mIt = playerCaches_.find(modName);
        if (mIt == playerCaches_.end()) return nullptr;
        const auto it = mIt->second.find(id);
        return it != mIt->second.end() ? &it->second : nullptr;
    }

    const bedrock::sdk::Entity* findEntity(const std::string& modName,
                                           std::uint64_t id) const {
        if (!worldFor(modName)) return nullptr;
        refreshCache(modName, bedrock::sdk::World::activeLocalPlayer());
        const auto mIt = entityCaches_.find(modName);
        if (mIt == entityCaches_.end()) return nullptr;
        const auto it = mIt->second.find(id);
        return it != mIt->second.end() ? &it->second : nullptr;
    }

    stageb::BackendOutcome removeGammaLockHeld(const std::string& modName) {
        const auto before = gammaStack_.size();
        gammaStack_.erase(std::remove_if(gammaStack_.begin(), gammaStack_.end(),
            [&](const auto& p) { return p.first == modName; }), gammaStack_.end());
        if (!brightnessConnected_) return stageb::BackendOutcome::success();
        if (gammaStack_.empty()) {
            if (before > 0) return toOutcome(brightness_.setGamma(std::nullopt));
            return stageb::BackendOutcome::success();
        }
        return toOutcome(brightness_.setGamma(gammaStack_.back().second));
    }

    mutable std::mutex mutex_;
    std::unordered_map<std::string, stageb::ScriptMod*> mods_;
    std::unordered_map<std::string, bedrock::sdk::World*> worlds_;
    mutable std::unordered_map<std::string, std::map<std::uint64_t, bedrock::sdk::Player>> playerCaches_;
    mutable std::unordered_map<std::string, std::map<std::uint64_t, bedrock::sdk::Entity>> entityCaches_;

    bedrock::sdk::BrightnessOverride brightness_;
    bool brightnessConnected_ = false;
    std::string brightnessConnectError_;
    std::vector<std::pair<std::string, float>> gammaStack_;
};

struct Runtime {
    std::shared_ptr<GameWorldBackend> world = std::make_shared<GameWorldBackend>();
    std::unique_ptr<stageb::PluginHost> host;
    std::unique_ptr<stageb::CommandService> commands;
    std::unique_ptr<bedrock::sdk::ClientHost> clientHost;
    std::vector<std::unique_ptr<stageb::ScriptMod>> scriptMods;
    std::unique_ptr<stageb::HostAnchorMod> anchor;

    void syncScriptMods(bool allowCallback = false) {
        if (!host || !clientHost) return;
        // Detach mods that are no longer active
        for (auto it = scriptMods.begin(); it != scriptMods.end();) {
            const auto& name = (*it)->modName();
            bool stillActive = false;
            for (const auto& view : host->list()) {
                if (view.name == name && view.status == stageb::ModStatus::Active) {
                    stillActive = true;
                    break;
                }
            }
            if (!stillActive) {
                world->detachMod(name);
                if (!allowCallback) (void)clientHost->detach(**it);
                it = scriptMods.erase(it);
            } else {
                ++it;
            }
        }
        // Attach newly active mods
        for (const auto& view : host->list()) {
            if (view.status != stageb::ModStatus::Active) continue;
            bool alreadyAttached = false;
            for (const auto& existing : scriptMods) {
                if (existing->modName() == view.name) {
                    alreadyAttached = true;
                    break;
                }
            }
            if (alreadyAttached) continue;
            auto mod = std::make_unique<stageb::ScriptMod>(
                *host, view.name, [this] { commands->drain(*host); });
            world->attachMod(view.name, *mod);
            if (!allowCallback) {
                if (const auto attached = clientHost->attach(*mod); !attached) {
                    world->detachMod(view.name);
                    logger.error("Cannot attach ", view.name, ": ", attached.error().message);
                    continue;
                }
            } else {
                (void)mod->enable();
            }
            scriptMods.push_back(std::move(mod));
        }
    }
};

Runtime& runtime() {
    static Runtime value;
    return value;
}

DWORD WINAPI start(LPVOID parameter) {
    HMODULE pinned{};
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                            GET_MODULE_HANDLE_EX_FLAG_PIN,
                            reinterpret_cast<LPCWSTR>(&start), &pinned))
        return 1;
    try {
        wchar_t path[32768]{};
        const auto length = GetModuleFileNameW(static_cast<HMODULE>(parameter), path,
                                               ARRAYSIZE(path));
        if (!length || length >= ARRAYSIZE(path)) return 2;
        const auto directory = std::filesystem::path(path).parent_path();
        if (fileLog.open(directory / L"ScriptClient.log")) fileLog.attach(logger);

        bedrock::ProfileManager profiles;
        std::string error;
        if (!profiles.open({}, {bedrock::UpdatePolicy::EmbeddedOnly,
                                bedrock::CandidatePolicy::AllowCandidates}, error)) {
            logger.error("Game profile unavailable: ", error);
            return 4;
        }
        const auto profile = profiles.snapshot();
        const auto version = profile->choice().version;

        auto& state = runtime();
        state.commands = std::make_unique<stageb::CommandService>();
        state.commands->setRespond([](const std::string& text) {
            logger.info(text);
        });
        state.host = std::make_unique<stageb::PluginHost>(
            stageb::resolveModsDir(directory), version, state.world);
        // Bug 5: persist each mod's console.log to <ScriptClient.dll dir>/<Mod>.log.
        state.host->setLogDirectory(directory);
        // File API roots are anchored to the DLL directory, not to the mods
        // directory override, so mod data survives mod deletion/redeploy and
        // `/mods reload`. Each mod gets <DLL dir>/mod-data/<ModName>/.
        state.host->setDataDirectory(directory / "mod-data");
        state.host->setDefinitionsChanged([&state](const stageb::WireDefinitions& definitions) {
            std::vector<std::string> names;
            for (const auto& view : state.host->list()) names.push_back(view.name);
            state.commands->setModNames(std::move(names));
            if (state.commands->native().isLoaded()) {
                std::string replaceError;
                if (!state.commands->replaceDefinitions(definitions, replaceError))
                    logger.error("command replaceDefinitions: ", replaceError);
                state.syncScriptMods(true);
            }
        });

        // §7.1/§7.2: start with the anchor, then attach one ScriptMod per JS
        // plugin. All share the single GameFrameScope driven by ClientHost.
        state.anchor = std::make_unique<stageb::HostAnchorMod>(*state.host, [&state] {
            state.commands->drain(*state.host);
        });
        state.clientHost = std::make_unique<bedrock::sdk::ClientHost>();
        if (const auto started = state.clientHost->start(*state.anchor, [](const auto& e) {
                logger.error("SDK client host: ", e.message);
            }); !started) {
            logger.error("Cannot start SDK client host: ", started.error().message);
            return 7;
        }
        state.world->connectBrightness(*state.clientHost);

        if (!state.host->initialize(error)) {
            logger.error("PluginHost initialization failed: ", error);
            return 5;
        }
        for (const auto& view : state.host->list())
            logger.info("mod ", view.name, " [", stageb::statusName(view.status), "] api ",
                        view.apiVersion, view.error.empty() ? "" : " - " + view.error);

        if (!state.commands->load(*profile, state.host->definitions(), error)) {
            logger.error("CommandService load failed: ", error);
            return 6;
        }

        state.syncScriptMods();
        logger.info("ScriptClient ready; mods dir: ", state.host->modsDir().string());
        return 0;
    } catch (const std::exception& error) {
        logger.error("Startup failed: ", error.what());
        return 3;
    } catch (...) {
        OutputDebugStringA("[ScriptClient] Startup failed\n");
        return 8;
    }
}
} // namespace

BOOL WINAPI DllMain(HINSTANCE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(module);
        if (const auto worker = CreateThread(nullptr, 0, start, module, 0, nullptr))
            CloseHandle(worker);
    }
    return TRUE;
}
