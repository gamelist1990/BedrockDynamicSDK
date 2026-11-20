#pragma once

#include "common.hpp"
#include "js_runtime.hpp"

#include <unordered_map>

namespace stageb {

// §4.1: Sucrase runs in an internal runtime, invisible to plugins. `.ts` and
// ESM syntax are both lowered to CommonJS so the host has a single module
// system. Transformed code is cached by (path, mtime, size, SHA-256).
class Transpiler {
public:
    Transpiler();
    ~Transpiler();
    Transpiler(const Transpiler&) = delete;
    Transpiler& operator=(const Transpiler&) = delete;

    bool initialize(std::string& error);
    bool transform(const fs::path& path, const std::string& source,
                   std::string& output, std::string& error);
    void clearCache();

private:
    struct CacheEntry {
        std::uintmax_t mtime = 0;
        std::uintmax_t size = 0;
        std::string hash;
        std::string code;
    };
    std::unique_ptr<JsRuntime> runtime_;
    JSValue transformFunction_ = JS_UNDEFINED;
    std::unordered_map<std::string, CacheEntry> cache_;
};

std::string sha256Hex(std::string_view data);

} // namespace stageb
