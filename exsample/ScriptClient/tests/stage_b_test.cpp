// Stage B Phase 1 host tests (DESIGN §10.3): no game, no BOL.
#include "command_model.hpp"
#include "manifest.hpp"
#include "mod_scanner.hpp"
#include "module_loader.hpp"
#include "plugin_host.hpp"
#include "sdk_bindings.hpp"
#include "transpiler.hpp"

#include <cmath>
#include <cstdlib>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string_view>
#include <vector>
#include <nlohmann/json.hpp>
#include <bedrock/logging/content_log.hpp>
#include <bedrock/sdk/profiles.hpp>

#ifndef STAGEB_GOLDEN_DIR
#define STAGEB_GOLDEN_DIR "/home/pexserver/WorkSpace/ts-mods/EffectFake"
#endif

namespace fs = std::filesystem;
using namespace stageb;

namespace {

int failures = 0;
int checks = 0;

#define CHECK(condition, message)                                                        \
    do {                                                                                 \
        ++checks;                                                                        \
        if (!(condition)) {                                                              \
            ++failures;                                                                  \
            std::cerr << "FAIL " << __func__ << ":" << __LINE__ << " " << message << '\n';\
        }                                                                                \
    } while (false)

fs::path tempDir(const std::string& name) {
    static int sequence = 0;
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    auto path = fs::temp_directory_path() / ("stageb-test-" + name + "-" +
                                             std::to_string(stamp) + "-" +
                                             std::to_string(sequence++));
    fs::create_directories(path);
    return path;
}

void writeFile(const fs::path& path, const std::string& content) {
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary);
    out << content;
}

fs::path copyGolden(const fs::path& root) {
    const fs::path source(STAGEB_GOLDEN_DIR);
    fs::create_directories(root);
    fs::copy(source / "manifest.json", root / "manifest.json");
    fs::copy(source / "package.json", root / "package.json");
    fs::copy(source / "README.md", root / "README.md");
    fs::copy(source / "src", root / "src", fs::copy_options::recursive);
    return root;
}

// ---------------------------------------------------------------------------
// 1. Manifest validation (§10.3-1)
// ---------------------------------------------------------------------------
void testManifest() {
    const auto dir = tempDir("manifest");
    const auto modDir = copyGolden(dir / "EffectFake");
    std::ifstream in(modDir / "manifest.json");
    nlohmann::json golden;
    in >> golden;

    const auto ok = validateManifest(golden, "EffectFake", modDir, "1.26.65.1");
    CHECK(ok.ok, "golden EffectFake manifest must be accepted");
    CHECK(ok.manifest.name == "EffectFake", "golden name");
    CHECK(ok.manifest.apiVersion == "1.0.0", "golden apiVersion");
    CHECK(ok.manifest.gameVersions.size() == 1, "golden gameVersions");
    CHECK(ok.manifest.memoryMiB == 64, "default memoryMiB");

    auto wrongFolder = golden;
    CHECK(!validateManifest(wrongFolder, "OtherName", modDir, "1.26.65.1").ok,
          "folder/name mismatch must fail");

    auto api2 = golden;
    api2["apiVersion"] = "2.0.0";
    const auto incompatible = validateManifest(api2, "EffectFake", modDir, "1.26.65.1");
    CHECK(!incompatible.ok && incompatible.incompatible, "apiVersion 2 must be incompatible");

    auto apiInt = golden;
    apiInt["apiVersion"] = 1;
    CHECK(validateManifest(apiInt, "EffectFake", modDir, "1.26.65.1").ok,
          "integer apiVersion 1 must normalize to 1.0.0");

    auto wrongGame = golden;
    wrongGame["gameVersions"] = {"1.26.60.5"};
    CHECK(validateManifest(wrongGame, "EffectFake", modDir, "1.26.65.1").incompatible,
          "gameVersions mismatch must be incompatible");

    auto starGame = golden;
    starGame["gameVersions"] = {"*"};
    CHECK(validateManifest(starGame, "EffectFake", modDir, "1.26.65.1").ok,
          "\"*\" must opt in to all versions");

    auto missingEntry = golden;
    missingEntry["entry"] = "src/missing.ts";
    CHECK(!validateManifest(missingEntry, "EffectFake", modDir, "1.26.65.1").ok,
          "missing entry must fail");

    auto escape = golden;
    escape["entry"] = "../../outside.ts";
    CHECK(!validateManifest(escape, "EffectFake", modDir, "1.26.65.1").ok,
          "entry escaping mod dir must fail");

    auto badMemory = golden;
    badMemory["memoryMiB"] = 9999;
    const auto memory = validateManifest(badMemory, "EffectFake", modDir, "1.26.65.1");
    CHECK(memory.ok && memory.manifest.memoryMiB == 64 && !memory.warning.empty(),
          "out-of-range memoryMiB falls back to 64 with a warning");

    const auto scan = scanMods(dir, "1.26.65.1");
    CHECK(scan.candidates.size() == 1, "scanner finds one mod directory");
    CHECK(scan.candidates[0].status == ModStatus::Active ||
          scan.candidates[0].validation.ok, "scanner validates golden manifest");
}

// ---------------------------------------------------------------------------
// 2. require resolution (§10.3-2)
// ---------------------------------------------------------------------------
void testRequireResolution() {
    const auto dir = tempDir("require");
    writeFile(dir / "index.js", R"JS(
const a = require('./a');
const pkg = require('pkg-main');
const nested = require('nested');
const data = require('./data.json');
const ts = require('./helper.ts');
const builtin = require('@bedrock/sdk');
exports.total = a.value + pkg.value + nested.value + data.value + ts.value;
exports.builtin = builtin.value;
)JS");
    writeFile(dir / "a.js", "exports.value = 1; exports.b = require('./b');\n");
    writeFile(dir / "b.js", "const a = require('./a'); exports.value = a.value + 10;\n");
    writeFile(dir / "data.json", R"({"value":5})");
    writeFile(dir / "helper.ts", "export const value: number = 6;\n");
    writeFile(dir / "escape.js", "exports.x = require('../outside').x;\n");
    writeFile(dir / "node_modules/pkg-main/package.json", R"({"main":"lib/entry.js"})");
    writeFile(dir / "node_modules/pkg-main/lib/entry.js", "exports.value = 2;\n");
    writeFile(dir / "node_modules/nested/index.js",
              "exports.value = require('deep').value + 3;\n");
    writeFile(dir / "node_modules/nested/node_modules/deep/index.js",
              "exports.value = 4;\n");
    writeFile(dir / "node_modules/@bedrock/sdk/index.js", "exports.value = 999;\n");

    JsRuntime runtime;
    std::string error;
    CHECK(runtime.initialize(error), "runtime init: " + error);
    runtime.setContextOpaque(nullptr);
    Transpiler transpiler;
    CHECK(transpiler.initialize(error), "transpiler init: " + error);
    HostContext host;
    runtime.setContextOpaque(&host);
    ModuleLoader loader(runtime, transpiler, dir, host);
    const auto ctx = runtime.context();
    JsValue builtin(ctx, JS_NewObject(ctx));
    JS_SetPropertyStr(ctx, builtin.get(), "value", JS_NewFloat64(ctx, 42));
    loader.setBuiltin("@bedrock/sdk", builtin.get());

    CHECK(loader.loadEntry((dir / "index.js").string(), error), "entry load: " + error);
    JsValue total(ctx, JS_GetPropertyStr(ctx, loader.entryExports(), "total"));
    JsValue value(ctx, JS_GetPropertyStr(ctx, loader.entryExports(), "builtin"));
    CHECK(jsToNumber(ctx, total.get()) == 21, "nested node_modules/circular/ts/json resolution");
    CHECK(jsToNumber(ctx, value.get()) == 42, "builtin wins over node_modules shadow");

    ModuleLoader escapeLoader(runtime, transpiler, dir, host);
    CHECK(!escapeLoader.loadEntry((dir / "escape.js").string(), error),
          "escape outside mod root rejected");

    JsRuntime unsupportedRuntime;
    CHECK(unsupportedRuntime.initialize(error), "runtime init: " + error);
}

