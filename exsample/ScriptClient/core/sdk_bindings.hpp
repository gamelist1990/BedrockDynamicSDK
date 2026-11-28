#pragma once

#include "command_model.hpp"
#include "common.hpp"
#include "host_context.hpp"
#include "js_runtime.hpp"

#include <bedrock/sdk.hpp>

#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace stageb {

struct Vector3Data {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
};

struct BlockPosData {
    int x = 0;
    int y = 0;
    int z = 0;
};

struct RotationData {
    float x = 0.0f;
    float y = 0.0f;
};

using DynamicPropertyData = std::variant<bool, double, std::string, Vector3Data>;

struct ItemStackData {
    std::string typeId;
    unsigned amount = 1;
};

struct EffectData {
    std::string typeId;
    double durationSeconds = 0.0;
    int amplifier = 0;
    bool showParticles = true;
};

struct CpsData {
    unsigned left = 0;
    unsigned right = 0;
};

struct DeviceData {
    bool available = false;
    std::string platform;
    std::string clientVersion;
    unsigned screenWidth = 0;
    unsigned screenHeight = 0;
};

struct BackendOutcome {
    bool ok = true;
    std::string code = "Unavailable";
    std::string message;

    static BackendOutcome success() { return {true, {}, {}}; }
    static BackendOutcome error(std::string c, std::string msg) {
        return {false, std::move(c), std::move(msg)};
    }
};

// Test/real-game seam for §5.2 player, entity, dimension, block, world,
// settings, keybinds, and brightness operations.
// All calls carry `modName` so that per-Mod isolation is preserved (§7.2).
class WorldBackend {
public:
    virtual ~WorldBackend() = default;

    // --- World ---
    virtual bool hasLocalPlayer(const std::string& /*modName*/) const {
        return hasLocalPlayer();
    }
    virtual bool hasLocalPlayer() const = 0;

    virtual std::optional<std::uint64_t> localPlayerId(const std::string& modName) const {
        return hasLocalPlayer(modName) ? std::optional<std::uint64_t>{1} : std::nullopt;
    }

    virtual std::vector<std::uint64_t> allPlayerIds(const std::string& modName) const {
        if (auto id = localPlayerId(modName)) return {*id};
        return {};
    }

    virtual std::optional<std::string> currentDimensionId(const std::string& /*modName*/) const {
        return std::nullopt;
    }

    virtual std::optional<std::string> getDimensionId(const std::string& modName,
                                                      const std::string& typeId) const {
        auto cur = currentDimensionId(modName);
        if (cur && *cur == typeId) return cur;
        return std::nullopt;
    }

    virtual BackendOutcome worldSendMessage(const std::string& /*modName*/,
                                            const std::string& text) {
        sendMessage(text);
        return BackendOutcome::success();
    }
    virtual void sendMessage(const std::string&) {}

    virtual std::optional<DynamicPropertyData> worldGetDynamicProperty(
        const std::string& modName, const std::string& key) const {
        auto modIt = fallbackWorldProps_.find(modName);
        if (modIt == fallbackWorldProps_.end()) return std::nullopt;
        auto it = modIt->second.find(key);
        if (it == modIt->second.end()) return std::nullopt;
        return it->second;
    }

    virtual BackendOutcome worldSetDynamicProperty(
        const std::string& modName, const std::string& key,
        const std::optional<DynamicPropertyData>& value) {
        if (key.empty()) return BackendOutcome::error("Invalid", "Dynamic property key is empty");
        if (value) fallbackWorldProps_[modName][key] = *value;
        else fallbackWorldProps_[modName].erase(key);
        return BackendOutcome::success();
    }

    // --- Entity / Player ---
    virtual bool entityIsValid(const std::string& modName, std::uint64_t id) const {
        auto local = localPlayerId(modName);
        return local && *local == id;
    }

    virtual std::optional<std::string> entityTypeId(const std::string& modName,
                                                    std::uint64_t id) const {
        auto local = localPlayerId(modName);
        if (local && *local == id) return std::string("minecraft:player");
        return std::nullopt;
    }

    virtual std::optional<Vector3Data> entityLocation(const std::string& /*modName*/,
                                                      std::uint64_t /*id*/) const {
        return std::nullopt;
    }

