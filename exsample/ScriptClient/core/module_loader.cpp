#include "module_loader.hpp"
#include "log_sink.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <fstream>
#include <sstream>

namespace stageb {
namespace {

const char* const kExtensions[] = {"", ".js", ".ts", ".json", ".mjs"};
const char* const kIndexFiles[] = {"index.js", "index.ts", "index.json", "index.mjs"};

bool hasExtension(const fs::path& path) {
    const auto ext = path.extension().generic_string();
    return ext == ".js" || ext == ".ts" || ext == ".json" || ext == ".mjs";
}

} // namespace

ModuleLoader::ModuleLoader(JsRuntime& runtime, Transpiler& transpiler,
                           const fs::path& modRoot, HostContext& host)
    : runtime_(runtime), transpiler_(transpiler), modRoot_(fs::weakly_canonical(modRoot)),
      host_(host) {
    host_.loader = this;
    const auto ctx = runtime_.context();
    JsValue console(ctx, JS_NewObject(ctx));
    for (int level = 0; level < 4; ++level) {
        JsValue fn(ctx, JS_NewCFunctionMagic(ctx, &consoleCallback, "log", 0,
                                             JS_CFUNC_generic_magic, level));
        JS_DefinePropertyValueStr(ctx, console.get(),
                                  (level == 0 ? "log" : level == 1 ? "info"
                                   : level == 2 ? "warn" : "error"),
                                  fn.release(), JS_PROP_C_W_E);
    }
    const JsValue global(ctx, JS_GetGlobalObject(ctx));
    JS_DefinePropertyValueStr(ctx, global.get(), "console", console.release(), JS_PROP_C_W_E);
}

void ModuleLoader::setBuiltin(const std::string& name, JSValue value) {
    const auto ctx = runtime_.context();
    const auto it = builtins_.find(name);
    if (it != builtins_.end()) JS_FreeValue(ctx, it->second);
    builtins_[name] = JS_DupValue(ctx, value);
}

void ModuleLoader::clearCache() {
    const auto ctx = runtime_.context();
    for (const auto& [key, value] : modules_) {
        (void)key;
        JS_FreeValue(ctx, value);
    }
    modules_.clear();
    requireDirs_.clear();
    if (!JS_IsUndefined(entryExports_)) JS_FreeValue(ctx, entryExports_);
    entryExports_ = JS_UNDEFINED;
    transpiler_.clearCache();
}

void ModuleLoader::throwError(JSContext* ctx, const char* code,
                              const std::string& message) const {
    JsValue error(ctx, JS_NewError(ctx));
    JS_SetPropertyStr(ctx, error.get(), "code", JS_NewString(ctx, code));
    JS_SetPropertyStr(ctx, error.get(), "message", JS_NewString(ctx, message.c_str()));
    JS_Throw(ctx, error.release());
}

std::optional<fs::path> ModuleLoader::resolveFile(const fs::path& base) const {
    std::error_code ec;
    for (const char* suffix : kExtensions) {
        auto candidate = base;
        if (suffix[0] != '\0') candidate += suffix;
        if (!fs::is_regular_file(candidate, ec)) continue;
        auto resolved = confinedResolve(modRoot_, candidate);
        if (resolved) return resolved;
    }
    for (const char* index : kIndexFiles) {
        auto candidate = base / index;
        if (!fs::is_regular_file(candidate, ec)) continue;
        auto resolved = confinedResolve(modRoot_, candidate);
        if (resolved) return resolved;
    }
    return std::nullopt;
}

std::optional<fs::path> ModuleLoader::resolveBare(const fs::path& fromDir,
                                                    const std::string& specifier) const {
    fs::path dir = fromDir;
    while (true) {
        const auto candidate = dir / "node_modules" / specifier;
        std::error_code ec;
        if (fs::is_directory(candidate, ec)) {
            const auto packageJson = candidate / "package.json";
            std::string main;
            if (fs::is_regular_file(packageJson, ec)) {
                std::ifstream in(packageJson);
                try {
                    nlohmann::json doc;
                    in >> doc;
                    if (doc.is_object() && doc.contains("main") && doc["main"].is_string())
                        main = doc["main"].get<std::string>();
                } catch (...) { /* fall through to index files */ }
            }
            if (!main.empty()) {
                const auto mainPath = candidate / main;
                if (auto resolved = resolveFile(mainPath)) return resolved;
            }
            if (auto resolved = resolveFile(candidate)) return resolved;
        }
        if (auto resolved = resolveFile(candidate)) return resolved;
        if (dir == modRoot_ || dir.parent_path() == dir) break;
        auto parent = dir.parent_path();
        if (!confinedResolve(modRoot_, parent)) break;
        dir = parent;
    }
    return std::nullopt;
}

JSValue ModuleLoader::makeRequire(const fs::path& fromDir) {
    requireDirs_.push_back(fromDir);
    const auto magic = static_cast<int>(requireDirs_.size() - 1);
    return JS_NewCFunctionMagic(runtime_.context(), &requireCallback, "require", 1,
                                JS_CFUNC_generic_magic, magic);
}

bool ModuleLoader::loadEntry(const std::string& absolutePath, std::string& error) {
    const auto path = confinedResolve(modRoot_, absolutePath);
    if (!path) { error = "entry escapes the mod directory"; return false; }
    JSValue exports = JS_UNDEFINED;
    const bool ok = loadModule(*path, exports, error);
    if (ok) entryExports_ = exports;
    return ok;
}

bool ModuleLoader::loadModule(const fs::path& path, JSValue& exports, std::string& error) {
    const auto ctx = runtime_.context();
    const auto key = path.generic_string();
    if (const auto it = modules_.find(key); it != modules_.end()) {
        exports = JS_DupValue(ctx, it->second);
        return true;
    }

    std::ifstream file(path, std::ios::binary);
    if (!file) { error = "cannot open module: " + key; return false; }
    std::ostringstream buffer;
    buffer << file.rdbuf();
    const auto source = buffer.str();

    JsValue moduleExports;
    if (path.extension() == ".json") {
        moduleExports = JsValue(ctx, JS_ParseJSON(ctx, source.data(), source.size(), key.c_str()));
        if (JS_IsException(moduleExports.get())) {
            error = runtime_.takeException();
            return false;
        }
        modules_[key] = JS_DupValue(ctx, moduleExports.get());
    } else {
        std::string code;
        if (!transpiler_.transform(path, source, code, error)) return false;
        const auto wrapper = "(function(exports, require, module, __filename, __dirname){\n" +
                             code + "\n})";
        const JsValue function(ctx, JS_Eval(ctx, wrapper.data(), wrapper.size(),
                                            key.c_str(), JS_EVAL_TYPE_GLOBAL));
        if (JS_IsException(function.get())) { error = runtime_.takeException(); return false; }
        JsValue localExports(ctx, JS_NewObject(ctx));
        // Publish partial exports before evaluation so circular requires see
        // the same object (Node semantics, §4.2).
        modules_[key] = JS_DupValue(ctx, localExports.get());
        JsValue moduleObject(ctx, JS_NewObject(ctx));
        JS_SetPropertyStr(ctx, moduleObject.get(), "exports", JS_DupValue(ctx, localExports.get()));
        JS_SetPropertyStr(ctx, moduleObject.get(), "id", JS_NewString(ctx, key.c_str()));
        const auto dir = path.parent_path().generic_string();
        const JsValue filename(ctx, JS_NewString(ctx, key.c_str()));
        const JsValue dirname(ctx, JS_NewString(ctx, dir.c_str()));
        const JsValue requireFn(ctx, makeRequire(path.parent_path()));
        std::array<JSValue, 5> args{localExports.get(), requireFn.get(), moduleObject.get(),
                                    filename.get(), dirname.get()};
        JsRuntime::CallScope scope(runtime_);
        const JsValue ignored(ctx, JS_Call(ctx, function.get(), JS_UNDEFINED, 5, args.data()));
        if (runtime_.faulted()) {
            runtime_.clearFault();
            JS_GetException(ctx);
            error = "module evaluation exceeded its budget: " + key;
            return false;
        }
        if (JS_IsException(ignored.get())) { error = runtime_.takeException(); return false; }
        // CommonJS allows rebinding module.exports.
        JsValue rebound(ctx, JS_GetPropertyStr(ctx, moduleObject.get(), "exports"));
        if (!JS_IsUndefined(rebound.get())) {
            localExports = JsValue(ctx, rebound.release());
            JS_FreeValue(ctx, modules_[key]);
            modules_[key] = JS_DupValue(ctx, localExports.get());
        }
        moduleExports = std::move(localExports);
    }

    exports = moduleExports.release();
    error.clear();
    return true;
}

JSValue ModuleLoader::requireCallback(JSContext* ctx, JSValueConst, int argc,
                                      JSValueConst* argv, int magic) {
    auto* host = static_cast<HostContext*>(JS_GetContextOpaque(ctx));
    if (!host || !host->loader) {
        host->loader->throwError(ctx, "Internal", "module loader unavailable");
        return JS_EXCEPTION;
    }
    auto* self = host->loader;
    if (argc < 1 || !JS_IsString(argv[0])) {
        self->throwError(ctx, "Invalid", "require() expects a module name");
        return JS_EXCEPTION;
    }
    const auto specifier = jsToString(ctx, argv[0]);
    if (const auto it = self->builtins_.find(specifier); it != self->builtins_.end())
        return JS_DupValue(ctx, it->second);

    static const char* const unsupported[] = {"fs", "path", "events", "os", "net", "http",
                                              "https", "child_process", "worker_threads"};
    for (const char* name : unsupported)
        if (specifier == name || specifier.rfind(std::string(name) + "/", 0) == 0) {
            self->throwError(ctx, "Unavailable",
                             std::string("unsupported built-in module: ") + specifier);
            return JS_EXCEPTION;
        }

    const auto fromDir = self->requireDirs_[static_cast<std::size_t>(magic)];
    std::optional<fs::path> resolved;
    if (!specifier.empty() && (specifier[0] == '.' || specifier[0] == '/'))
        resolved = self->resolveFile(fromDir / specifier);
    else
        resolved = self->resolveBare(fromDir, specifier);
    if (!resolved) {
        self->throwError(ctx, "NotFound", "cannot resolve module: " + specifier);
        return JS_EXCEPTION;
    }
    JSValue exports = JS_UNDEFINED;
    std::string error;
    if (!self->loadModule(*resolved, exports, error)) {
        self->throwError(ctx, "Invalid", error);
        return JS_EXCEPTION;
    }
    return exports;
}

JSValue ModuleLoader::consoleCallback(JSContext* ctx, JSValueConst, int argc,
                                      JSValueConst* argv, int magic) {
    auto* host = static_cast<HostContext*>(JS_GetContextOpaque(ctx));
    std::string line;
    for (int i = 0; i < argc; ++i) {
        if (i) line += ' ';
        line += jsToString(ctx, argv[i]);
    }
    if (host && host->log) {
        const char* levels[] = {"info", "info", "warn", "error"};
        host->log->write(levels[magic & 3], line);
    }
    return JS_UNDEFINED;
}

} // namespace stageb