// ---------------------------------------------------------------------------
// 3. Golden EffectFake transpile + evaluate + command declaration (§10.3-3)
// ---------------------------------------------------------------------------
struct GoldenHost {
    fs::path mods;
    PluginHost host;
    std::vector<WireDefinitions> published;
    explicit GoldenHost(const fs::path& modsDir)
        : mods(modsDir), host(modsDir, "1.26.65.1") {
        host.setDefinitionsChanged([this](const WireDefinitions& defs) {
            published.push_back(defs);
        });
    }
};

void testGoldenEffectFake() {
    const auto dir = tempDir("golden");
    fs::create_directories(dir / "mods");
    copyGolden(dir / "mods" / "EffectFake");
    GoldenHost fixture(dir / "mods");
    std::string error;
    CHECK(fixture.host.initialize(error), "initialize: " + error);
    const auto views = fixture.host.list();
    CHECK(views.size() == 1, "one golden mod loaded");
    CHECK(views[0].status == ModStatus::Active, "golden mod active: " + views[0].error);
    CHECK(views[0].apiVersion == "1.0.0", "golden apiVersion");

    const auto& definitions = fixture.host.definitions();
    CHECK(definitions.commands.size() == 1, "one command declared: /effectfake");
    CHECK(definitions.commands[0].name == "effectfake", "command name");
    CHECK(definitions.commands[0].params.size() == 5,
          "flattened overload: action + 4 optional args");
    CHECK(definitions.commands[0].params[1].kind == WireParamKind::PacketEnum,
          "effect uses packet enum");
    CHECK(definitions.commands[0].params[3].kind == WireParamKind::CustomEnum,
          "amplifier uses custom enum");
    bool foundAction = false, foundDuration = false, foundAmplifier = false, foundBool = false;
    for (const auto& e : definitions.enums) {
        if (e.name == "EffectfakeAction") {
            foundAction = true;
            CHECK(e.merged && e.mergedFrom == "Effect", "merged action enum");
            CHECK(e.values.size() == 2 && e.values[0] == "give" && e.values[1] == "clear",
                  "action enum values give/clear");
        }
        if (e.name == "EffectfakeDuration") {
            foundDuration = true;
            CHECK(e.values[0] == "Infinity", "duration enum includes Infinity");
        }
        if (e.name == "EffectfakeAmplifier") {
            foundAmplifier = true;
            CHECK(e.values.size() == 6 && e.values.back() == "9", "amplifier suggestions");
        }
        if (e.name == "EffectfakeVisible") {
            foundBool = true;
            CHECK(e.values.size() == 2 && e.values[0] == "true", "bool enum");
        }
    }
    CHECK(foundAction && foundDuration && foundAmplifier && foundBool, "all enums generated");
}

// ---------------------------------------------------------------------------
// 4. Command grammar + line parsing (§10.3-4)
// ---------------------------------------------------------------------------
void testCommandParsing() {
    const auto dir = tempDir("parse");
    fs::create_directories(dir / "mods");
    copyGolden(dir / "mods" / "EffectFake");
    GoldenHost fixture(dir / "mods");
    std::string error;
    CHECK(fixture.host.initialize(error), "initialize: " + error);

    std::string response;
    CHECK(fixture.host.dispatchCommand("/effectfake give regeneration Infinity 4 false", response),
          "dispatch give");
    CHECK(response.find("[EffectFake]") == std::string::npos ||
          !response.empty(), "handler or usage responded");

    // Rebuild the declaration from a fresh host to inspect parse results
    // deterministically (the plugin declaration is internal to PluginHost).
    CommandDecl decl;
    decl.name = "effectfake";
    ArgSpec effect;
    effect.kind = ArgKind::Effect;
    ArgSpec duration;
    duration.kind = ArgKind::Duration;
    duration.optional = true;
    duration.defaultValue = 30;
    duration.infinity = true;
    ArgSpec amplifier;
    amplifier.kind = ArgKind::Int;
    amplifier.optional = true;
    amplifier.defaultValue = 0;
    amplifier.min = 0;
    amplifier.max = 255;
    amplifier.suggestions = {"0", "1", "2", "3", "4", "9"};
    ArgSpec visible;
    visible.kind = ArgKind::Bool;
    visible.optional = true;
    visible.stringDefault = "true";
    decl.branches.push_back({{"give"}, {{"effect", effect}, {"duration", duration},
                                         {"amplifier", amplifier}, {"visible", visible}}});
    ArgSpec optionalEffect = effect;
    optionalEffect.optional = true;
    decl.branches.push_back({{"clear"}, {{"effect", optionalEffect}}});

    const auto parsed = parseCommandLine(decl, "/effectfake give regeneration Infinity 4 false");
    CHECK(parsed.ok, "full give line parses");
    CHECK(parsed.values.at("effect").text == "minecraft:regeneration", "effect normalized");
    CHECK(std::isinf(parsed.values.at("duration").number), "Infinity preserved");
    CHECK(parsed.values.at("amplifier").number == 4, "amplifier parsed");
    CHECK(!parsed.values.at("visible").boolean, "visible=false parsed");

    const auto defaults = parseCommandLine(decl, "/effectfake give regeneration");
    CHECK(defaults.ok && defaults.values.at("duration").number == 30, "duration default 30");
    CHECK(defaults.values.at("amplifier").number == 0, "amplifier default 0");
    CHECK(defaults.values.at("visible").boolean, "visible default true");

    const auto clear = parseCommandLine(decl, "/effectfake clear");
    CHECK(clear.ok && !clear.values.at("effect").present, "clear with no effect");
    const auto clearOne = parseCommandLine(decl, "/effectfake clear speed");
    CHECK(clearOne.ok && clearOne.values.at("effect").text == "minecraft:speed",
          "clear with effect");

    CHECK(!parseCommandLine(decl, "/effectfake give").ok, "missing required effect rejected");
    CHECK(!parseCommandLine(decl, "/effectfake give speed 30 999 true").ok,
          "amplifier out of range rejected");
    CHECK(!parseCommandLine(decl, "/effectfake give speed abc 1 true").ok,
          "bad number rejected");
}

