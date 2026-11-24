#pragma once

#include "common.hpp"
#include "host_context.hpp"
#include "js_runtime.hpp"
#include "transpiler.hpp"

#include <unordered_map>
#include <vector>

namespace stageb {

// §4.2: CommonJS require resolver with node_modules support, built-in module
// priority, TS-on-load transpilation and a per-path module cache (circular
// requires see partial exports, like Node).
class ModuleLoader {
public:
    ModuleLoader(JsRuntime& runtime, Transpiler& transpiler, const fs::path& modRoot,
                 HostContext& host);
    ~ModuleLoader();

    void setBuiltin(const std::string& name, JSValue value);
    bool installBuiltin(const char* name, const char* factory, JSValueConst argument,
                        std::string& error);
    bool loadEntry(const std::string& absolutePath, std::string& error);
    JSValue entryExports() const noexcept { return entryExports_; }
    void clearCache();

    const fs::path& root() const noexcept { return modRoot_; }

private:
    static JSValue requireCallback(JSContext* ctx, JSValueConst self, int argc,
                                   JSValueConst* argv, int magic);
    static JSValue consoleCallback(JSContext* ctx, JSValueConst self, int argc,
                                   JSValueConst* argv, int magic);

    std::optional<fs::path> resolveFile(const fs::path& base) const;
    std::optional<fs::path> resolveBare(const fs::path& fromDir,
                                        const std::string& specifier) const;
    bool loadModule(const fs::path& path, JSValue& exports, std::string& error);
    JSValue makeRequire(const fs::path& fromDir);
    void throwError(JSContext* ctx, const char* code, const std::string& message) const;

    JsRuntime& runtime_;
    Transpiler& transpiler_;
    fs::path modRoot_;
    HostContext& host_;
    std::unordered_map<std::string, JSValue> builtins_;
    std::unordered_map<std::string, JSValue> modules_;
    std::vector<fs::path> requireDirs_;
    JSValue entryExports_ = JS_UNDEFINED;
};

} // namespace stageb