    virtual std::optional<RotationData> entityRotation(const std::string& /*modName*/,
                                                       std::uint64_t /*id*/) const {
        return std::nullopt;
    }

    virtual std::optional<std::string> entityDimensionId(const std::string& modName,
                                                         std::uint64_t id) const {
        if (!entityIsValid(modName, id)) return std::nullopt;
        return currentDimensionId(modName);
    }

    virtual std::optional<ItemStackData> entityItemStack(const std::string& /*modName*/,
                                                         std::uint64_t /*id*/) const {
        return std::nullopt;
    }

    virtual BackendOutcome entityTeleport(const std::string& /*modName*/,
                                          std::uint64_t /*id*/, Vector3Data pos) {
        if (!std::isfinite(pos.x) || !std::isfinite(pos.y) || !std::isfinite(pos.z))
            return BackendOutcome::error("Invalid", "Teleport coordinates must be finite");
        return BackendOutcome::error("Unavailable", "General teleport is not available in this client");
    }

    virtual BackendOutcome entityAddTag(const std::string& /*modName*/,
                                        std::uint64_t /*id*/, const std::string& /*tag*/,
                                        bool& outAdded) {
        outAdded = false;
        return BackendOutcome::error("Unavailable", "Native entity tags are not available in this client");
    }

    virtual std::optional<DynamicPropertyData> entityGetDynamicProperty(
        const std::string& modName, std::uint64_t id, const std::string& key) const {
        if (!entityIsValid(modName, id)) return std::nullopt;
        auto modIt = fallbackEntityProps_.find(modName);
        if (modIt == fallbackEntityProps_.end()) return std::nullopt;
        auto entIt = modIt->second.find(id);
        if (entIt == modIt->second.end()) return std::nullopt;
        auto it = entIt->second.find(key);
        if (it == entIt->second.end()) return std::nullopt;
        return it->second;
    }

    virtual BackendOutcome entitySetDynamicProperty(
        const std::string& modName, std::uint64_t id, const std::string& key,
        const std::optional<DynamicPropertyData>& value) {
        if (!entityIsValid(modName, id))
            return BackendOutcome::error("Unavailable", "Entity is not loaded");
        if (key.empty())
            return BackendOutcome::error("Invalid", "Dynamic property key is empty");
        if (value) fallbackEntityProps_[modName][id][key] = *value;
        else fallbackEntityProps_[modName][id].erase(key);
        return BackendOutcome::success();
    }

    // Components
    virtual std::optional<float> entityHealth(const std::string& /*modName*/,
                                              std::uint64_t /*id*/) const {
        return std::nullopt;
    }
    virtual std::optional<float> entityFood(const std::string& /*modName*/,
                                            std::uint64_t /*id*/) const {
        return std::nullopt;
    }
    virtual std::optional<Vector3Data> entityMovementVelocity(const std::string& /*modName*/,
                                                              std::uint64_t /*id*/) const {
        return std::nullopt;
    }
    virtual std::optional<std::vector<std::optional<ItemStackData>>> entityInventory(
        const std::string& /*modName*/, std::uint64_t /*id*/) const {
        return std::nullopt;
    }
    virtual std::optional<std::array<std::optional<ItemStackData>, 6>> entityEquippable(
        const std::string& /*modName*/, std::uint64_t /*id*/) const {
        return std::nullopt;
    }

    // Player specific
    virtual std::optional<std::string> playerName(const std::string& modName,
                                                  std::uint64_t id) const {
        auto local = localPlayerId(modName);
        if (local && *local == id) return localPlayerName();
        return std::nullopt;
    }
    virtual std::string localPlayerName() const { return "Player"; }

    virtual BackendOutcome playerSendMessage(const std::string& /*modName*/,
                                             std::uint64_t /*id*/,
                                             const std::string& text) {
        playerSendMessage(text);
        return BackendOutcome::success();
    }
    virtual void playerSendMessage(const std::string&) {}

