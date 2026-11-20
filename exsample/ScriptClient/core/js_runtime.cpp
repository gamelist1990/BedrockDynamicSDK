#include "js_runtime.hpp"

#include <algorithm>
#include <cstring>

namespace stageb {

int interruptCallback(JSContext*, void* opaque) {
    auto* self = static_cast<JsRuntime*>(opaque);
    if (!self->active_) return 0;
    if (Clock::now() >= self->deadline_) {
        self->interrupted_ = true;
        return 1;
    }
    return 0;
}

JsRuntime::JsRuntime(JsOptions options) : options_(std::move(options)) {}

JsRuntime::~JsRuntime() {
    if (ctx_) JS_FreeContext(ctx_);
    if (runtime_) JS_FreeRuntime(runtime_);
}

bool JsRuntime::initialize(std::string& error) {
    runtime_ = JS_NewRuntime();
    if (!runtime_) { error = "JS_NewRuntime failed"; return false; }
    JS_SetMemoryLimit(runtime_, options_.memoryMiB * 1024ull * 1024ull);
    JS_SetMaxStackSize(runtime_, options_.maxStackSize);
    JS_SetInterruptHandler(runtime_, &interruptCallback, this);
    ctx_ = JS_NewContext(runtime_);
    if (!ctx_) { error = "JS_NewContext failed"; return false; }
    error.clear();
    return true;
}

void JsRuntime::beginCall() noexcept {
    deadline_ = Clock::now() + std::chrono::milliseconds(options_.budgetMs);
    active_ = true;
}

void JsRuntime::endCall() noexcept { active_ = false; }

std::string JsRuntime::takeException() {
    if (!ctx_) return {};
    const JsValue exception(ctx_, JS_GetException(ctx_));
    if (JS_IsException(exception.get())) return "unknown JS exception";
    std::string text = jsToString(ctx_, exception.get());
    const JsValue stack(ctx_, JS_GetPropertyStr(ctx_, exception.get(), "stack"));
    if (!JS_IsUndefined(stack.get()) && !JS_IsNull(stack.get())) {
        const auto stackText = jsToString(ctx_, stack.get());
        if (!stackText.empty()) text += "\n" + stackText;
    }
    if (interrupted_) text += "\n(watchdog budget exceeded)";
    return text;
}

bool JsRuntime::finishCall(bool ok, std::string& error) {
    if (interrupted_) {
        // Clear the pending interruption exception so the realm can be torn
        // down cleanly by reload (§9).
        const JsValue exception(ctx_, JS_GetException(ctx_));
        error = "JS call exceeded the " + std::to_string(options_.budgetMs) + " ms budget";
        return false;
    }
    if (!ok) error = takeException();
    return ok;
}

bool JsRuntime::eval(const std::string& code, const std::string& filename, std::string& error) {
    if (!ctx_) { error = "runtime not initialized"; return false; }
    CallScope scope(*this);
    const JsValue result(ctx_, JS_Eval(ctx_, code.data(), code.size(), filename.c_str(),
                                       JS_EVAL_TYPE_GLOBAL));
    return finishCall(!JS_IsException(result.get()), error);
}

bool JsRuntime::callFunction(JSValueConst function, JSValueConst thisValue,
                             const std::vector<JSValueConst>& args, JSValue& result,
                             std::string& error) {
    if (!ctx_) { error = "runtime not initialized"; return false; }
    CallScope scope(*this);
    std::vector<JSValue> argv(args.begin(), args.end());
    JsValue returned(ctx_, JS_Call(ctx_, function, thisValue,
                                   static_cast<int>(argv.size()), argv.data()));
    const bool ok = !JS_IsException(returned.get());
    if (ok) result = returned.release();
    return finishCall(ok, error);
}

bool JsRuntime::callMethod(JSValueConst object, const char* name,
                           const std::vector<JSValueConst>& args, JSValue& result,
                           std::string& error) {
    if (!ctx_) { error = "runtime not initialized"; return false; }
    const JsValue method(ctx_, JS_GetPropertyStr(ctx_, object, name));
    if (!JS_IsFunction(ctx_, method.get())) {
        error = std::string("missing method: ") + name;
        return false;
    }
    return callFunction(method.get(), object, args, result, error);
}

void JsRuntime::drainJobs() {
    if (!ctx_) return;
    JSContext* jobContext = nullptr;
    while (true) {
        CallScope scope(*this);
        const int status = JS_ExecutePendingJob(runtime_, &jobContext);
        if (status <= 0 || interrupted_) break;
    }
}

std::string jsToString(JSContext* ctx, JSValueConst value) {
    size_t length = 0;
    const char* text = JS_ToCStringLen(ctx, &length, value);
    if (!text) return {};
    std::string out(text, length);
    JS_FreeCString(ctx, text);
    return out;
}

bool jsToBool(JSContext* ctx, JSValueConst value, bool fallback) {
    const int result = JS_ToBool(ctx, value);
    return result < 0 ? fallback : result != 0;
}

double jsToNumber(JSContext* ctx, JSValueConst value, double fallback) {
    double result = 0;
    return JS_ToFloat64(ctx, &result, value) < 0 ? fallback : result;
}

std::uint32_t jsArrayLength(JSContext* ctx, JSValueConst value) {
    std::int64_t length = 0;
    if (JS_IsArray(value) && JS_GetLength(ctx, value, &length) == 0 && length > 0)
        return static_cast<std::uint32_t>(length);
    return 0;
}

std::vector<std::string> jsToStringArray(JSContext* ctx, JSValueConst value) {
    std::vector<std::string> out;
    const auto length = jsArrayLength(ctx, value);
    out.reserve(length);
    for (uint32_t i = 0; i < length; ++i) {
        const JsValue item(ctx, JS_GetPropertyUint32(ctx, value, i));
        out.push_back(jsToString(ctx, item.get()));
    }
    return out;
}

void setGlobalFunction(JSContext* ctx, const char* name, JSCFunction* function, void* opaque) {
    JsValue fn(ctx, JS_NewCFunction(ctx, function, name, 0));
    const JsValue global(ctx, JS_GetGlobalObject(ctx));
    JS_DefinePropertyValueStr(ctx, global.get(), name, fn.release(), JS_PROP_C_W_E);
}

} // namespace stageb
