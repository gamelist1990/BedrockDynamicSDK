#pragma once

#include "common.hpp"
#include <quickjs.h>

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

namespace stageb {

// RAII wrapper for a quickjs-ng JSValue that the caller owns.
class JsValue {
public:
    JsValue() : ctx_(nullptr), value_(JS_UNDEFINED) {}
    JsValue(JSContext* ctx, JSValue value) : ctx_(ctx), value_(value) {}
    ~JsValue() { reset(); }
    JsValue(const JsValue&) = delete;
    JsValue& operator=(const JsValue&) = delete;
    JsValue(JsValue&& other) noexcept : ctx_(other.ctx_), value_(other.value_) {
        other.ctx_ = nullptr;
        other.value_ = JS_UNDEFINED;
    }
    JsValue& operator=(JsValue&& other) noexcept {
        if (this != &other) {
            reset();
            ctx_ = other.ctx_;
            value_ = other.value_;
            other.ctx_ = nullptr;
            other.value_ = JS_UNDEFINED;
        }
        return *this;
    }
    JSValue get() const noexcept { return value_; }
    JSValue release() noexcept {
        const auto v = value_;
        ctx_ = nullptr;
        value_ = JS_UNDEFINED;
        return v;
    }
    void reset(JSValue replacement = JS_UNDEFINED) noexcept {
        if (ctx_) JS_FreeValue(ctx_, value_);
        value_ = replacement;
        // The replacement must belong to the same context; reset() is only
        // called with JS_UNDEFINED by callers.
    }
    explicit operator bool() const noexcept { return !JS_IsUndefined(value_) && !JS_IsNull(value_); }
private:
    JSContext* ctx_;
    JSValue value_;
};

struct JsOptions {
    std::size_t memoryMiB = 64;
    std::size_t maxStackSize = 1024 * 1024;
    int budgetMs = 100;
};

// One JSRuntime + one JSContext per plugin (§2.3). All calls must happen on
// the owning host thread. An over-budget call sets `faulted` and all later
// callbacks are suppressed by the caller (§8.4).
class JsRuntime {
public:
    explicit JsRuntime(JsOptions options = {});
    ~JsRuntime();
    JsRuntime(const JsRuntime&) = delete;
    JsRuntime& operator=(const JsRuntime&) = delete;

    bool initialize(std::string& error);
    JSContext* context() const noexcept { return ctx_; }
    void setContextOpaque(void* opaque) const noexcept {
        if (ctx_) JS_SetContextOpaque(ctx_, opaque);
    }
    void* contextOpaque() const noexcept {
        return ctx_ ? JS_GetContextOpaque(ctx_) : nullptr;
    }

    bool eval(const std::string& code, const std::string& filename, std::string& error);
    bool callMethod(JSValueConst object, const char* name,
                    const std::vector<JSValueConst>& args, JSValue& result,
                    std::string& error);
    bool callFunction(JSValueConst function, JSValueConst thisValue,
                      const std::vector<JSValueConst>& args, JSValue& result,
                      std::string& error);
    void drainJobs();

    bool faulted() const noexcept { return interrupted_; }
    void clearFault() noexcept { interrupted_ = false; }
    std::string takeException();

    // Budget scope: starts the wall-clock deadline, resets it on exit.
    class CallScope {
    public:
        explicit CallScope(JsRuntime& runtime) : runtime_(runtime) { runtime_.beginCall(); }
        ~CallScope() { runtime_.endCall(); }
    private:
        JsRuntime& runtime_;
    };

private:
    friend int interruptCallback(JSContext*, void*);
    void beginCall() noexcept;
    void endCall() noexcept;
    bool finishCall(bool ok, std::string& error);

    JsOptions options_;
    JSRuntime* runtime_ = nullptr;
    JSContext* ctx_ = nullptr;
    bool active_ = false;
    bool interrupted_ = false;
    Clock::time_point deadline_{};
};

// Helpers used by bindings and the module loader.
std::string jsToString(JSContext* ctx, JSValueConst value);
bool jsToBool(JSContext* ctx, JSValueConst value, bool fallback = false);
double jsToNumber(JSContext* ctx, JSValueConst value, double fallback = 0.0);
std::vector<std::string> jsToStringArray(JSContext* ctx, JSValueConst value);
std::uint32_t jsArrayLength(JSContext* ctx, JSValueConst value);
void setGlobalFunction(JSContext* ctx, const char* name, JSCFunction* function, void* opaque);

} // namespace stageb