    virtual BackendOutcome playerAddEffect(const std::string& /*modName*/,
                                           std::uint64_t /*id*/,
                                           const std::string& typeId, double seconds,
                                           int amplifier, bool showParticles) {
        if (typeId.empty() || !(seconds > 0.0))
            return BackendOutcome::error("Invalid", "An effect ID and positive duration are required");
        addEffect(typeId, seconds, amplifier, showParticles);
        return BackendOutcome::success();
    }
    virtual void addEffect(const std::string&, double /*seconds*/, int /*amplifier*/,
                           bool /*showParticles*/) {}

    virtual BackendOutcome playerRemoveEffect(const std::string& /*modName*/,
                                              std::uint64_t /*id*/,
                                              const std::string& typeId,
                                              bool& outRemoved) {
        if (typeId.empty())
            return BackendOutcome::error("Invalid", "An effect ID is required");
        removeEffect(typeId);
        outRemoved = true;
        return BackendOutcome::success();
    }
    virtual void removeEffect(const std::string&) {}

    virtual BackendOutcome playerClearEffects(const std::string& /*modName*/,
                                              std::uint64_t /*id*/) {
        clearEffects();
        return BackendOutcome::success();
    }
    virtual void clearEffects() {}

    virtual BackendOutcome playerGetEffects(const std::string& /*modName*/,
                                            std::uint64_t /*id*/,
                                            std::vector<EffectData>& outEffects) {
        outEffects.clear();
        for (const auto& [typeId, sec] : getEffects()) {
            outEffects.push_back(EffectData{typeId, sec, 0, true});
        }
        return BackendOutcome::success();
    }
    virtual std::vector<std::pair<std::string, double>> getEffects() { return {}; }

    // Client diagnostics (§ client info). These are read-only snapshots of the
    // SDK sampler and are not failures when unavailable: they read zero/empty
    // for a non-local player or a client without the sampler installed.
    virtual double playerFps(const std::string& /*modName*/, std::uint64_t /*id*/) const {
        return 0.0;
    }
    virtual double playerPing(const std::string& /*modName*/, std::uint64_t /*id*/) const {
        return 0.0;
    }
    virtual CpsData playerCps(const std::string& /*modName*/, std::uint64_t /*id*/) const {
        return {};
    }
    virtual DeviceData playerDevice(const std::string& /*modName*/, std::uint64_t /*id*/) const {
        return {};
    }

    // --- Dimension & Block ---
    virtual std::optional<std::string> dimensionGetBlockType(
        const std::string& /*modName*/, const std::string& /*dimId*/,
        BlockPosData /*pos*/) const {
        return std::nullopt;
    }

    virtual std::vector<std::uint64_t> dimensionGetEntityIds(
        const std::string& modName, const std::string& /*dimId*/) const {
        return allPlayerIds(modName);
    }

    virtual BackendOutcome dimensionSpawnEntity(
        const std::string& /*modName*/, const std::string& /*dimId*/,
        const std::string& /*typeId*/, Vector3Data /*pos*/,
        std::uint64_t& outEntityId) {
        outEntityId = 0;
        return BackendOutcome::error("Unavailable", "Entity creation is not available in this client");
    }

    virtual BackendOutcome blockSetType(
        const std::string& /*modName*/, const std::string& /*dimId*/,
        BlockPosData /*pos*/, const std::string& /*typeId*/) {
        return BackendOutcome::error("Unavailable", "Direct block mutation is not available in this client");
    }

    // --- Settings / Keybinds / Brightness bridges (§5.2 / §7.3) ---
    virtual BackendOutcome registerSettingsPage(
        const std::string& /*modName*/, const bedrock::sdk::SettingsPage& /*page*/) {
        return BackendOutcome::success();
    }
    virtual BackendOutcome unregisterSettingsPage(
        const std::string& /*modName*/, const bedrock::sdk::SettingsPage& /*page*/) {
        return BackendOutcome::success();
    }
    virtual BackendOutcome readSettingValue(
        const std::string& /*modName*/, const bedrock::sdk::SettingsPage& page,
        std::string_view settingId, bedrock::sdk::SettingValue& outValue) {
        auto res = page.value(settingId);
        if (!res)
            return BackendOutcome::error(
                errorCodeName(static_cast<int>(res.error().code)), res.error().message);
        outValue = res.value();
        return BackendOutcome::success();
    }