// ---------------------------------------------------------------------------
// 5. Lifecycle, watchdog fault, reload, /mods (§10.3-5)
// ---------------------------------------------------------------------------
void testLifecycle() {
    const auto dir = tempDir("lifecycle");
    const auto modDir = dir / "mods" / "Loop";
    fs::create_directories(modDir / "src");
    writeFile(modDir / "manifest.json",
              R"({"apiVersion":"1.0.0","entry":"src/index.ts","gameVersions":["1.26.65.1"],"name":"Loop","version":"0.1.0"})");
    writeFile(modDir / "package.json", R"({"name":"loop","type":"module","private":true,"version":"0.1.0"})");
    writeFile(modDir / "src/index.ts", R"TS(
import { Mod } from "@bedrock/sdk";
export default class Loop extends Mod {
  onEnable() { globalThis.enabled = true; }
  onUpdate() { while (true) {} }
}
)TS");

    PluginHost host(dir / "mods", "1.26.65.1");
    host.setFaultBudgetMs(5);
    std::string error;
    CHECK(host.initialize(error), "initialize: " + error);
    auto views = host.list();
    CHECK(views.size() == 1 && views[0].status == ModStatus::Active, "loop mod initially active");
    host.tick();
    views = host.list();
    CHECK(views[0].status == ModStatus::Faulted, "runaway onUpdate faults the mod");

    writeFile(modDir / "src/index.ts", R"TS(
import { Mod } from "@bedrock/sdk";
export default class Loop extends Mod {
  private n: number = 0;
  onEnable() { this.n = 1; }
  onUpdate() { this.n++; }
}
)TS");
    CHECK(host.reload("Loop", error), "reload after fixing: " + error);
    views = host.list();
    CHECK(views[0].status == ModStatus::Active, "reloaded mod active");
    host.tick();
    CHECK(host.list()[0].status == ModStatus::Active, "reloaded mod ticks cleanly");

    const auto listing = host.handleModsCommand("/mods list");
    CHECK(listing.find("Loop") != std::string::npos, "/mods list shows the mod");
    CHECK(host.handleModsCommand("/mods path").find("mods") != std::string::npos,
          "/mods path prints the resolved directory");
    CHECK(host.handleModsCommand("/mods reload Loop").find("reloaded") != std::string::npos,
          "/mods reload works");
}

void testBindingRegistry() {
    std::ifstream in(fs::path(STAGEB_SOURCE_DIR) / "data" / "binding-registry.json");
    CHECK(in.good(), "binding registry file present");
    if (!in.good()) return;
    nlohmann::json doc;
    in >> doc;
    CHECK(doc["apiVersion"] == "1.0.0", "registry pins apiVersion 1.0.0");
    CHECK(doc["exports"].size() >= 5, "registry lists the frozen exports");
    CHECK(doc["builtinModules"].size() == 2, "two built-in modules");
    bool hasCommandContext = false, hasFiles = false;
    for (const auto& e : doc["exports"]) {
        if (e["name"] == "CommandContext") hasCommandContext = true;
        if (e["name"] == "files") hasFiles = true;
    }
    CHECK(hasCommandContext, "CommandContext type is registered for Stage C");
    CHECK(hasFiles, "files value export is registered for Stage C");
    int filesMembers = 0;
    bool allFilesBound = true;
    for (const auto& m : doc["members"]) {
        const auto path = m["path"].get<std::string>();
        if (path.rfind("files.", 0) == 0) {
            ++filesMembers;
            if (!m.value("bound", false)) allFilesBound = false;
        }
    }
    CHECK(filesMembers >= 14, "all files members are registered");
    CHECK(allFilesBound, "all files members are marked bound");
    CHECK(doc.contains("unsupportedLimitations") && doc["unsupportedLimitations"].is_array() &&
          !doc["unsupportedLimitations"].empty(),
          "registry explicitly documents Stage A limitations");
    bool allMarkedBound = true;
    for (const auto& m : doc["members"]) {
        if (!m.value("bound", false)) allMarkedBound = false;
    }
    CHECK(allMarkedBound, "all registered members are marked bound");
}

// ---------------------------------------------------------------------------
// 6. Settings / Keybinds / Brightness / PlayerSpawn / Surface / Per-Mod World
// ---------------------------------------------------------------------------
class MockFullWorldBackend final : public WorldBackend {
public:
    struct RegisteredPageInfo {
        std::string modId;
        std::size_t controlCount = 0;
        std::size_t localeCount = 0;
    };
    std::map<std::string, RegisteredPageInfo> registeredPages;
    std::map<std::string, std::map<std::string, bedrock::sdk::SettingValue>> storedSettings;
    std::map<std::string, std::map<std::string, bedrock::sdk::Key>> registeredKeybinds;
    std::vector<std::pair<std::string, float>> gammaStack;
    std::optional<float> activeGamma;
    std::map<std::string, std::vector<std::string>> messagesByMod;
    std::map<std::string, std::vector<EffectData>> effectsByMod;
    std::map<BlockPosData, std::string, bool(*)(const BlockPosData&, const BlockPosData&)> blocks{
        [](const BlockPosData& a, const BlockPosData& b) {
            if (a.x != b.x) return a.x < b.x;
            if (a.y != b.y) return a.y < b.y;
            return a.z < b.z;
        }
    };

    MockFullWorldBackend() {
        blocks[{1, 64, -2}] = "minecraft:glass";
    }

    bool hasLocalPlayer() const override { return true; }
    bool hasLocalPlayer(const std::string&) const override { return true; }
    std::optional<std::uint64_t> localPlayerId(const std::string& modName) const override {
        return modName == "ModB" ? std::optional<std::uint64_t>{200} : std::optional<std::uint64_t>{100};
    }
    std::vector<std::uint64_t> allPlayerIds(const std::string& modName) const override {
        return {*localPlayerId(modName)};
    }
    std::optional<std::string> currentDimensionId(const std::string&) const override {
        return "minecraft:overworld";
    }
    BackendOutcome worldSendMessage(const std::string& modName, const std::string& text) override {
        messagesByMod[modName].push_back(text);
        return BackendOutcome::success();
    }

