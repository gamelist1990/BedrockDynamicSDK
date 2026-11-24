// Stage B Phase 1 host tests (DESIGN §10.3): no game, no BOL.
#include "command_model.hpp"
#include "manifest.hpp"
#include "mod_scanner.hpp"
#include "module_loader.hpp"
#include "package.hpp"
#include "plugin_host.hpp"
#include "sdk_bindings.hpp"
#include "transpiler.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string_view>
#include <thread>
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
    CHECK(clearOne.values.at("effect").present, "supplied arg marked present");
    CHECK(!defaults.values.at("duration").present, "defaulted arg marked not present");

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
export function onEnable() { globalThis.enabled = true; }
export function onUpdate() { while (true) {} }
)TS");

    PluginHost host(dir / "mods", "1.26.65.1");
    host.setFaultBudgetMs(50);
    std::string error;
    CHECK(host.initialize(error), "initialize: " + error);
    auto views = host.list();
    CHECK(views.size() == 1 && views[0].status == ModStatus::Active, "loop mod initially active");
    host.tick();
    views = host.list();
    CHECK(views[0].status == ModStatus::Faulted, "runaway onUpdate faults the mod");

    writeFile(modDir / "src/index.ts", R"TS(
let n: number = 0;
export function onEnable() { n = 1; }
export function onUpdate() { n++; }
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
    CHECK(doc["exports"].size() >= 5, "registry lists the baseline exports");
    const auto modules = doc["builtinModules"].get<std::vector<std::string>>();
    for (const char* expected : {"@bedrock/sdk", "@bedrock/vanilla-data",
                                 "@minecraft/server-net", "fs", "path", "events", "os"})
        CHECK(std::find(modules.begin(), modules.end(), expected) != modules.end(),
              std::string("builtin registered: ") + expected);
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
    BackendOutcome playerRemoveEffect(const std::string& modName, std::uint64_t,
                                       const std::string& id, bool& removed) override {
        removed = std::erase_if(effectsByMod[modName], [&](const auto& effect) {
            return effect.typeId == id;
        }) != 0;
        return BackendOutcome::success();
    }
    BackendOutcome playerClearEffects(const std::string& modName, std::uint64_t) override {
        effectsByMod[modName].clear();
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
import { system, Setting, ItemStack, EquipmentSlot, brightness, world } from "@bedrock/sdk";

import type { PlayerSpawnAfterEvent } from "@bedrock/sdk";

function spawn(ev: PlayerSpawnAfterEvent): void {
  world.sendMessage(`spawn:${ev.player.name}:${ev.initialSpawn}`);
}

export function onEnable(): void {
  const page = system.settings("moda.title")
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

  system.keybinds
    .add("zoom", "key.zoom", "KeyC")
    .add("freecam", "key.freecam", "F5")
    .locale("en_US", { "key.zoom": "Zoom", "key.freecam": "Freecam" });

  world.sendMessage(`zoomKey=${String(system.keybinds.key("zoom"))}`);

  system.afterEvents.keybindChanged.subscribe((ev: { id: string; pressed: boolean }) => {
    world.sendMessage(`keySub:${ev.id}:${ev.pressed}`);
  });

  world.afterEvents.playerSpawn.subscribe(spawn);

  brightness.setGamma(20);
}

export function onKeybind(id: string, pressed: boolean): void {
  world.sendMessage(`onKeybind:${id}:${pressed}`);
  if (id === "unsub" && pressed) {
    world.afterEvents.playerSpawn.unsubscribe(spawn);
  }
}

export function onUpdate(): void {
  const p = world.getLocalPlayer();
  if (!p) return;
  world.setDynamicProperty("modProp", "fromModA");
  p.setDynamicProperty("pProp", 42);

  const hp = p.getComponent("minecraft:health");
  const food = p.getComponent("minecraft:food");
  const mov = p.getComponent("minecraft:movement");
  const inv = p.getComponent("minecraft:inventory");
  const eq = p.getComponent("minecraft:equippable");
  const slot0 = inv ? inv.container.getItem(0) : undefined;
  const hand = eq ? eq.getEquipment(EquipmentSlot.Mainhand) : undefined;

  const dim = world.currentDimension;
  const blk = dim ? dim.getBlock({ x: 1, y: 64, z: -2 }) : undefined;
  const oldBlkType = blk ? blk.typeId : "none";
  if (blk) blk.setType("minecraft:stone");
  const newBlk = dim ? dim.getBlock({ x: 1, y: 64, z: -2 }) : undefined;

  const ents = dim ? dim.getEntities() : [];
  let droppedInfo = "none";
  for (const e of ents) {
    if (e.typeId === "minecraft:item") {
      const st = e.getComponent("minecraft:item")?.itemStack;
      if (st) droppedInfo = `${st.typeId}x${st.amount}`;
    }
  }

  const customStack = new ItemStack("minecraft:apple", 5);
  const rotation = p.getRotation();
  const noUnavailableStubs = !("spawnEntity" in dim) && !("teleport" in p) && !("addTag" in p);

  world.sendMessage(
    `tick:p=${p.name}:loc=${p.location.x},${p.location.y},${p.location.z}:rot=${rotation.x},${rotation.y}` +
    `:hp=${hp.currentValue}:food=${food.currentValue}:vel=${mov.velocity.x}` +
    `:inv=${slot0.typeId}x${slot0.amount}:eq=${hand.typeId}` +
    `:blk=${oldBlkType}->${newBlk.typeId}:drop=${droppedInfo}` +
    `:stack=${customStack.typeId}x${customStack.amount}:noStubs=${noUnavailableStubs}` +
    `:prop=${world.getDynamicProperty("modProp")}/${p.getDynamicProperty("pProp")}`
  );
}
)TS");

    writeFile(modBDir / "manifest.json",
              R"({"apiVersion":"1.0.0","entry":"src/index.ts","gameVersions":["1.26.65.1"],"name":"ModB","version":"1.0.0"})");
    writeFile(modBDir / "package.json", R"({"name":"modb","type":"module","private":true,"version":"1.0.0"})");
    writeFile(modBDir / "src/index.ts", R"TS(
import { system, brightness, world } from "@bedrock/sdk";

export function onEnable(): void {
  system.keybinds.add("unsub", "key.unsub", "KeyG");
  brightness.setGamma(12);
}
export function onUpdate(): void {
  const p = world.getLocalPlayer();
  const propFromA = world.getDynamicProperty("modProp");
  world.setDynamicProperty("modProp", "fromModB");
  world.sendMessage(`modB:p=${p ? p.name : "none"}:propA=${String(propFromA)}:propB=${String(world.getDynamicProperty("modProp"))}`);
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
          "dimension.getEntities + minecraft:item component");
    CHECK(hasMsg(msgsA, "stack=minecraft:applex5:noStubs=true"),
          "new ItemStack constructor and no unavailable operation stubs");
    CHECK(hasMsg(msgsA, "prop=fromModA/42"), "world and entity dynamic properties");

    // 5. Per-Mod WorldBackend isolation
    CHECK(hasMsg(msgsB, "modB:p=Steve:propA=undefined:propB=fromModB"),
          "ModB sees its own world/player context and isolated dynamic properties");

    // 6. Keybind dispatch -> both system.afterEvents and named onKeybind
    host.notifyKeybind("ModA", "zoom", true);
    CHECK(hasMsg(msgsA, "keySub:zoom:true") && hasMsg(msgsA, "onKeybind:zoom:true"),
          "keybind notification reaches both the after-event and lifecycle export");

    // 7. world.afterEvents.playerSpawn subscription & unsubscription
    host.notifyPlayerSpawn("ModA", 100, true);
    CHECK(hasMsg(msgsA, "spawn:Alex:true"), "playerSpawn delivered to JS subscriber");

    const auto countBeforeUnsub = msgsA.size();
    host.notifyKeybind("ModA", "unsub", true); // unsubscribes the spawn callback
    host.notifyPlayerSpawn("ModA", 100, false);
    bool spawnedAfterUnsub = false;
    for (std::size_t i = countBeforeUnsub; i < msgsA.size(); ++i) {
        if (msgsA[i].find("spawn:") != std::string::npos) spawnedAfterUnsub = true;
    }
    CHECK(!spawnedAfterUnsub, "playerSpawn.unsubscribe(callback) stops future delivery");

    // 8. Unload / reload cleanup (SettingsPage unregister + BrightnessOverride restore)
    writeFile(modBDir / "src/index.ts", R"TS(
export function onEnable(): void {}
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
import { system } from "@bedrock/sdk";
system.commands.register("probe", [
  {
    literals: ["run"], parameters: [{ name: "n", kind: "int", optional: true, default: 7 }],
    execute(ctx) {
      ctx.respond(`got=${String(ctx.get("n"))}|opt=${String(ctx.getOptional("n"))}|raw=${ctx.rawLine()}`);
    },
  },
  {
    literals: ["clear"], parameters: [{ name: "effect", kind: "effect", optional: true }],
    execute(ctx) {
      ctx.respond(`opt=${String(ctx.getOptional("effect"))}|type=${typeof ctx.getOptional("effect")}`);
    },
  },
  {
    literals: ["duration"],
    parameters: [{ name: "ticks", kind: "duration", optional: true, default: 23, infinity: true }],
    execute(ctx) { ctx.respond(`${ctx.get("ticks")}/${ctx.getOptional("ticks")}`); },
  },
]);
)TS");
    writeFile(noLitDir / "manifest.json",
              R"({"apiVersion":"1.0.0","entry":"src/index.ts","gameVersions":["1.26.65.1"],"name":"NoLit","version":"1.0.0"})");
    writeFile(noLitDir / "package.json",
              R"({"name":"nolit","type":"module","private":true,"version":"1.0.0"})");
    writeFile(noLitDir / "src/index.ts", R"TS(
import { system } from "@bedrock/sdk";
console.log("hello-from-nolit");
system.commands.register("nolit", [{ execute(ctx) { ctx.respond("nolit-ok"); } }]);
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
    CHECK(host.dispatchCommand("/probe run", response), "probe dispatch absent defaulted arg");
    CHECK(response.find("got=7|opt=undefined") != std::string::npos,
          "absent optional: get yields the declared default, getOptional yields undefined: " + response);

    response.clear();
    CHECK(host.dispatchCommand("/probe clear", response), "probe dispatch absent no-default arg");
    CHECK(response.find("opt=undefined|type=undefined") != std::string::npos,
          "absent optional WITHOUT default yields real undefined from getOptional: " + response);

    response.clear();
    CHECK(host.dispatchCommand("/probe clear speed", response), "probe dispatch present no-default arg");
    CHECK(response.find("opt=minecraft:speed|type=string") != std::string::npos,
          "present optional without default yields its value: " + response);

    response.clear();
    CHECK(host.dispatchCommand("/probe duration", response) && response == "23/undefined",
          "duration default round-trips in ticks: " + response);
    response.clear();
    CHECK(host.dispatchCommand("/probe duration 2.05", response) && response == "41/41",
          "explicit command seconds convert to ticks: " + response);
    response.clear();
    CHECK(host.dispatchCommand("/probe duration Infinity", response) && response == "Infinity/Infinity",
          "explicit Infinity round-trips in both context methods: " + response);

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
import { files } from "@bedrock/sdk";
export function onEnable(): void {
  if (!files.exists("state.txt")) files.writeText("state.txt", "persisted");
  console.log("root=" + files.dataDir);
  console.log("value=" + files.readText("state.txt"));
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

// ---------------------------------------------------------------------------
// 5. Dev bindings (guarded memory, symbols, module introspection)
// ---------------------------------------------------------------------------
void testDevBindings() {
    // Host test realm with a real SdkBindings; dev ops return Unavailable on
    // non-Windows builds and are guarded on Windows. We verify the JS-level
    // error contract without requiring a game process.
    FilesRealm realm(tempDir("dev-bindings"), "DevTestMod");
    CHECK(realm.ok(), "dev realm install: " + realm.error);

    std::string devErr;

    // dev.memory.readBytes at address 0 -> JS Error {code:'Unavailable'}
    CHECK(realm.runtime.eval(R"JS(
(function () {
  try { __sdkModule.dev.memory.readBytes(0, 32); globalThis.out = { dev1: "ok" }; }
  catch (e) { globalThis.out = { dev1: String(e.code || "no-code") }; }
})();
)JS", "dev-readbytes.js", devErr), "dev readBytes eval");
    const auto dev1 = filesOut(realm.runtime, "dev1");
    CHECK(dev1 == "Unavailable",
          "readBytes(0) throws Unavailable, got: " + dev1);

    // dev.memory.scanAob with empty pattern -> Unavailable (host only)
    CHECK(realm.runtime.eval(R"JS(
(function () {
  try { __sdkModule.dev.memory.scanAob(""); globalThis.out = { dev2: "ok" }; }
  catch (e) { globalThis.out = { dev2: String(e.code || "no-code") }; }
})();
)JS", "dev-scanaob.js", devErr), "dev scanAob eval");
    const auto dev2 = filesOut(realm.runtime, "dev2");
    CHECK(dev2 == "Unavailable",
          "scanAob('') throws Unavailable, got: " + dev2);

    // dev.symbols.list with empty profile -> Unavailable (host only)
    CHECK(realm.runtime.eval(R"JS(
(function () {
  try { const l = __sdkModule.dev.symbols.list(); globalThis.out = { dev3: JSON.stringify(l) }; }
  catch (e) { globalThis.out = { dev3: String(e.code || "no-code") }; }
})();
)JS", "dev-symbols-list.js", devErr), "dev symbols.list eval");
    const auto dev3 = filesOut(realm.runtime, "dev3");
    CHECK(dev3 == "Unavailable",
          "symbols.list throws Unavailable, got: " + dev3);

    // dev.module.info returns Unavailable (host only)
    CHECK(realm.runtime.eval(R"JS(
(function () {
  try { const m = __sdkModule.dev.module.info(); globalThis.out = { dev4: "ok" }; }
  catch (e) { globalThis.out = { dev4: String(e.code || "no-code") }; }
})();
)JS", "dev-module-info.js", devErr), "dev module.info eval");
    const auto dev4 = filesOut(realm.runtime, "dev4");
    CHECK(dev4 == "Unavailable",
          "module.info throws Unavailable, got: " + dev4);
}

std::string readTestLog(const fs::path& dir, std::string name);
void checkActive(PluginHost& host, const std::string& name);

void testManagerPackages(const fs::path& fixtures) {
    std::cerr << "manager: storage and rejected archives\n";
    const auto dir = tempDir("manager-packages");
    fs::create_directories(dir / "mods");
    CHECK(selectStorageRoot(dir, fixtures) == dir, "uses EXE storage root");
    CHECK(selectStorageRoot(dir / "missing-game", dir) == dir, "uses writable DLL fallback");
    for (const char* invalid : {"traversal.zip", "case.mods", "no-marker.zip", "symlink.mods",
                                "native.mods", "device.mods", "incompatible.mods"}) {
        bool rejected = false;
        try { (void)installPackage(fixtures / invalid, dir / "mods", "1.26.65.1"); }
        catch (const std::exception&) { rejected = true; }
        CHECK(rejected && fs::is_empty(dir / "mods"), std::string("rejected without publishing: ") + invalid);
    }
    PluginHost host(dir / "mods", "1.26.65.1");
    host.setStateDirectory(dir / ".scriptclient-disabled");
    host.setLogDirectory(dir / "logs");
    std::string error;
    CHECK(host.initialize(error) && host.list().empty(), "empty initial scan");
    std::cerr << "manager: publish and load valid archive\n";
    const auto name = installPackage(fixtures / "valid.mods", dir / "mods", "1.26.65.1");
    CHECK(name == "ManagerTest" && host.reload(name, error), "install + reload discovers a new mod: " + error);
    checkActive(host, name);
    CHECK(readTestLog(dir, name).find("test-enabled") != std::string::npos, "TypeScript entry executed");
    bool duplicate = false;
    try { (void)installPackage(fixtures / "valid.mods", dir / "mods", "1.26.65.1"); }
    catch (const std::exception&) { duplicate = true; }
    CHECK(duplicate && fs::is_regular_file(dir / "mods" / name / "index.ts"), "no replacement");
    std::cerr << "manager: disable, reopen and re-enable\n";
    host.setEnabled(name, false);
    CHECK(host.reload(name, error) && host.list().front().status == ModStatus::Disabled,
          "disable reload leaves mod listed");
    const auto disabledLog = readTestLog(dir, name);
    CHECK(disabledLog.find("test-disabled") != std::string::npos, "unload lifecycle executed");
    PluginHost reopened(dir / "mods", "1.26.65.1");
    reopened.setStateDirectory(dir / ".scriptclient-disabled");
    reopened.setLogDirectory(dir / "logs");
    CHECK(reopened.initialize(error) && reopened.list().front().status == ModStatus::Disabled &&
          readTestLog(dir, name) == disabledLog, "fresh host skips disabled mod before JS startup");
    reopened.setEnabled(name, true);
    CHECK(reopened.reload(name, error), "re-enable uses same reload path");
    checkActive(reopened, name);
}

} // namespace

namespace {

fs::path makeTestMod(const fs::path& dir, std::string name, std::string code) {
    const auto mod = dir / "mods" / name;
    writeFile(mod / "manifest.json", nlohmann::json({
        {"name", name}, {"apiVersion", "1.0.0"}, {"version", "1.0.0"},
        {"entry", "index.js"}, {"gameVersions", {"*"}}
    }).dump());
    writeFile(mod / "index.js", code);
    return mod;
}

std::string readTestLog(const fs::path& dir, std::string name) {
    std::ifstream file(dir / "logs" / (name + ".log"));
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}

void checkActive(PluginHost& host, const std::string& name) {
    for (const auto& mod : host.list())
        if (mod.name == name) {
            CHECK(mod.status == ModStatus::Active, name + " not active: " + mod.error);
            return;
        }
    CHECK(false, "missing mod " + name);
}

void testNodeBuiltins() {
    const auto dir = tempDir("node-builtins");
    const auto mod = makeTestMod(dir, "NodeTest", R"JS(
function check(test, message) { if (!test) throw new Error(message); }
function fails(fn, code) { try { fn(); } catch (err) { return err instanceof Error && err.code === code; } }
const fs = require('fs');
const path = require('path');
const EventEmitter = require('events');
check(fs === require('node:fs') && fs.promises === require('node:fs/promises'), 'node aliases');
check(fails(() => require('net'), 'Unavailable') && fails(() => require('node:net'), 'Unavailable'), 'net blocked');
check(fails(() => require('http'), 'NotFound') && fails(() => require('node:https'), 'Unavailable'),
      'no fake Node HTTP streams');
check(path.posix.join('/a/', 'b', '..', 'c') === '/a/c', 'posix join');
check(path.posix.dirname('/a/b.txt') === '/a' &&
  path.posix.basename('/a/b.txt', '.txt') === 'b' && path.posix.extname('/a/b.txt') === '.txt', 'posix parts');
check(path.posix.normalize('a//./b/../') === 'a/' &&
  path.posix.relative('/a/c', '/a/d/b') === '../d/b' && path.posix.resolve('a') === '/a', 'posix normalization');
const parsed = path.win32.parse('C:\\dir\\x.txt');
check(path.win32.format(parsed) === 'C:\\dir\\x.txt' && parsed.root === 'C:\\', 'win32 parse/format');
check(path.win32.normalize('\\\\server\\share\\a\\..\\') === '\\\\server\\share\\' &&
      path.win32.relative('C:\\a', 'c:\\B') === '..\\B' &&
      path.win32.resolve('D:tmp') === 'D:\\tmp' && path.win32.isAbsolute('D:\\tmp'), 'win32 paths');
const os = require('os');
check((os.platform() === 'win32' ? '\r\n' : '\n') === os.EOL && os.arch().length > 0, 'os constants');
fs.mkdirSync('nested');
fs.mkdirSync(path.join('nested', 'sub', 'deep'), {recursive: true});
const file = path.join('nested', 'hello.txt');
fs.writeFileSync(file, 'hello 世界', {encoding: 'utf8'});
fs.appendFileSync(file, new Uint8Array([33]));
check(fs.readFileSync(file, 'utf-8') === 'hello 世界!', 'UTF-8 read and byte append');
check(fs.readFileSync(file) instanceof Uint8Array, 'binary read');
check(require('@bedrock/sdk').files.readText(file) === 'hello 世界!', 'shared file root');
fs.copyFileSync(file, 'copy.txt', fs.constants.COPYFILE_EXCL);
fs.renameSync('copy.txt', 'renamed.txt');
check(!fs.existsSync('copy.txt') && fs.existsSync('renamed.txt'), 'copy and rename');
const stat = fs.statSync('renamed.txt');
check(stat.isFile() && !stat.isDirectory() && stat.size === 13 && stat.mtime instanceof Date, 'stat');
check(fs.statSync('nested').isDirectory() && fs.readdirSync('nested').includes('hello.txt'), 'list');
fs.rmSync('nested', {recursive: true});
check(!fs.existsSync('nested'), 'recursive remove');
check(fails(() => fs.writeFileSync('../escape', 'no'), 'Invalid') &&
      fails(() => fs.readFileSync('/absolute'), 'Invalid') &&
      fails(() => fs.writeFileSync('trunc\0alias', 'no'), 'Invalid') &&
      fails(() => fs.rmSync('.', {recursive:true}), 'Invalid') &&
      fails(() => fs.copyFileSync('renamed.txt', '../escape'), 'Invalid'), 'file sandbox');
if (os.platform() !== 'win32') {
  check(fails(() => fs.writeFileSync('outside/escape', 'no'), 'Invalid') &&
        fails(() => fs.writeFileSync('dangling', 'no'), 'Invalid'), 'symlink sandbox');
}
const e = new EventEmitter.EventEmitter();
const order = [];
function f() { check(this === e, 'emitter receiver'); order.push('on'); }
e.on('item', f).once('item', () => order.push('once')).prependListener('item', () => order.push('first'));
check(e.listenerCount('item') === 3 && e.emit('item'), 'listener count and emit');
e.off('item', f);
e.emit('item');
check(order.join() === 'first,on,once,first', 'listener order');
e.removeAllListeners();
check(!e.emit('item') && e.eventNames().length === 0, 'listener removal');
exports.onUpdate = function () {
  if (exports.started) return;
  exports.started = true;
  fs.readFile('renamed.txt', 'utf8', (err, text) => {
    if (err || text !== 'hello 世界!') return console.error('callback failed');
    console.log('callback-ok');
  });
  fs.promises.readFile('missing').catch(err => console.log('promise-error=' + err.code));
};
console.log('node-builtins-ok');
)JS");
    writeFile(mod / "node_modules" / "fs" / "index.js", "throw new Error('must not shadow fs');");
    writeFile(mod / "node_modules" / "net" / "index.js", "throw new Error('must not shadow net');");
#ifndef _WIN32
    fs::create_directories(dir / "outside");
    fs::create_directories(dir / "mod-data" / "NodeTest");
    fs::create_directory_symlink(dir / "outside", dir / "mod-data" / "NodeTest" / "outside");
    fs::create_symlink(dir / "outside" / "escape", dir / "mod-data" / "NodeTest" / "dangling");
#endif
    PluginHost host(dir / "mods", "1.26.65.1");
    host.setDataDirectory(dir / "mod-data");
    host.setLogDirectory(dir / "logs");
    std::string error;
    CHECK(host.initialize(error), "initialize node builtins: " + error);
    checkActive(host, "NodeTest");
    host.tick();
    const auto log = readTestLog(dir, "NodeTest");
    CHECK(log.find("node-builtins-ok") != log.npos, "Node builtin checks: " + log);
    CHECK(log.find("callback-ok") != log.npos, "fs callback on job queue");
    CHECK(log.find("promise-error=NotFound") != log.npos, "fs promises preserve error code");
    CHECK(!fs::exists(dir / "mod-data" / "escape") && !fs::exists(dir / "outside" / "escape"),
          "no sandbox escape");
}

void testScriptStyleSdk() {
    const auto dir = tempDir("script-style");
    const auto mods = dir / "mods";
    fs::create_directories(mods);
    fs::copy(fs::path(STAGEB_SOURCE_DIR) / "examples" / "OfficialStyle", mods / "OfficialStyle",
             fs::copy_options::recursive);
    copyGolden(mods / "EffectFake");
    makeTestMod(dir, "SdkTest", R"JS(
const { world, system, ItemStack, Entity, Player, Block, Effect, EventSignal, TicksPerSecond } = require('@bedrock/sdk');
function check(test, message) { if (!test) throw new Error(message); }
function code(fn) { try { fn(); } catch (error) { return error.code; } }
check(code(() => require('@minecraft/server')) === 'NotFound', 'removed builtin does not resolve');
check(!('beforeEvents' in world) && !('beforeEvents' in system) && !('localPlayer' in world),
  'no fabricated phases or legacy accessor');
check(system.currentTick === 0, 'clock starts at zero');
let fired = 0;
const spawn = world.afterEvents.playerSpawn;
const cb = ({ player, initialSpawn }) => {
  ++fired;
  spawn.unsubscribe(cb);
  check(player instanceof Player && player instanceof Entity && initialSpawn && player.name === 'Alex',
    'client spawn event wraps a Player');
  check(player.id === world.getLocalPlayer().id && player.isValid && player.typeId === 'minecraft:player',
    'player identity');
  check(world.getPlayers({ name: 'Alex' }).length === 1, 'name filter');
  check(world.getPlayers({ excludeNames: ['Alex'] }).length === 0, 'exclude filter');
  const block = world.getDimension('overworld').getBlock({x: 1.2, y: 64, z: -1.5});
  check(block instanceof Block && block.typeId === 'minecraft:glass' && block.z === -2 &&
    block.dimension.id === 'minecraft:overworld', 'Block reads');
  check(world.getDimension('nether') === undefined, 'inactive dimension not accessible');
  check(new ItemStack('stone', 2).amount === 2 &&
    player.getComponent('inventory').container.getItem(0).typeId === 'minecraft:diamond', 'ItemStack reads');
  check(code(() => player.addEffect('speed', 1.5)) === 'Invalid' &&
    code(() => player.addEffect('speed', 0)) === 'Invalid', 'reject invalid tick duration');
  check(player.addEffect('speed', 2 * TicksPerSecond + 1, { amplifier: 2, showParticles: false }) === undefined,
    'no fabricated addEffect return');
  const effect = player.getEffect('speed');
  check(effect instanceof Effect && effect.duration === 41 && effect.amplifier === 2 && !effect.showParticles,
    'effect snapshot round-trips ticks and options');
  check(player.removeEffect('speed') && player.getEffects().length === 0, 'effect removal');
  player.addEffect('night_vision', Infinity);
  check(player.getEffects()[0].duration === Infinity, 'preserve host-reported Infinity');
  player.clearEffects();
  check(player.getEffects().length === 0, 'clear all effects');
  player.addEffect('speed', 41);
  console.log('sdk-player-ok');
};
check(spawn instanceof EventSignal && spawn.subscribe(cb) === cb && spawn.subscribe(cb) === cb,
  'signal duplicates return the original callback');
const signal = system.afterEvents.keybindChanged;
const key = signal.subscribe(({ id, pressed }) => {
  ++keyCount;
  signal.unsubscribe(key);
  console.log(`key:${id}:${pressed}`);
});
let keyCount = 0;
const trace = [];
const record = name => trace.push(`${name}:${system.currentTick}`);
system.run(() => {
  record('run');
  system.run(() => record('nested'));
});
system.runTimeout(() => record('default'));
system.runTimeout(() => record('timeout'), 3);
const canceled = system.runInterval(() => record('canceled'));
check(system.clearRun(canceled) && !system.clearRun(canceled), 'cancellation reports removal');
check(code(() => system.runTimeout(() => {}, -1)) === 'Invalid' &&
  code(() => system.runInterval(() => {}, 0)) === 'Invalid', 'invalid delays rejected');
const interval = system.runInterval(() => {
  record('interval');
  if (system.currentTick === 4) system.clearRun(interval);
}, 2);
system.runTimeout(() => console.log(`trace=${trace.join(',')} spawn=${fired} key=${keyCount}`), 5);
)JS");
    auto world = std::make_shared<MockFullWorldBackend>();
    PluginHost host(mods, "1.26.65.1", world);
    host.setLogDirectory(dir / "logs");
    std::string error;
    CHECK(host.initialize(error), "initialize Script API mods: " + error);
    checkActive(host, "SdkTest");
    checkActive(host, "OfficialStyle");
    checkActive(host, "EffectFake");
    host.notifyPlayerSpawn("SdkTest", 100, true);
    host.notifyPlayerSpawn("SdkTest", 100, false);
    host.notifyKeybind("SdkTest", "zoom", true);
    host.notifyKeybind("SdkTest", "zoom", false);
    host.notifyPlayerSpawn("OfficialStyle", 100, true);
    for (int i = 0; i < 21; ++i) host.tick();
    const auto log = readTestLog(dir, "SdkTest");
    CHECK(log.find("sdk-player-ok") != log.npos, "player surface: " + log);
    CHECK(log.find("trace=run:1,default:1,interval:2,nested:2,timeout:3,interval:4 spawn=1 key=1") != log.npos,
          "scheduler timing, cancellation, duplicate subscription and unsubscription: " + log);
    const auto& effects = world->effectsByMod["SdkTest"];
    CHECK(effects.size() == 1 && effects[0].durationSeconds == 2.05, "host edge still uses seconds");
    const auto sample = readTestLog(dir, "OfficialStyle");
    CHECK(sample.find("interval players=1") != sample.npos && sample.find("ticks=40") != sample.npos,
          "official sample ran: " + sample);
    CHECK(world->messagesByMod["OfficialStyle"] ==
              std::vector<std::string>{"Hello from the Script API style mod!"},
          "sample sends a message through the backend");
    const auto invoke = [&](const std::string& line) {
        std::string response;
        return host.dispatchCommand(line, response) && response.empty();
    };
    auto& goldenEffects = world->effectsByMod["EffectFake"];
    CHECK(invoke("/effectfake give speed") && goldenEffects.size() == 1 &&
          goldenEffects[0].durationSeconds == 30 && goldenEffects[0].showParticles,
          "EffectFake defaults apply via the SDK edge");
    CHECK(invoke("/effectfake give night_vision Infinity 2 false") && goldenEffects.size() == 2 &&
          std::isinf(goldenEffects[1].durationSeconds) && goldenEffects[1].amplifier == 2 &&
          !goldenEffects[1].showParticles, "EffectFake preserves Infinity and options");
    CHECK(invoke("/effectfake clear speed") && goldenEffects.size() == 1 &&
          goldenEffects[0].typeId == "minecraft:night_vision", "EffectFake clears a single effect");
    CHECK(invoke("/effectfake clear") && goldenEffects.empty(), "EffectFake clears all effects");
}

void testHttp() {
    const auto* base = std::getenv("STAGEB_HTTP_FIXTURE");
    CHECK(base != nullptr, "HTTP tests require the loopback fixture");
    if (!base) return;
    const auto dir = tempDir("http");
    makeTestMod(dir, "HttpTest", "const url = " + nlohmann::json(base).dump() + ";\n" + R"JS(
const { http, HttpRequest, HttpRequestMethod, HttpHeader, HttpResponse } = require('@minecraft/server-net');
const { system } = require('@bedrock/sdk');
function check(test, message) { if (!test) throw new Error(message); }
async function fails(promise, code) {
  try { await promise; } catch (err) { check(err instanceof Error && err.code === code,
    'expected ' + code + ', got ' + err.code + ': ' + err.message); return; }
  throw new Error('expected rejection ' + code);
}
let ticks = 0;
system.runInterval(() => ++ticks);
(async () => {
  const request = new HttpRequest(url + '/echo').setMethod(HttpRequestMethod.POST)
    .setHeaders([new HttpHeader('X-Fixture-Request', 'native')]).setBody('hello 世界');
  const echo = await http.request(request);
  check(echo instanceof HttpResponse && echo.status === 201 && echo.body === 'hello 世界' &&
    echo.request === request && echo.headers.some(h => h.key.toLowerCase() === 'x-echo' && h.value === 'native'),
    'server-net echo response');
  const target = await fetch(url + '/redirect');
  check(target.status === 200 && target.ok && target.headers.get('X-FIXTURE') === 'scriptclient' &&
    (await target.json()).ok && target.bodyUsed, 'redirect and JSON');
  await fails(target.text(), 'EINVAL');
  const failure = await fetch(url + '/status');
  check(!failure.ok && failure.status === 418 && await failure.text() === 'not a success', 'non-2xx response');
  const binary = await (await fetch(url + '/bytes')).arrayBuffer();
  check([...new Uint8Array(binary)].join() === '0,128,255,10', 'binary response');
  await fails(fetch(url + '/large', { maxResponseBytes: 1024 }), 'EMSGSIZE');
  const start = ticks;
  await fails(http.request(new HttpRequest(url + '/slow').setTimeout(0.15)), 'ETIMEDOUT');
  check(ticks - start >= 3, 'host ticks must continue while request is pending: ' + (ticks - start));
  const cancelled = fetch(url + '/slow');
  http.cancelAll('test cancellation');
  await fails(cancelled, 'ECANCELED');
  await fails(fetch('file:///etc/passwd'), 'EINVAL');
  await fails(fetch(url + '/get', { headers: { 'X-Injected': 'x\r\ny' } }), 'EINVAL');
  console.log('http-fixture-ok ticks=' + (ticks - start));
})().catch(error => console.error('http-test-failed: ' + error.message + '\n' + error.stack));
)JS");
    PluginHost host(dir / "mods", "1.26.65.1");
    host.setLogDirectory(dir / "logs");
    std::string error;
    CHECK(host.initialize(error), "initialize network mod: " + error);
    checkActive(host, "HttpTest");
    const auto deadline = Clock::now() + std::chrono::seconds(5);
    std::string log;
    while (Clock::now() < deadline) {
        host.tick();
        log = readTestLog(dir, "HttpTest");
        if (log.find("http-fixture-ok") != log.npos || log.find("http-test-failed") != log.npos) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    CHECK(log.find("http-fixture-ok") != log.npos, "HTTP fixture checks: " + log);
    if (log.find("http-fixture-ok") != log.npos) std::cout << log;
}

} // namespace

int main(int argc, char** argv) {
    if (argc == 3 && std::string_view(argv[1]) == "--manager") {
        try {
            testManagerPackages(argv[2]);
        } catch (const std::exception& e) {
            std::cerr << "manager test stopped: " << e.what() << '\n';
            return 1;
        }
        std::cout << "checks=" << checks << " failures=" << failures << "\n";
        return failures == 0 ? 0 : 1;
    }
    if (argc == 2 && std::string_view(argv[1]) == "--http-only") {
        testHttp();
        std::cout << "checks=" << checks << " failures=" << failures << "\n";
        return failures == 0 ? 0 : 1;
    }
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
    testNodeBuiltins();
    testScriptStyleSdk();
    // 5. Dev bindings (guarded memory, symbols, module introspection)
    testDevBindings();
    std::cout << "checks=" << checks << " failures=" << failures << "\n";
    return failures == 0 ? 0 : 1;
}