    virtual BackendOutcome keybindAdd(
        const std::string& /*modName*/, const std::string& /*id*/,
        const std::string& /*labelKey*/, bedrock::sdk::Key /*defaultKey*/) {
        return BackendOutcome::success();
    }
    virtual void keybindLocale(
        const std::string& /*modName*/, const std::string& /*localeId*/,
        const std::vector<bedrock::sdk::Translation>& /*entries*/) {}
    virtual std::optional<bedrock::sdk::Key> keybindKey(
        const std::string& /*modName*/, std::string_view /*id*/) const {
        return std::nullopt;
    }

    virtual BackendOutcome setBrightnessGamma(
        const std::string& /*modName*/, std::optional<float> gamma) {
        if (gamma && (!std::isfinite(*gamma) || *gamma < 0.0f || *gamma > 25.0f))
            return BackendOutcome::error("Invalid", "Gamma must be finite and in 0..25");
        return BackendOutcome::success();
    }

private:
    mutable std::map<std::string, std::map<std::string, DynamicPropertyData>> fallbackWorldProps_;
    mutable std::map<std::string, std::map<std::uint64_t, std::map<std::string, DynamicPropertyData>>> fallbackEntityProps_;
};

struct SettingsDecl {
    std::string titleKey;
    std::string modNameKey;
    std::vector<std::string> added;
    std::vector<bedrock::sdk::Setting> controls;
    std::vector<bedrock::sdk::Locale> locales;
    bool registered = false;
};

struct KeybindEntryDecl {
    std::string id;
    std::string labelKey;
    bedrock::sdk::Key defaultKey = bedrock::sdk::Key::C;
};

struct KeybindsDecl {
    std::vector<KeybindEntryDecl> bindings;
    std::vector<bedrock::sdk::Locale> locales;
};

std::optional<bedrock::sdk::Key> parseKeyName(std::string_view name);
std::string keyToName(bedrock::sdk::Key key);

// Collects JS builder declarations (§6.4) and owns the JS handler callbacks
// until the realm is destroyed.
class CommandRegistry {
public:
    explicit CommandRegistry(JsRuntime& runtime) : runtime_(runtime) {}
    ~CommandRegistry();
    CommandRegistry(const CommandRegistry&) = delete;
    CommandRegistry& operator=(const CommandRegistry&) = delete;

    int beginCommand(const std::string& name, const std::string& description);
    bool unregisterCommand(const std::string& name);
    void addBranch(int builderId, const std::vector<std::string>& literals,
                   const std::vector<std::pair<std::string, ArgSpec>>& args,
                   JSValue handler);
    const std::vector<CommandDecl>& declarations() const noexcept { return declarations_; }
    JSValue handler(std::size_t index) const;

    void setDescription(const std::string& description) { description_ = description; }
    const std::string& description() const noexcept { return description_; }

private:
    struct Builder {
        std::size_t command = 0;
    };
    JsRuntime& runtime_;
    std::string description_;
    std::vector<CommandDecl> declarations_;
    std::vector<Builder> builders_;
    std::vector<JSValue> handlers_;
};

// Installs the frozen apiVersion 1.0.0 binding surface (§5.2) into a realm.
class SdkBindings {
public:
    SdkBindings(JsRuntime& runtime, HostContext& host, CommandRegistry& commands,
                WorldBackend& world, std::string modName = {}, fs::path dataRoot = {});
    ~SdkBindings();
    SdkBindings(const SdkBindings&) = delete;
    SdkBindings& operator=(const SdkBindings&) = delete;

    bool install(std::string& error);
    JSValue moduleObject() const noexcept { return module_.get(); }
    void advanceTick();

    // Lifecycle & event delivery from host
    void notifyPlayerSpawn(std::uint64_t playerId, bool initialSpawn = true);
    void notifyKeybind(std::string_view id, bool pressed);
    void enqueueSettingChange(std::string id, bedrock::sdk::SettingValue value);
    void forceCleanup();

