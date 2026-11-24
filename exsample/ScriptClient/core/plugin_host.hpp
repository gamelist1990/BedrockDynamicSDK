#pragma once

#include "http_client.hpp"

#include "command_model.hpp"
#include "common.hpp"
#include "host_context.hpp"
#include "js_runtime.hpp"
#include "log_sink.hpp"
#include "manifest.hpp"
#include "mod_scanner.hpp"
#include "module_loader.hpp"
#include "sdk_bindings.hpp"
#include "transpiler.hpp"

#include <functional>
#include <memory>
#include <vector>

namespace stageb {

class PreparedPackage;

struct ModView {
    std::string name;
    std::string displayName;
    std::string version;
    std::string apiVersion;
    ModStatus status;
    std::string error;
};

// Orchestrates the plugin realms (§1.2). One instance owns all ScriptMods and
// the single command-definition set; the Windows DLL wires it to
// NativeInjectedCommands and the extended ClientHost.
class PluginHost {
public:
    using DefinitionsChanged = std::function<void(const WireDefinitions&)>;

    PluginHost(fs::path modsDir, std::string gameVersion,
               std::shared_ptr<WorldBackend> world = nullptr);
    ~PluginHost();
    PluginHost(const PluginHost&) = delete;
    PluginHost& operator=(const PluginHost&) = delete;

    bool initialize(std::string& error);
    void tick();
    void tickMod(const std::string& name);
    void notifyPlayerSpawn(const std::string& name, std::uint64_t playerId = 0,
                           bool initialSpawn = true);
    void notifyPlayerSpawnAll(std::uint64_t playerId = 0, bool initialSpawn = true);
    void notifyKeybind(const std::string& name, std::string_view keybindId, bool pressed);
    void enqueueSettingChange(const std::string& name, const std::string& settingId,
                              const bedrock::sdk::SettingValue& value);
    bool reload(const std::string& name, std::string& error);
    bool install(PreparedPackage& package, std::string& error);
    std::string remove(const std::string& name, bool removeData);
    bool isEnabled(const std::string& name) const;
    void setEnabled(const std::string& name, bool enabled);
    void setStateDirectory(fs::path directory) { disabledDir_ = std::move(directory); }
    const fs::path& modsDir() const noexcept { return modsDir_; }
    const std::string& gameVersion() const noexcept { return gameVersion_; }

    const SettingsDecl* settingsFor(const std::string& name) const;
    const KeybindsDecl* keybindsFor(const std::string& name) const;

    std::vector<ModView> list() const;
    std::string handleModsCommand(const std::string& line);
    bool dispatchCommand(const std::string& line, std::string& response);

    void setDefinitionsChanged(DefinitionsChanged callback) {
        definitionsChanged_ = std::move(callback);
    }
    const WireDefinitions& definitions() const noexcept { return definitions_; }

    // Test hooks (no game required).
    void setWorldBackend(std::shared_ptr<WorldBackend> world) { world_ = std::move(world); }
    void setFaultBudgetMs(int milliseconds) { faultBudgetMs_ = milliseconds; }
    // Production hook: write each Mod's console.log lines to
    // <logDir>/<ModName>.log. Left empty in tests so lines() stays the seam.
    void setLogDirectory(fs::path logDir) { logDir_ = std::move(logDir); }
    // Production hook: persistent per-Mod file roots under
    // <storage root>/mod-data/<ModName>. Left empty in tests that do
    // not use files; tests set a temp directory for the same behavior.
    void setDataDirectory(fs::path dataDir) { dataDir_ = std::move(dataDir); }
    // Production hook: pass the loaded game profile JSON (symbols, target) to
    // every ScriptMod's bindings for the dev.* introspection surface.
    void setProfileJson(std::string profileJson) { profileJson_ = std::move(profileJson); }
    std::size_t loadedCount() const noexcept { return loaded_.size(); }

private:
    struct LoadedMod {
        Manifest manifest;
        fs::path dir;
        ModStatus status = ModStatus::Loading;
        std::string error;

        // Declaration order matters: JSValues must die before the runtime.
        std::unique_ptr<JsRuntime> runtime;
        std::unique_ptr<Transpiler> transpiler;
        std::unique_ptr<LogSink> log;
        std::unique_ptr<HostContext> host;
        std::unique_ptr<CommandRegistry> commands;
        std::unique_ptr<SdkBindings> bindings;
        std::unique_ptr<ModuleLoader> loader;
        std::unique_ptr<HttpClient> http;
        JSValue instance = JS_UNDEFINED;

        ~LoadedMod() {
            if (!JS_IsUndefined(instance) && runtime)
                JS_FreeValue(runtime->context(), instance);
        }
    };

    bool loadOne(const ModCandidate& candidate, std::unique_ptr<LoadedMod>& out,
                 std::string& error);
    void unloadOne(LoadedMod& mod);
    bool unloadByName(const std::string& name);
    bool constructAndEnable(LoadedMod& mod, std::string& error);
    bool callLifecycle(LoadedMod& mod, const char* method, std::string& error);
    void publishDefinitions();
    LoadedMod* findByName(const std::string& name);
    const LoadedMod* findByName(const std::string& name) const;
    static JSValue contextCallback(JSContext* ctx, JSValueConst self, int argc,
                                   JSValueConst* argv, int magic);

    fs::path modsDir_;
    fs::path logDir_;
    fs::path dataDir_;
    fs::path disabledDir_;
    std::string gameVersion_;
    std::string profileJson_;
    std::shared_ptr<WorldBackend> world_;
    std::vector<std::unique_ptr<LoadedMod>> loaded_;
    std::vector<ModCandidate> candidates_;
    WireDefinitions definitions_;
    DefinitionsChanged definitionsChanged_;
    int faultBudgetMs_ = 100;
    bool reloading_ = false;
};

} // namespace stageb
