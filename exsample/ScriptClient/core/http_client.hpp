#pragma once

#include "js_runtime.hpp"

namespace stageb {

class ModuleLoader;

// All JS state stays on the host thread. Workers only own plain request/result data.
class HttpClient {
public:
    explicit HttpClient(JsRuntime& runtime);
    ~HttpClient();
    bool install(ModuleLoader& loader, std::string& error);
    void advance();
private:
    struct Pending;
    static JSValue submit(JSContext*, JSValueConst, int, JSValueConst*);
    static JSValue cancel(JSContext*, JSValueConst, int, JSValueConst*);
    JsRuntime& runtime_;
    std::vector<std::unique_ptr<Pending>> pending_;
};

} // namespace stageb