    const SettingsDecl& settings() const noexcept { return settings_; }
    const KeybindsDecl& keybinds() const noexcept { return keybinds_; }
    std::optional<float> lastGamma() const noexcept { return lastGamma_; }
    bool hasGammaOverride() const noexcept { return hasGammaOverride_; }

private:
    enum class Op {
        BeginCommand, UnregisterCommand, Executes, SetDescription, CreateScope,
        WorldHasPlayer, WorldLocalPlayer, WorldGetAllPlayers,
        WorldCurrentDimension, WorldGetDimension,
        WorldSendMessage, WorldGetDynamicProperty, WorldSetDynamicProperty,
        WorldSubscribePlayerSpawn, WorldUnsubscribePlayerSpawn,
        EntityIsValid, EntityTypeId, EntityLocation, EntityRotation,
        EntityDimension, EntityGetItemStack,
        EntityTeleport, EntityAddTag,
        EntityGetDynamicProperty, EntitySetDynamicProperty,
        EntityGetComponent,
        PlayerName, PlayerSendMessage,
        PlayerAddEffect, PlayerRemoveEffect, PlayerClearEffects, PlayerGetEffects,
        PlayerFps, PlayerPing, PlayerCps, PlayerDevice,
        DimensionTypeId, DimensionGetBlock, DimensionGetEntities, DimensionSpawnEntity,
        BlockTypeId, BlockLocation, BlockSetType,
        SystemRun, SystemRunTimeout, SystemRunInterval, SystemClear,
        SettingsAdd, SettingsLocale, SettingsOnChange, SettingsModName,
        SettingsRegisterPage, SettingsUnregisterPage, SettingsValue,
        KeybindAdd, KeybindLocale, KeybindKey,
        KeybindSubscribe, KeybindUnsubscribe,
        BrightnessSetGamma,
        FilesDataDir, FilesReadText, FilesWriteText, FilesAppendText,
        FilesReadBytes, FilesWriteBytes, FilesAppendBytes, FilesExists, FilesStat, FilesList,
        FilesMkdir, FilesRemove, FilesRename, FilesCopy,
        WorldObject, SystemObject, BrightnessObject,
        // --- dev namespace ---
        DevMemoryReadBytes, DevMemoryReadU32, DevMemoryReadI64, DevMemoryReadF32,
        DevMemoryReadPtr, DevMemoryWriteBytes, DevMemoryScanAob,
        DevSymbolsList, DevSymbolsStatus,
        DevModuleInfo
    };
    static JSValue hostCallback(JSContext* ctx, JSValueConst self, int argc,
                                JSValueConst* argv, int magic);

    void drainPendingSettings();
    JSValue throwBackendError(JSContext* ctx, const BackendOutcome& outcome);

    // Per-Mod file surface: one implementation shared by all realms, rooted at
    // <dataRoot>/<ModName> and created lazily on first use.
    bool ensureFilesRoot(std::string& error);
    std::optional<fs::path> resolveFilesPath(const std::string& relative, std::string& error) const;
    bool readFileBytes(const fs::path& path, std::vector<std::uint8_t>& out,
                       std::string& error) const;
    bool writeFileBytes(const fs::path& path, const std::vector<std::uint8_t>& data,
                        bool append, std::string& error);

    JsRuntime& runtime_;
    HostContext& host_;
    CommandRegistry& commands_;
    WorldBackend& world_;
    std::string modName_;
    fs::path filesRoot_;
    JsValue module_;
    JsValue worldObject_;
    JsValue systemObject_;
    JsValue brightnessObject_;
    SettingsDecl settings_;
    KeybindsDecl keybinds_;
    std::unique_ptr<bedrock::sdk::SettingsPage> activePage_;
    JSValue settingsOnChange_ = JS_UNDEFINED;

    struct PendingSettingChange {
        std::string id;
        bedrock::sdk::SettingValue value;
    };
    std::mutex pendingSettingsMutex_;
    std::deque<PendingSettingChange> pendingSettings_;
    std::shared_ptr<bool> aliveToken_ = std::make_shared<bool>(true);

    std::uint64_t nextSubId_ = 1;
    std::map<std::uint64_t, JSValue> playerSpawnSubscribers_;
    std::map<std::uint64_t, JSValue> keybindSubscribers_;

    bool hasGammaOverride_ = false;
    std::optional<float> lastGamma_;

    unsigned tick_ = 0;
    std::uint64_t nextTaskId_ = 1;
    struct Task {
        JSValue function;
        std::uint64_t period = 0;
        unsigned due = 0;
    };
    std::map<std::uint64_t, Task> tasks_;
};

} // namespace stageb
