#include "plugin_host.hpp"
#include "vanilla_catalog.hpp"

#include <array>
#include <sstream>

namespace stageb {
namespace {

constexpr const char* kContextOps[] = {"get", "getOptional", "rawLine", "respond"};

class NullWorldBackend final : public WorldBackend {
public:
    bool hasLocalPlayer() const override { return false; }
};

JSValue vanillaAllEffectIds(JSContext* ctx, JSValueConst, int, JSValueConst*) {
    JsValue global(ctx, JS_GetGlobalObject(ctx));
    JsValue ids(ctx, JS_GetPropertyStr(ctx, global.get(), "__stagebVanillaIds"));
    return JS_DupValue(ctx, ids.get());
}

} // namespace

PluginHost::PluginHost(fs::path modsDir, std::string gameVersion,
                       std::shared_ptr<WorldBackend> world)
    : modsDir_(std::move(modsDir)), gameVersion_(std::move(gameVersion)),
      world_(std::move(world)) {}

PluginHost::~PluginHost() = default;

bool PluginHost::initialize(std::string& error) {
    if (!world_) world_ = std::make_shared<NullWorldBackend>();
    const auto scan = scanMods(modsDir_, gameVersion_);
    candidates_ = scan.candidates;
    loaded_.clear();
    for (const auto& candidate : candidates_) {
        std::unique_ptr<LoadedMod> mod;
        std::string modError;
        if (!loadOne(candidate, mod, modError)) {
            if (!mod) {
                mod = std::make_unique<LoadedMod>();
                mod->dir = candidate.dir;
                mod->manifest.name = candidate.folderName;
                mod->manifest.displayName = candidate.folderName;
                mod->status = candidate.status == ModStatus::Scanned ? ModStatus::Error
                                                                     : candidate.status;
                mod->error = modError.empty() ? candidate.validation.error : modError;
            }
        }
        loaded_.push_back(std::move(mod));
    }
    publishDefinitions();
    error.clear();
    return true;
}

bool PluginHost::loadOne(const ModCandidate& candidate, std::unique_ptr<LoadedMod>& out,
                         std::string& error) {
    auto mod = std::make_unique<LoadedMod>();
    mod->dir = candidate.dir;
    if (!candidate.validation.ok) {
        mod->status = candidate.validation.incompatible ? ModStatus::Incompatible
                                                        : ModStatus::Error;
        mod->error = candidate.validation.error;
        out = std::move(mod);
        error = out->error;
        return false;
    }
    mod->manifest = candidate.validation.manifest;

    JsOptions options;
    options.memoryMiB = mod->manifest.memoryMiB;
    options.budgetMs = faultBudgetMs_;
    mod->runtime = std::make_unique<JsRuntime>(options);
    if (!mod->runtime->initialize(error)) { out = std::move(mod); return false; }
    const auto ctx = mod->runtime->context();

    mod->transpiler = std::make_unique<Transpiler>();
    if (!mod->transpiler->initialize(error)) { out = std::move(mod); return false; }

    mod->log = std::make_unique<LogSink>(mod->manifest.name);
    // Bug 5: the production host never called LogSink::open(), so every
    // console.log line stayed in the in-memory vector and was discarded on
    // unload. Open <logDir>/<ModName>.log when a directory was configured.
    if (!logDir_.empty()) {
        std::error_code ignored;
        fs::create_directories(logDir_, ignored);
        std::string safeName = mod->manifest.name;
        for (char& ch : safeName)
            if (ch == '/' || ch == '\\' || ch == ':' || ch == '*' || ch == '?' ||
                ch == '"' || ch == '<' || ch == '>' || ch == '|')
                ch = '_';
        (void)mod->log->open(logDir_ / (safeName + ".log"));
    }
    mod->host = std::make_unique<HostContext>();
    mod->host->log = mod->log.get();
    mod->host->world = world_.get();
    mod->runtime->setContextOpaque(mod->host.get());
    mod->commands = std::make_unique<CommandRegistry>(*mod->runtime);
    mod->bindings = std::make_unique<SdkBindings>(*mod->runtime, *mod->host,
                                                  *mod->commands, *world_,
                                                  mod->manifest.name, dataDir_);
    mod->commands->setDescription(mod->manifest.description);
    if (!mod->bindings->install(error)) { out = std::move(mod); return false; }
    mod->loader = std::make_unique<ModuleLoader>(*mod->runtime, *mod->transpiler,
                                                 mod->dir, *mod->host);
    mod->loader->setBuiltin("@bedrock/sdk", mod->bindings->moduleObject());

    // §4.3: vanilla-data is generated from the same catalog as the C++/TS
    // headers (tools/vanilla_data.py). Built-ins always win over node_modules.
    JsValue vanilla(ctx, JS_NewObject(ctx));
    JsValue types(ctx, JS_NewObject(ctx));
    JsValue ids(ctx, JS_NewArray(ctx));
    int index = 0;
    for (const auto& effect : kVanillaEffects) {
        JS_SetPropertyStr(ctx, types.get(), effect.name, JS_NewString(ctx, effect.id));
        JS_SetPropertyInt64(ctx, ids.get(), index++, JS_NewString(ctx, effect.id));
    }
    JsValue global(ctx, JS_GetGlobalObject(ctx));
    JS_DefinePropertyValueStr(ctx, global.get(), "__stagebVanillaIds", JS_DupValue(ctx, ids.get()),
                              JS_PROP_C_W_E);
    JS_SetPropertyStr(ctx, vanilla.get(), "MinecraftEffectTypes", types.release());
    JS_SetPropertyStr(ctx, vanilla.get(), "dataVersion", JS_NewString(ctx, kVanillaCatalogVersion));
    JsValue allIds(ctx, JS_NewCFunction(ctx, &vanillaAllEffectIds, "allEffectIds", 0));
    JS_SetPropertyStr(ctx, vanilla.get(), "allEffectIds", allIds.release());
    mod->loader->setBuiltin("@bedrock/vanilla-data", vanilla.get());

    if (!mod->loader->loadEntry(mod->manifest.entry, error)) { out = std::move(mod); return false; }
    if (!constructAndEnable(*mod, error)) { out = std::move(mod); return false; }
    mod->status = ModStatus::Active;
    out = std::move(mod);
    error.clear();
    return true;
}

bool PluginHost::constructAndEnable(LoadedMod& mod, std::string& error) {
    const auto ctx = mod.runtime->context();
    const JsValue exports(ctx, JS_GetPropertyStr(ctx, mod.loader->entryExports(), "default"));
    JsValue instance(ctx, JS_UNDEFINED);
    if (JS_IsConstructor(ctx, exports.get())) {
        JsRuntime::CallScope scope(*mod.runtime);
        instance = JsValue(ctx, JS_CallConstructor(ctx, exports.get(), 0, nullptr));
        if (JS_IsException(instance.get())) {
            error = mod.runtime->takeException();
            return false;
        }
        mod.instance = instance.release();
    }
    // Both forms drive onEnable; the class form keeps the instance for later
    // lifecycle calls, the fallback form uses the module exports object.
    const auto target = !JS_IsUndefined(mod.instance) ? mod.instance
                                                      : mod.loader->entryExports();
    if (!callLifecycle(mod, "onEnable", error)) return false;
    mod.host->bindings = mod.bindings.get();
    (void)target;
    return true;
}

bool PluginHost::callLifecycle(LoadedMod& mod, const char* method, std::string& error) {
    const auto ctx = mod.runtime->context();
    const auto target = !JS_IsUndefined(mod.instance) ? mod.instance
                                                      : mod.loader->entryExports();
    const JsValue methodValue(ctx, JS_GetPropertyStr(ctx, target, method));
    if (JS_IsUndefined(methodValue.get()) || JS_IsNull(methodValue.get())) {
        error.clear();
        return true; // lifecycle hooks are optional
    }
    if (!JS_IsFunction(ctx, methodValue.get())) {
        error = std::string(method) + " is not a function";
        return false;
    }
    JSValue result = JS_UNDEFINED;
    const bool ok = mod.runtime->callFunction(methodValue.get(), target, {}, result, error);
    if (!JS_IsUndefined(result)) JS_FreeValue(ctx, result);
    return ok;
}

void PluginHost::unloadOne(LoadedMod& mod) {
    if (mod.status == ModStatus::Active || mod.status == ModStatus::Faulted) {
        std::string ignored;
        callLifecycle(mod, "onDisable", ignored);
    }
    if (mod.bindings) mod.bindings->forceCleanup();
    mod.loader.reset();
    mod.bindings.reset();
    mod.commands.reset();
    mod.host.reset();
    mod.log.reset();
    mod.transpiler.reset();
    if (!JS_IsUndefined(mod.instance)) {
        JS_FreeValue(mod.runtime->context(), mod.instance);
        mod.instance = JS_UNDEFINED;
    }
    mod.runtime.reset();
    mod.status = ModStatus::Scanned;
}

void PluginHost::tick() {
    for (const auto& mod : loaded_)
        if (mod) tickMod(mod->manifest.name);
}

void PluginHost::tickMod(const std::string& name) {
    auto* mod = findByName(name);
    if (!mod || mod->status != ModStatus::Active || !mod->runtime) return;
    mod->runtime->drainJobs();
    if (mod->bindings) mod->bindings->advanceTick();
    if (mod->runtime->faulted()) {
        mod->status = ModStatus::Faulted;
        mod->error = "JS call exceeded watchdog budget";
        mod->log->write("error", "faulted: " + mod->error);
        mod->runtime->clearFault();
        return;
    }
    std::string error;
    if (!callLifecycle(*mod, "onUpdate", error)) {
        mod->status = ModStatus::Faulted;
        mod->error = error;
        mod->log->write("error", "faulted: " + error);
    }
    if (mod->runtime->faulted()) mod->runtime->clearFault();
    mod->runtime->drainJobs();
}

void PluginHost::notifyPlayerSpawn(const std::string& name, std::uint64_t playerId,
                                   bool initialSpawn) {
    auto* mod = findByName(name);
    if (!mod || mod->status != ModStatus::Active || !mod->runtime || !mod->bindings) return;
    if (playerId == 0 && world_) {
        if (const auto id = world_->localPlayerId(name)) playerId = *id;
        else playerId = 1;
    }
    mod->bindings->notifyPlayerSpawn(playerId, initialSpawn);
    if (mod->runtime->faulted()) {
        mod->status = ModStatus::Faulted;
        mod->error = "JS playerSpawn callback exceeded watchdog budget";
        mod->log->write("error", "faulted: " + mod->error);
        mod->runtime->clearFault();
        return;
    }
    mod->runtime->drainJobs();
}

void PluginHost::notifyPlayerSpawnAll(std::uint64_t playerId, bool initialSpawn) {
    for (const auto& mod : loaded_)
        if (mod && mod->status == ModStatus::Active)
            notifyPlayerSpawn(mod->manifest.name, playerId, initialSpawn);
}

void PluginHost::notifyKeybind(const std::string& name, std::string_view keybindId,
                               bool pressed) {
    auto* mod = findByName(name);
    if (!mod || mod->status != ModStatus::Active || !mod->runtime || !mod->bindings) return;
    mod->bindings->notifyKeybind(keybindId, pressed);
    if (mod->runtime->faulted()) {
        mod->status = ModStatus::Faulted;
        mod->error = "JS keybind callback exceeded watchdog budget";
        mod->log->write("error", "faulted: " + mod->error);
        mod->runtime->clearFault();
        return;
    }

    const auto ctx = mod->runtime->context();
    const auto target = !JS_IsUndefined(mod->instance) ? mod->instance
                                                       : mod->loader->entryExports();
    const JsValue methodValue(ctx, JS_GetPropertyStr(ctx, target, "onKeybind"));
    if (JS_IsFunction(ctx, methodValue.get())) {
        JsValue idArg(ctx, JS_NewStringLen(ctx, keybindId.data(), keybindId.size()));
        JsValue pressedArg(ctx, JS_NewBool(ctx, pressed));
        JSValue result = JS_UNDEFINED;
        std::string error;
        const bool ok = mod->runtime->callFunction(
            methodValue.get(), target, {idArg.get(), pressedArg.get()}, result, error);
        if (!JS_IsUndefined(result)) JS_FreeValue(ctx, result);
        if (!ok && mod->runtime->faulted()) {
            mod->status = ModStatus::Faulted;
            mod->error = error;
            mod->log->write("error", "faulted: " + error);
            mod->runtime->clearFault();
            return;
        }
    }
    mod->runtime->drainJobs();
}

void PluginHost::enqueueSettingChange(const std::string& name, const std::string& settingId,
                                      const bedrock::sdk::SettingValue& value) {
    auto* mod = findByName(name);
    if (!mod || !mod->bindings) return;
    mod->bindings->enqueueSettingChange(settingId, value);
}

const SettingsDecl* PluginHost::settingsFor(const std::string& name) const {
    const auto* mod = findByName(name);
    return (mod && mod->bindings) ? &mod->bindings->settings() : nullptr;
}

const KeybindsDecl* PluginHost::keybindsFor(const std::string& name) const {
    const auto* mod = findByName(name);
    return (mod && mod->bindings) ? &mod->bindings->keybinds() : nullptr;
}

bool PluginHost::reload(const std::string& name, std::string& error) {
    if (reloading_) { error = "reload already in progress"; return false; }
    reloading_ = true;
    struct Guard { bool& flag; ~Guard() { flag = false; } } guard{reloading_};

    std::size_t index = 0;
    bool foundExisting = false;
    for (std::size_t i = 0; i < loaded_.size(); ++i) {
        if (loaded_[i] && loaded_[i]->manifest.name == name) {
            index = i;
            foundExisting = true;
            break;
        }
    }
    if (!foundExisting) { error = "unknown mod: " + name; return false; }
    auto* existing = loaded_[index].get();
    unloadOne(*existing);

    const auto scan = scanMods(modsDir_, gameVersion_);
    candidates_ = scan.candidates;
    const ModCandidate* found = nullptr;
    for (const auto& candidate : candidates_)
        if (candidate.folderName == name) { found = &candidate; break; }
    if (!found) { error = "mod directory disappeared: " + name; return false; }

    std::unique_ptr<LoadedMod> replacement;
    std::string loadError;
    const bool ok = loadOne(*found, replacement, loadError);
    if (!replacement) {
        replacement = std::make_unique<LoadedMod>();
        replacement->dir = found->dir;
        replacement->manifest.name = name;
        replacement->status = ModStatus::Error;
    }
    loaded_[index] = std::move(replacement);
    if (!ok) error = loadError;
    publishDefinitions();
    return ok;
}

PluginHost::LoadedMod* PluginHost::findByName(const std::string& name) {
    for (const auto& mod : loaded_)
        if (mod && mod->manifest.name == name) return mod.get();
    return nullptr;
}

const PluginHost::LoadedMod* PluginHost::findByName(const std::string& name) const {
    for (const auto& mod : loaded_)
        if (mod && mod->manifest.name == name) return mod.get();
    return nullptr;
}

void PluginHost::publishDefinitions() {
    std::vector<CommandDecl> combinedDeclarations;
    for (const auto& mod : loaded_) {
        if (!mod || mod->status != ModStatus::Active || !mod->commands) continue;
        for (const auto& decl : mod->commands->declarations()) {
            auto copy = decl;
            if (copy.description.empty()) copy.description = mod->manifest.description;
            combinedDeclarations.push_back(copy);
        }
    }
    auto wire = buildWireDefinitions(combinedDeclarations);
    // The /mods command itself is installed by the Windows command service;
    // here we only publish plugin definitions.
    definitions_ = std::move(wire);
    if (definitionsChanged_) definitionsChanged_(definitions_);
}

std::vector<ModView> PluginHost::list() const {
    std::vector<ModView> views;
    for (const auto& mod : loaded_) {
        if (!mod) continue;
        views.push_back({mod->manifest.name, mod->manifest.displayName, mod->manifest.version,
                         mod->manifest.apiVersion, mod->status, mod->error});
    }
    return views;
}

std::string PluginHost::handleModsCommand(const std::string& line) {
    const auto tokens = tokenize(line);
    if (tokens.empty() || tokens[0] != "mods") return "usage: /mods list|reload <name>|path";
    const std::string action = tokens.size() > 1 ? tokens[1] : "";
    if (action == "path") return modsDir_.string();
    if (action == "list") {
        std::ostringstream out;
        for (const auto& view : list())
            out << view.name << " " << view.displayName << " v" << view.version << " ["
                << statusName(view.status) << "] api " << view.apiVersion
                << (view.error.empty() ? "" : " - " + view.error) << "\n";
        return out.str();
    }
    if (action == "reload" && tokens.size() > 2) {
        std::string error;
        return reload(tokens[2], error) ? "reloaded " + tokens[2] : "reload failed: " + error;
    }
    return "usage: /mods list|reload <name>|path";
}

bool PluginHost::dispatchCommand(const std::string& line, std::string& response) {
    response.clear();
    for (const auto& slot : loaded_) {
        if (!slot || slot->status != ModStatus::Active || !slot->commands) continue;
        for (const auto& decl : slot->commands->declarations()) {
            const auto tokens = tokenize(line);
            if (tokens.empty() || tokens[0] != decl.name) continue;
            const auto parsed = parseCommandLine(decl, line);
            if (!parsed.ok) {
                response = parsed.error + "\nusage: /" + decl.name;
                return true;
            }
            const auto& branch = decl.branches[parsed.branch];
            if (branch.handlerIndex == static_cast<std::size_t>(-1)) return false;
            const auto ctx = slot->runtime->context();
            const auto handler = slot->commands->handler(branch.handlerIndex);
            if (JS_IsUndefined(handler)) return false;

            JsValue contextObject(ctx, JS_NewObject(ctx));
            for (int op = 0; op < 4; ++op) {
                JsValue fn(ctx, JS_NewCFunctionMagic(ctx, &contextCallback, kContextOps[op], 1,
                                                    JS_CFUNC_generic_magic, op));
                JS_DefinePropertyValueStr(ctx, contextObject.get(), kContextOps[op],
                                          fn.release(), JS_PROP_C_W_E);
            }
            slot->host->activeValues = &parsed.values;
            slot->host->response = &response;
            slot->host->rawLine = &line;
            JSValue result = JS_UNDEFINED;
            std::string error;
            // A TS handler is written as `(ctx) => ...`. The context object was
            // only installed as `this`, so `ctx` arrived as undefined. Pass it
            // as the first argument as well (keeping `this` for compatibility).
            const bool ok = slot->runtime->callFunction(
                handler, contextObject.get(), {contextObject.get()}, result, error);
            if (!JS_IsUndefined(result)) JS_FreeValue(ctx, result);
            slot->host->activeValues = nullptr;
            slot->host->response = nullptr;
            slot->host->rawLine = nullptr;
            if (!ok) response = error;
            return true;
        }
    }
    return false;
}

JSValue PluginHost::contextCallback(JSContext* ctx, JSValueConst, int argc,
                                    JSValueConst* argv, int magic) {
    auto* host = static_cast<HostContext*>(JS_GetContextOpaque(ctx));
    if (!host) return JS_ThrowTypeError(ctx, "command context unavailable");
    switch (magic) {
        case 0: // get
        case 1: { // getOptional
            const auto name = argc > 0 ? jsToString(ctx, argv[0]) : std::string();
            if (!host->activeValues) {
                if (magic == 1) return JS_UNDEFINED;
                JsValue error(ctx, JS_NewError(ctx));
                JS_SetPropertyStr(ctx, error.get(), "code", JS_NewString(ctx, "Unavailable"));
                JS_SetPropertyStr(ctx, error.get(), "message",
                                  JS_NewString(ctx, "no active command context"));
                return JS_Throw(ctx, error.release());
            }
            const auto it = host->activeValues->find(name);
            if (it == host->activeValues->end()) {
                if (magic == 1) return JS_UNDEFINED;
                JsValue error(ctx, JS_NewError(ctx));
                JS_SetPropertyStr(ctx, error.get(), "code", JS_NewString(ctx, "Invalid"));
                JS_SetPropertyStr(ctx, error.get(), "message",
                                  JS_NewString(ctx, ("missing argument: " + name).c_str()));
                return JS_Throw(ctx, error.release());
            }
            const auto& value = it->second;
            switch (value.kind) {
                case ArgKind::Effect:
                case ArgKind::String:
                case ArgKind::Message:
                    return JS_NewString(ctx, value.text.c_str());
                case ArgKind::Duration:
                case ArgKind::Int:
                case ArgKind::Float:
                    return JS_NewFloat64(ctx, value.number);
                case ArgKind::Bool:
                    return JS_NewBool(ctx, value.boolean);
            }
            return JS_UNDEFINED;
        }
        case 2: // rawLine
            return JS_NewString(ctx, host->rawLine ? host->rawLine->c_str() : "");
        case 3: // respond
            if (host->response) *host->response += argc > 0 ? jsToString(ctx, argv[0]) : "";
            return JS_UNDEFINED;
    }
    return JS_UNDEFINED;
}

} // namespace stageb