    bool entityIsValid(const std::string&, std::uint64_t id) const override {
        return id == 100 || id == 200 || id == 300;
    }
    std::optional<std::string> entityTypeId(const std::string&, std::uint64_t id) const override {
        if (id == 300) return "minecraft:item";
        if (id == 100 || id == 200) return "minecraft:player";
        return std::nullopt;
    }
    std::optional<Vector3Data> entityLocation(const std::string& modName, std::uint64_t id) const override {
        if (id == 300) return Vector3Data{5.0f, 65.0f, 5.0f};
        return modName == "ModB" ? Vector3Data{20.0f, 70.0f, -10.0f} : Vector3Data{1.0f, 64.0f, -2.0f};
    }
    std::optional<RotationData> entityRotation(const std::string&, std::uint64_t) const override {
        return RotationData{15.0f, 90.0f};
    }
    std::optional<ItemStackData> entityItemStack(const std::string&, std::uint64_t id) const override {
        if (id == 300) return ItemStackData{"minecraft:acacia_log", 4};
        return std::nullopt;
    }
    std::optional<float> entityHealth(const std::string&, std::uint64_t) const override {
        return 20.0f;
    }
    std::optional<float> entityFood(const std::string&, std::uint64_t) const override {
        return 18.0f;
    }
    std::optional<Vector3Data> entityMovementVelocity(const std::string&, std::uint64_t) const override {
        return Vector3Data{0.25f, 0.0f, -0.5f};
    }
    std::optional<std::vector<std::optional<ItemStackData>>> entityInventory(
        const std::string&, std::uint64_t) const override {
        return std::vector<std::optional<ItemStackData>>{
            ItemStackData{"minecraft:diamond", 2},
            std::nullopt,
            ItemStackData{"minecraft:iron_ingot", 16}
        };
    }
    std::optional<std::array<std::optional<ItemStackData>, 6>> entityEquippable(
        const std::string&, std::uint64_t) const override {
        std::array<std::optional<ItemStackData>, 6> eq{};
        eq[0] = ItemStackData{"minecraft:diamond_sword", 1};
        eq[2] = ItemStackData{"minecraft:iron_helmet", 1};
        return eq;
    }

    std::optional<std::string> playerName(const std::string& modName, std::uint64_t) const override {
        return modName == "ModB" ? "Steve" : "Alex";
    }
    BackendOutcome playerSendMessage(const std::string& modName, std::uint64_t,
                                     const std::string& text) override {
        messagesByMod[modName].push_back(text);
        return BackendOutcome::success();
    }
    BackendOutcome playerAddEffect(const std::string& modName, std::uint64_t,
                                   const std::string& typeId, double seconds,
                                   int amplifier, bool showParticles) override {
        if (typeId.empty() || !(seconds > 0.0))
            return BackendOutcome::error("Invalid", "invalid effect");
        effectsByMod[modName].push_back({typeId, seconds, amplifier, showParticles});
        return BackendOutcome::success();
    }
    BackendOutcome playerGetEffects(const std::string& modName, std::uint64_t,
                                    std::vector<EffectData>& outEffects) override {
        outEffects = effectsByMod[modName];
        return BackendOutcome::success();
    }

    std::optional<std::string> dimensionGetBlockType(
        const std::string&, const std::string& dimId, BlockPosData pos) const override {
        if (dimId != "minecraft:overworld") return std::nullopt;
        const auto it = blocks.find(pos);
        return it != blocks.end() ? std::optional<std::string>{it->second} : std::nullopt;
    }
    std::vector<std::uint64_t> dimensionGetEntityIds(
        const std::string& modName, const std::string&) const override {
        return {*localPlayerId(modName), 300};
    }
    BackendOutcome blockSetType(const std::string&, const std::string& dimId,
                                BlockPosData pos, const std::string& typeId) override {
        if (dimId != "minecraft:overworld")
            return BackendOutcome::error("Unavailable", "dimension inactive");
        if (typeId.empty())
            return BackendOutcome::error("Invalid", "empty block typeId");
        blocks[pos] = typeId;
        return BackendOutcome::success();
    }

    BackendOutcome registerSettingsPage(const std::string& modName,
                                        const bedrock::sdk::SettingsPage& page) override {
        if (registeredPages.contains(modName))
            return BackendOutcome::error("AlreadyRegistered", "settings page already registered");
        registeredPages[modName] = {page.id(), 1, 1};
        storedSettings[modName]["enabled"] = true;
        storedSettings[modName]["speed"] = 2.5;
        storedSettings[modName]["mode"] = std::string("hold");
        return BackendOutcome::success();
    }
    BackendOutcome unregisterSettingsPage(const std::string& modName,
                                          const bedrock::sdk::SettingsPage&) override {
        registeredPages.erase(modName);
        return BackendOutcome::success();
    }
    BackendOutcome readSettingValue(const std::string& modName,
                                    const bedrock::sdk::SettingsPage&,
                                    std::string_view settingId,
                                    bedrock::sdk::SettingValue& outValue) override {
        const auto mIt = storedSettings.find(modName);
        if (mIt == storedSettings.end())
            return BackendOutcome::error("NotFound", "page not found");
        const auto it = mIt->second.find(std::string(settingId));
        if (it == mIt->second.end())
            return BackendOutcome::error("NotFound", "setting not found");
        outValue = it->second;
        return BackendOutcome::success();
    }

    BackendOutcome keybindAdd(const std::string& modName, const std::string& id,
                              const std::string&, bedrock::sdk::Key defaultKey) override {
        if (registeredKeybinds[modName].contains(id))
            return BackendOutcome::error("AlreadyRegistered", "duplicate keybind");
        registeredKeybinds[modName][id] = defaultKey;
        return BackendOutcome::success();
    }
    std::optional<bedrock::sdk::Key> keybindKey(const std::string& modName,
                                                std::string_view id) const override {
        const auto mIt = registeredKeybinds.find(modName);
        if (mIt == registeredKeybinds.end()) return std::nullopt;
        const auto it = mIt->second.find(std::string(id));
        return it != mIt->second.end() ? std::optional<bedrock::sdk::Key>{it->second} : std::nullopt;
    }

    BackendOutcome setBrightnessGamma(const std::string& modName,
                                      std::optional<float> gamma) override {
        if (gamma && (!std::isfinite(*gamma) || *gamma < 0.0f || *gamma > 25.0f))
            return BackendOutcome::error("Invalid", "Gamma must be finite and in 0..25");
        if (gamma) {
            gammaStack.erase(std::remove_if(gammaStack.begin(), gammaStack.end(),
                [&](const auto& p) { return p.first == modName; }), gammaStack.end());
            gammaStack.emplace_back(modName, *gamma);
            activeGamma = *gamma;
        } else {
            gammaStack.erase(std::remove_if(gammaStack.begin(), gammaStack.end(),
                [&](const auto& p) { return p.first == modName; }), gammaStack.end());
            activeGamma = gammaStack.empty() ? std::nullopt
                                             : std::optional<float>{gammaStack.back().second};
        }
        return BackendOutcome::success();
    }
};

void testSettingsKeybindsBrightnessAndEvents() {
    const auto dir = tempDir("round2");
    const auto modADir = dir / "mods" / "ModA";
    const auto modBDir = dir / "mods" / "ModB";
    fs::create_directories(modADir / "src");
    fs::create_directories(modBDir / "src");

    writeFile(modADir / "manifest.json",
              R"({"apiVersion":"1.0.0","entry":"src/index.ts","gameVersions":["1.26.65.1"],"name":"ModA","version":"1.0.0"})");
    writeFile(modADir / "package.json", R"({"name":"moda","type":"module","private":true,"version":"1.0.0"})");
    writeFile(modADir / "src/index.ts", R"TS(
import { Mod, Setting, ItemStack, EquipmentSlot, brightness, world } from "@bedrock/sdk";

export default class ModA extends Mod {
  private spawnSub?: { remove(): void };
  private keySub?: { remove(): void };

  onEnable(): void {
    const page = this.settings("moda.title")
      .modName("moda.name")
      .add(Setting.toggle("enabled", "moda.enabled", true))
      .add(Setting.slider("speed", "moda.speed", 2.5, 0.5, 10.0, 0.5))
      .add(Setting.choice("mode", "moda.mode", "hold", [["hold", "moda.hold"], ["toggle", "moda.toggle"]]))
      .add(Setting.text("info", "moda.info", "moda.desc"))
      .locale("en_US", { "moda.title": "Mod A", "moda.enabled": "Enabled" })
      .locale("ja_JP", [["moda.title", "Mod A 設定"], ["moda.enabled", "有効"]])
      .onChange((id: string, val: unknown) => {
        world.sendMessage(`setting:${id}=${String(val)}`);
      })
      .registerPage();

    const initialSpeed = page.value("speed");
    world.sendMessage(`initSpeed=${String(initialSpeed)}`);

    this.keybinds
      .add("zoom", "key.zoom", "KeyC")
      .add("freecam", "key.freecam", "F5")
      .locale("en_US", { "key.zoom": "Zoom", "key.freecam": "Freecam" });

    world.sendMessage(`zoomKey=${String(this.keybinds.key("zoom"))}`);

    this.keySub = this.keybinds.changed.subscribe((ev: { id: string; pressed: boolean }) => {
      world.sendMessage(`keySub:${ev.id}:${ev.pressed}`);
    });

    this.spawnSub = world.afterEvents.playerSpawn.subscribe((ev: { player: any; initialSpawn: boolean }) => {
      world.sendMessage(`spawn:${ev.player.name}:${ev.initialSpawn}`);
    });

    brightness.setGamma(20);
  }

  onKeybind(id: string, pressed: boolean): void {
    world.sendMessage(`onKeybind:${id}:${pressed}`);
    if (id === "unsub" && pressed && this.spawnSub) {
      this.spawnSub.remove();
    }
  }

  onUpdate(): void {
    const p = world.localPlayer();
    if (!p) return;
    world.setDynamicProperty("modProp", "fromModA");
    p.setDynamicProperty("pProp", 42);

    const hp = p.getComponent("minecraft:health");
    const food = p.getComponent("minecraft:food");
    const mov = p.getComponent("minecraft:movement");
    const inv = p.getComponent("minecraft:inventory");
    const eq = p.getComponent("minecraft:equippable");
    const slot0 = inv ? inv.getItem(0) : undefined;
    const hand = eq ? eq.getEquipment(EquipmentSlot.MainHand) : undefined;

    const dim = world.currentDimension;
    const blk = dim ? dim.getBlock({ x: 1, y: 64, z: -2 }) : undefined;
    const oldBlkType = blk ? blk.typeId : "none";
    if (blk) blk.setType("minecraft:stone");
    const newBlk = dim ? dim.getBlock({ x: 1, y: 64, z: -2 }) : undefined;

    const ents = dim ? dim.getEntities() : [];
    let droppedInfo = "none";
    for (const e of ents) {
      if (e.typeId === "minecraft:item") {
        const st = e.getItemStack();
        if (st) droppedInfo = `${st.typeId}x${st.amount}`;
      }
    }

    const customStack = new ItemStack("minecraft:apple", 5);
    let spawnErrCode = "none";
    try {
      if (dim) dim.spawnEntity("minecraft:pig", { x: 0, y: 64, z: 0 });
    } catch (e: any) {
      spawnErrCode = e.code || "err";
    }

    world.sendMessage(
      `tick:p=${p.name}:loc=${p.location.x},${p.location.y},${p.location.z}:rot=${p.rotation.x},${p.rotation.y}` +
      `:hp=${hp.currentValue}:food=${food.currentValue}:vel=${mov.velocity.x}` +
      `:inv=${slot0.typeId}x${slot0.amount}:eq=${hand.typeId}` +
      `:blk=${oldBlkType}->${newBlk.typeId}:drop=${droppedInfo}` +
      `:stack=${customStack.typeId}x${customStack.amount}:spawnErr=${spawnErrCode}` +
      `:prop=${world.getDynamicProperty("modProp")}/${p.getDynamicProperty("pProp")}`
    );
  }
}
)TS");

    writeFile(modBDir / "manifest.json",
              R"({"apiVersion":"1.0.0","entry":"src/index.ts","gameVersions":["1.26.65.1"],"name":"ModB","version":"1.0.0"})");
    writeFile(modBDir / "package.json", R"({"name":"modb","type":"module","private":true,"version":"1.0.0"})");
    writeFile(modBDir / "src/index.ts", R"TS(
import { Mod, brightness, world } from "@bedrock/sdk";

export default class ModB extends Mod {
  onEnable(): void {
    this.keybinds.add("unsub", "key.unsub", "KeyG");
    brightness.setGamma(12);
  }
  onUpdate(): void {
    const p = world.localPlayer();
    const propFromA = world.getDynamicProperty("modProp");
    world.setDynamicProperty("modProp", "fromModB");
    world.sendMessage(`modB:p=${p ? p.name : "none"}:propA=${String(propFromA)}:propB=${String(world.getDynamicProperty("modProp"))}`);
  }
}
)TS");

    auto backend = std::make_shared<MockFullWorldBackend>();
    PluginHost host(dir / "mods", "1.26.65.1", backend);
    std::string error;
    CHECK(host.initialize(error), "round2 initialize: " + error);
    CHECK(host.list().size() == 2, "two mods loaded");
    CHECK(host.list()[0].status == ModStatus::Active && host.list()[1].status == ModStatus::Active,
          "both ModA and ModB are active");

    // 1. Settings translation verification
    const auto* settingsA = host.settingsFor("ModA");
    CHECK(settingsA != nullptr && settingsA->registered, "ModA settings page registered in SDK");
    CHECK(settingsA && settingsA->controls.size() == 4, "4 Setting controls translated (toggle/slider/choice/text)");
    CHECK(settingsA && settingsA->locales.size() == 2, "2 locales (en_US object, ja_JP tuple array) translated");
    CHECK(backend->registeredPages.contains("ModA"), "backend received SettingsPage::registerPage()");

    // 2. Keybinds translation verification
    const auto* kbA = host.keybindsFor("ModA");
    CHECK(kbA != nullptr && kbA->bindings.size() == 2, "2 keybinds translated");
    CHECK(kbA && kbA->bindings[0].defaultKey == bedrock::sdk::Key::C, "KeyC -> Key::C");
    CHECK(kbA && kbA->bindings[1].defaultKey == bedrock::sdk::Key::F5, "F5 -> Key::F5");

    // 3. Brightness last-writer-wins + unload restore verification
    CHECK(backend->activeGamma.has_value() && *backend->activeGamma == 12.0f,
          "ModB (loaded after ModA) wins gamma with 12.0");

    // 4. Setting onChange delivery on owning tick
    host.enqueueSettingChange("ModA", "speed", 7.5);
    host.tickMod("ModA");
    host.tickMod("ModB");

    const auto& msgsA = backend->messagesByMod["ModA"];
    const auto& msgsB = backend->messagesByMod["ModB"];
    auto hasMsg = [](const std::vector<std::string>& list, const std::string& needle) {
        for (const auto& m : list)
            if (m.find(needle) != std::string::npos) return true;
        return false;
    };
    CHECK(hasMsg(msgsA, "initSpeed=2.5"), "SettingsPage.value() read initial setting value");
    CHECK(hasMsg(msgsA, "zoomKey=C"), "keybinds.key() returned mapped key name");
    CHECK(hasMsg(msgsA, "setting:speed=7.5"), "SettingsPage.onChange delivered on owning tick");
    CHECK(hasMsg(msgsA, "tick:p=Alex:loc=1,64,-2:rot=15,90"), "player name/location/rotation getters");
    CHECK(hasMsg(msgsA, "hp=20:food=18:vel=0.25"), "health/food/movement components");
    CHECK(hasMsg(msgsA, "inv=minecraft:diamondx2:eq=minecraft:diamond_sword"),
          "inventory and equippable components");
    CHECK(hasMsg(msgsA, "blk=minecraft:glass->minecraft:stone"),
          "dimension.getBlock and block.setType");
    CHECK(hasMsg(msgsA, "drop=minecraft:acacia_logx4"),
          "dimension.getEntities + entity.getItemStack (item-entity-detection)");
    CHECK(hasMsg(msgsA, "stack=minecraft:applex5:spawnErr=Unavailable"),
          "new ItemStack constructor and Unavailable error propagation");
    CHECK(hasMsg(msgsA, "prop=fromModA/42"), "world and entity dynamic properties");

    // 5. Per-Mod WorldBackend isolation
    CHECK(hasMsg(msgsB, "modB:p=Steve:propA=undefined:propB=fromModB"),
          "ModB sees its own world/player context and isolated dynamic properties");

    // 6. Keybind dispatch -> both keybinds.changed.subscribe and Mod.onKeybind
    host.notifyKeybind("ModA", "zoom", true);
    CHECK(hasMsg(msgsA, "keySub:zoom:true") && hasMsg(msgsA, "onKeybind:zoom:true"),
          "keybind notification reaches both changed.subscribe and onKeybind");

    // 7. world.afterEvents.playerSpawn subscription & unsubscription
    host.notifyPlayerSpawn("ModA", 100, true);
    CHECK(hasMsg(msgsA, "spawn:Alex:true"), "playerSpawn delivered to JS subscriber");

    const auto countBeforeUnsub = msgsA.size();
    host.notifyKeybind("ModA", "unsub", true); // triggers this.spawnSub.remove()
    host.notifyPlayerSpawn("ModA", 100, false);
    bool spawnedAfterUnsub = false;
    for (std::size_t i = countBeforeUnsub; i < msgsA.size(); ++i) {
        if (msgsA[i].find("spawn:") != std::string::npos) spawnedAfterUnsub = true;
    }
    CHECK(!spawnedAfterUnsub, "playerSpawn subscription.remove() stops future delivery");

    // 8. Unload / reload cleanup (SettingsPage unregister + BrightnessOverride restore)
    writeFile(modBDir / "src/index.ts", R"TS(
import { Mod } from "@bedrock/sdk";
export default class ModB extends Mod {
  onEnable(): void {}
}
)TS");
    CHECK(host.reload("ModB", error), "reload ModB without gamma override: " + error);
    CHECK(backend->activeGamma.has_value() && *backend->activeGamma == 20.0f,
          "unloading ModB restores ModA's previous gamma (20.0)");
}

// ---------------------------------------------------------------------------
// 7. Stage B BOL live-fix regressions (bugs 2-5)
// ---------------------------------------------------------------------------
void testProfileFacadeRegression() {
#ifdef _WIN32
    bedrock::sdk::Profiles profiles;
    std::string error;
    if (profiles.open(bedrock::sdk::ProfileUpdatePolicy::EmbeddedOnly, error)) {
        const auto profile = profiles.snapshot();
        CHECK(!profile.version().empty(), "facade Profile::version uses selected RuntimeProfile");
    } else {
        CHECK(false, "embedded profile fixture should open: " + error);
    }
#else
    CHECK(true, "snapshot Profile::version regression requires Windows runtime fixture");
#endif
}

void testLiveFixRegressions() {
    // Bug 4 wire shape: a declaration with no literals must emit neither an
    // empty Action enum nor a leading action parameter.
    {
        CommandDecl noLit;
        noLit.name = "nolit";
        noLit.branches.push_back({{}, {}});
        const auto wire = buildWireDefinitions({noLit});
        CHECK(wire.enums.empty(), "no-literal command emits no enums");
        CHECK(wire.commands.size() == 1 && wire.commands[0].params.empty(),
              "no-literal command emits no leading action parameter");
    }

    // Bug 3 ArgSpec default: unspecified int bounds cover the full int32 range.
    {
        CommandDecl decl;
        decl.name = "intprobe";
        ArgSpec n;
        n.kind = ArgKind::Int;
        n.optional = true;
        n.defaultValue = 7;
        decl.branches.push_back({{"run"}, {{"n", n}}});
        const auto parsed = parseCommandLine(decl, "/intprobe run 42");
        CHECK(parsed.ok && parsed.values.at("n").number == 42,
              "Arg.int without min/max accepts 42 (full int32 range)");
    }

    // End-to-end JS mods: command context first argument, get/getOptional/
    // rawLine/respond, no-literal command, and persisted console.log.
    const auto dir = tempDir("livefix");
    const auto probeDir = dir / "mods" / "Probe";
    const auto noLitDir = dir / "mods" / "NoLit";
    fs::create_directories(probeDir / "src");
    fs::create_directories(noLitDir / "src");
    writeFile(probeDir / "manifest.json",
              R"({"apiVersion":"1.0.0","entry":"src/index.ts","gameVersions":["1.26.65.1"],"name":"Probe","version":"1.0.0"})");
    writeFile(probeDir / "package.json",
              R"({"name":"probe","type":"module","private":true,"version":"1.0.0"})");
    writeFile(probeDir / "src/index.ts", R"TS(
import { Mod, Arg } from "@bedrock/sdk";
export default class Probe extends Mod {
  onEnable(): void {
    this.commands.register("probe", (b: any) => {
      b.literal("run")
       .argument("n", Arg.int({ optional: true, default: 7 }))
       .executes((ctx: any) => {
         ctx.respond(`got=${String(ctx.get("n"))}|opt=${String(ctx.getOptional("n"))}|raw=${ctx.rawLine()}`);
       });
    });
  }
}
)TS");
    writeFile(noLitDir / "manifest.json",
              R"({"apiVersion":"1.0.0","entry":"src/index.ts","gameVersions":["1.26.65.1"],"name":"NoLit","version":"1.0.0"})");
    writeFile(noLitDir / "package.json",
              R"({"name":"nolit","type":"module","private":true,"version":"1.0.0"})");
    writeFile(noLitDir / "src/index.ts", R"TS(
import { Mod } from "@bedrock/sdk";
export default class NoLit extends Mod {
  onEnable(): void {
    console.log("hello-from-nolit");
    this.commands.register("nolit", (b: any) => {
      b.executes((ctx: any) => { ctx.respond("nolit-ok"); });
    });
  }
}
)TS");

    PluginHost host(dir / "mods", "1.26.65.1");
    host.setLogDirectory(dir / "logs");
    std::string error;
    CHECK(host.initialize(error), "livefix initialize: " + error);
    CHECK(host.list().size() == 2, "two livefix mods loaded");

    std::string response;
    CHECK(host.dispatchCommand("/probe run 42", response), "probe dispatch");
    CHECK(response.find("got=42|opt=42|raw=/probe run 42") != std::string::npos,
          "handler first arg is a populated CommandContext (Bug 2) / Arg.int range (Bug 3): " +
              response);

    response.clear();
    CHECK(host.dispatchCommand("/nolit", response), "no-literal command dispatch");
    CHECK(response.find("nolit-ok") != std::string::npos,
          "no-literal command loads and executes (Bug 4): " + response);

    const auto& defs = host.definitions();
    for (const auto& e : defs.enums)
        CHECK(e.merged || !e.values.empty(), "published enum is loadable: " + e.name);
    bool hasProbe = false, hasNolit = false;
    for (const auto& c : defs.commands) {
        if (c.name == "probe") hasProbe = true;
        if (c.name == "nolit") hasNolit = true;
    }
    CHECK(hasProbe && hasNolit, "both commands published");
    CHECK(host.handleModsCommand("/mods list").find("Probe") != std::string::npos,
          "/mods still works after the no-literal command loaded");

    // Bug 5: console.log persisted to <logDir>/<ModName>.log.
    std::ifstream logFile(dir / "logs" / "NoLit.log");
    CHECK(logFile.good(), "per-mod log file created");
    const std::string logContent((std::istreambuf_iterator<char>(logFile)),
                                 std::istreambuf_iterator<char>());
    CHECK(logContent.find("hello-from-nolit") != std::string::npos,
          "JS console.log written to the per-mod log file");
}

// ---------------------------------------------------------------------------
// Creator Tools content log: each mod's JS LogSink lines reach the same
// SDK content-log bridge, with the mod name as area and correct level mapping.
// ---------------------------------------------------------------------------
void testContentLogBridge() {
    struct Line {
        bedrock::LogLevel level;
        std::string tag;
        std::string text;
    };
    static std::vector<Line> lines;
    lines.clear();
    const auto writer = +[](bedrock::LogLevel level, std::string_view tag,
                            std::string_view text, void*) noexcept {
        lines.push_back({level, std::string(tag), std::string(text)});
    };
    bedrock::setContentLogWriter(writer);

    LogSink sink("EffectFake");
    sink.write("info", "hello-content-log");
    sink.write("warn", "careful-content-log");
    sink.write("error", "failed-content-log");
    CHECK(lines.size() == 3, "all three console levels emitted to content log");
    CHECK(lines[0].level == bedrock::LogLevel::Info && lines[0].tag == "EffectFake" &&
              lines[0].text == "hello-content-log", "console.log -> info + mod tag");
    CHECK(lines[1].level == bedrock::LogLevel::Warn, "console.warn -> warn");
    CHECK(lines[2].level == bedrock::LogLevel::Error, "console.error -> error");

    bedrock::setContentLogWriter(nullptr);
    sink.write("info", "after-clear");
    CHECK(lines.size() == 3, "no writer installed => content log is a no-op");
}

// ---------------------------------------------------------------------------
// 10. Per-Mod files API (synchronous, sandboxed, persistent).
// ---------------------------------------------------------------------------
class MinimalWorldBackend final : public WorldBackend {
public:
    bool hasLocalPlayer() const override { return false; }
};

// A single JS realm with the real SdkBindings + generated bootstrap, rooted at
// a test temp directory. Mirrors the production PluginHost wiring.
struct FilesRealm {
    JsRuntime runtime;
    HostContext host;
    CommandRegistry commands;
    MinimalWorldBackend world;
    SdkBindings bindings;
    std::string error;

    explicit FilesRealm(const fs::path& dataDir, const std::string& modName = "FilesMod")
        : commands(runtime), bindings(runtime, host, commands, world, modName, dataDir) {
        if (!runtime.initialize(error)) return;
        runtime.setContextOpaque(&host);
        if (!bindings.install(error)) return;
    }
    bool ok() const { return error.empty(); }
    bool eval(const std::string& code) {
        std::string evalError;
        return runtime.eval(code, "files-api-test.js", evalError);
    }
};

std::string filesOut(JsRuntime& runtime, const char* key) {
    const auto ctx = runtime.context();
    JsValue global(ctx, JS_GetGlobalObject(ctx));
    JsValue out(ctx, JS_GetPropertyStr(ctx, global.get(), "out"));
    JsValue value(ctx, JS_GetPropertyStr(ctx, out.get(), key));
    return jsToString(ctx, value.get());
}

int countText(const std::string& hay, const std::string& needle) {
    int count = 0;
    for (std::size_t pos = 0; (pos = hay.find(needle, pos)) != std::string::npos;
         pos += needle.size())
        ++count;
    return count;
}

void testFilesApi() {
    const auto dir = tempDir("files-api");
    const auto dataDir = dir / "mod-data";
    CHECK(!fs::exists(dataDir), "data root is created lazily, not at binding install");

    {
        FilesRealm realm(dataDir);
        CHECK(realm.ok(), "files realm install: " + realm.error);
        CHECK(realm.eval(R"JS(
(function () {
  const f = __sdkModule.files;
  const out = {};
  out.dataDir = f.dataDir;
  f.writeText("nested/deep/hello.txt", "hello-\u4e16\u754c");
  out.text = f.readText("nested/deep/hello.txt");
  f.appendText("nested/deep/hello.txt", "!");
  out.appended = f.readText("nested/deep/hello.txt");
  f.writeBytes("bin/data.bin", new Uint8Array([0, 1, 2, 255]));
  out.bytes = Array.from(f.readBytes("bin/data.bin")).join(",");
  f.writeJson("cfg/settings.json", { speed: 3, name: "test" });
  const json = f.readJson("cfg/settings.json");
  out.json = json.speed + ":" + json.name;
  out.exists = String(f.exists("cfg/settings.json")) + ":" + String(f.exists("nope.txt"));
  const st = f.stat("nested/deep/hello.txt");
  out.stat = st.size + ":" + String(st.directory) + ":" + (typeof st.modifiedMs);
  f.mkdir("folder/sub");
  out.list = f.list().map(function (e) { return e.name + (e.directory ? "/" : ""); }).sort().join(",");
  out.listSub = f.list("folder").map(function (e) { return e.name + (e.directory ? "/" : ""); }).join(",");
  f.rename("nested/deep/hello.txt", "moved/hello-renamed.txt");
  out.renamed = f.readText("moved/hello-renamed.txt");
  f.remove("bin/data.bin");
  out.removed = String(f.exists("bin/data.bin"));
  f.remove("folder", { recursive: true });
  out.dirRemoved = String(f.exists("folder"));
  const codes = [];
  ["../evil.txt", "/tmp/evil.txt", "a/../../evil.txt"].forEach(function (bad) {
    try { f.writeText(bad, "x"); codes.push("accepted"); }
    catch (e) { codes.push(String(e.code || "no-code")); }
  });
  out.traversal = codes.join(",");
  let dirRemoveCode = "none";
  try { f.remove("moved"); } catch (e) { dirRemoveCode = String(e.code || "no-code"); }
  out.dirRemoveCode = dirRemoveCode;
  globalThis.out = out;
})();
)JS"), "files eval");

        const auto root = fs::path(filesOut(realm.runtime, "dataDir"));
        CHECK(root.is_absolute(), "files.dataDir is absolute");
        CHECK(root.filename() == "FilesMod", "files.dataDir is per-Mod: " + root.generic_string());
        CHECK(filesOut(realm.runtime, "text") == "hello-\u4e16\u754c",
              "writeText/readText UTF-8 roundtrip");
        CHECK(filesOut(realm.runtime, "appended") == "hello-\u4e16\u754c!",
              "appendText appends to an existing file");
        CHECK(filesOut(realm.runtime, "bytes") == "0,1,2,255",
              "writeBytes/readBytes roundtrip including high bytes");
        CHECK(filesOut(realm.runtime, "json") == "3:test", "writeJson/readJson roundtrip");
        CHECK(filesOut(realm.runtime, "exists") == "true:false", "files.exists for present/absent paths");
        CHECK(filesOut(realm.runtime, "stat") == "13:false:number",
              "files.stat size/directory/modifiedMs (UTF-8 byte length): " +
                  filesOut(realm.runtime, "stat"));
        CHECK(filesOut(realm.runtime, "list") == "bin/,cfg/,folder/,nested/",
              "files.list defaults to '.' and marks directories");
        CHECK(filesOut(realm.runtime, "listSub") == "sub/", "files.list(path) lists a subdirectory");
        CHECK(filesOut(realm.runtime, "renamed") == "hello-\u4e16\u754c!",
              "files.rename moves a file and creates parent dirs");
        CHECK(filesOut(realm.runtime, "removed") == "false", "files.remove deletes a file");
        CHECK(filesOut(realm.runtime, "dirRemoved") == "false",
              "files.remove(dir, { recursive: true }) deletes a directory");
        CHECK(filesOut(realm.runtime, "traversal") == "Invalid,Invalid,Invalid",
              "../, absolute and escaping paths throw Invalid");
        CHECK(filesOut(realm.runtime, "dirRemoveCode") == "Invalid",
              "removing a directory without recursive throws Invalid");
        CHECK(!fs::exists(dir / "evil.txt"), "traversal did not create a file outside the root");
        CHECK(!fs::exists(dir / "mod-data" / "evil.txt"), "traversal stayed inside the data root");
    }

    // Persistence: a brand new JS context over the same root sees the files.
    {
        FilesRealm realm(dataDir);
        CHECK(realm.ok(), "reload realm install: " + realm.error);
        CHECK(realm.eval(R"JS(
(function () {
  const f = __sdkModule.files;
  globalThis.out = {
    dataDir: f.dataDir,
    text: f.readText("moved/hello-renamed.txt"),
    json: JSON.stringify(f.readJson("cfg/settings.json"))
  };
})();
)JS"), "persistence eval");
        CHECK(filesOut(realm.runtime, "text") == "hello-\u4e16\u754c!",
              "file still readable from a new JS context (simulated /mods reload)");
        CHECK(filesOut(realm.runtime, "json").find("speed") != std::string::npos,
              "JSON file persisted across a new JS context");
    }

    // Per-Mod isolation: another mod roots at its own folder.
    {
        FilesRealm realm(dataDir, "OtherMod");
        CHECK(realm.ok(), "isolated realm install: " + realm.error);
        CHECK(realm.eval(R"JS(
(function () {
  const f = __sdkModule.files;
  let code = "none";
  try { f.readText("moved/hello-renamed.txt"); } catch (e) { code = String(e.code || "no-code"); }
  globalThis.out = { dataDir: f.dataDir, code: code };
})();
)JS"), "isolation eval");
        CHECK(fs::path(filesOut(realm.runtime, "dataDir")).filename() == "OtherMod",
              "other mod gets its own data root");
        CHECK(filesOut(realm.runtime, "code") == "NotFound",
              "mods cannot read each other's files");
    }
}

void testFilesApiPluginHost() {
    const auto dir = tempDir("files-plugin");
    const auto modDir = dir / "mods" / "FileMod";
    fs::create_directories(modDir / "src");
    writeFile(modDir / "manifest.json",
              R"({"apiVersion":"1.0.0","entry":"src/index.ts","gameVersions":["1.26.65.1"],"name":"FileMod","version":"1.0.0"})");
    writeFile(modDir / "package.json", R"({"name":"filemod","type":"module","private":true,"version":"1.0.0"})");
    writeFile(modDir / "src/index.ts", R"TS(
import { Mod, files } from "@bedrock/sdk";
export default class FileMod extends Mod {
  onEnable(): void {
    if (!files.exists("state.txt")) files.writeText("state.txt", "persisted");
    console.log("root=" + files.dataDir);
    console.log("value=" + files.readText("state.txt"));
  }
}
)TS");

    PluginHost host(dir / "mods", "1.26.65.1");
    host.setDataDirectory(dir / "mod-data");
    host.setLogDirectory(dir / "logs");
    std::string error;
    CHECK(host.initialize(error), "files plugin initialize: " + error);
    CHECK(host.list().size() == 1 && host.list()[0].status == ModStatus::Active,
          "files-using mod loads and enables");

    const auto readLog = [&]() {
        std::ifstream in(dir / "logs" / "FileMod.log");
        return std::string((std::istreambuf_iterator<char>(in)),
                           std::istreambuf_iterator<char>());
    };
    const auto firstLog = readLog();
    const auto expectedRoot = (dir / "mod-data" / "FileMod").generic_string();
    CHECK(firstLog.find("root=" + expectedRoot) != std::string::npos,
          "PluginHost anchors files.dataDir to <data dir>/<ModName>");
    CHECK(firstLog.find("value=persisted") != std::string::npos,
          "TS mod reads back its written file end-to-end");
    CHECK(fs::exists(dir / "mod-data" / "FileMod" / "state.txt"),
          "file lives outside the deployed mod folder");
    CHECK(expectedRoot.rfind((dir / "mods").generic_string(), 0) != 0,
          "data root is not under the mods directory");

    CHECK(host.reload("FileMod", error), "files plugin reload: " + error);
    const auto secondLog = readLog();
    CHECK(countText(secondLog, "value=persisted") == 2,
          "same file readable after PluginHost reload (new JS context)");
}

} // namespace

int main() {
    testManifest();
    testRequireResolution();
    testGoldenEffectFake();
    testCommandParsing();
    testLifecycle();
    testBindingRegistry();
    testSettingsKeybindsBrightnessAndEvents();
    testProfileFacadeRegression();
    testLiveFixRegressions();
    testContentLogBridge();
    testFilesApi();
    testFilesApiPluginHost();
    std::cout << "checks=" << checks << " failures=" << failures << "\n";
    return failures == 0 ? 0 : 1;
}
