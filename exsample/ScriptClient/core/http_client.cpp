#include "http_client.hpp"

#include "builtin_sources.hpp"
#include "host_context.hpp"
#include "http_transport.hpp"
#include "module_loader.hpp"

#include <nlohmann/json.hpp>
#include <thread>

namespace stageb {
namespace {
constexpr int kMaxWorkers = 16;
constexpr std::size_t kMaxPending = 4;
constexpr std::size_t kMaxBytes = 8 * 1024 * 1024;
constexpr int kMaxTimeoutMs = 30'000;
std::atomic<int> workers{0};

struct Work {
    std::atomic<bool> cancelled{false};
    std::atomic<bool> done{false};
    Clock::time_point finished;
    http::Result result;
};

bool token(std::string_view value) {
    constexpr std::string_view punctuation("!#$%&'*+-.^_`|~");
    if (value.empty()) return false;
    for (const unsigned char ch : value)
        if (!(ch >= '0' && ch <= '9') && !(ch >= 'A' && ch <= 'Z') &&
            !(ch >= 'a' && ch <= 'z') && punctuation.find(ch) == punctuation.npos)
            return false;
    return true;
}

bool forbiddenHeader(std::string_view key) {
    return key == "connection" || key == "keep-alive" || key == "proxy-connection" ||
           key == "host" || key == "content-length" || key == "transfer-encoding" ||
           key == "upgrade" || key == "expect" || key == "trailer" || key == "te" ||
           key.starts_with("proxy-");
}

JSValue makeError(JSContext* ctx, const char* code, std::string_view message) {
    JsValue error(ctx, JS_NewError(ctx));
    if (JS_IsException(error.get())) return JS_EXCEPTION;
    if (JS_DefinePropertyValueStr(ctx, error.get(), "code", JS_NewString(ctx, code), JS_PROP_C_W_E) < 0 ||
        JS_DefinePropertyValueStr(ctx, error.get(), "message",
                                  JS_NewStringLen(ctx, message.data(), message.size()),
                                  JS_PROP_CONFIGURABLE | JS_PROP_WRITABLE) < 0)
        return JS_EXCEPTION;
    return error.release();
}

JSValue throwError(JSContext* ctx, const char* code, std::string_view message) {
    auto error = makeError(ctx, code, message);
    return JS_IsException(error) ? error : JS_Throw(ctx, error);
}

JSValue makeResult(JSContext* ctx, const http::Result& result) {
    const auto string = [ctx](const std::string& value) {
        return JS_NewStringLen(ctx, value.data(), value.size());
    };
    JsValue out(ctx, JS_NewObjectProto(ctx, JS_NULL));
    JsValue headers(ctx, JS_NewArray(ctx));
    if (JS_IsException(out.get()) || JS_IsException(headers.get())) return JS_EXCEPTION;
    std::uint32_t index = 0;
    for (const auto& [key, value] : result.headers) {
        JsValue pair(ctx, JS_NewArray(ctx));
        if (JS_IsException(pair.get()) ||
            JS_DefinePropertyValueUint32(ctx, pair.get(), 0, string(key), JS_PROP_C_W_E) < 0 ||
            JS_DefinePropertyValueUint32(ctx, pair.get(), 1, string(value), JS_PROP_C_W_E) < 0 ||
            JS_DefinePropertyValueUint32(ctx, headers.get(), index++, pair.release(), JS_PROP_C_W_E) < 0)
            return JS_EXCEPTION;
    }
    if (JS_SetPropertyStr(ctx, out.get(), "status", JS_NewInt32(ctx, result.status)) < 0 ||
        JS_SetPropertyStr(ctx, out.get(), "headers", headers.release()) < 0 ||
        JS_SetPropertyStr(ctx, out.get(), "body", JS_NewArrayBufferCopy(ctx,
            reinterpret_cast<const std::uint8_t*>(result.body.data()), result.body.size())) < 0 ||
        JS_SetPropertyStr(ctx, out.get(), "text", string(result.body)) < 0)
        return JS_EXCEPTION;
    return out.release();
}
} // namespace

struct HttpClient::Pending {
    std::shared_ptr<Work> work = std::make_shared<Work>();
    JsValue resolve, reject;
    Clock::time_point deadline;
    std::optional<std::string> reason;
    ~Pending() { work->cancelled.store(true); }
};

HttpClient::HttpClient(JsRuntime& runtime) : runtime_(runtime) {}
HttpClient::~HttpClient() = default;

bool HttpClient::install(ModuleLoader& loader, std::string& error) {
    auto* ctx = runtime_.context();
    const JsValue bridge(ctx, JS_NewObject(ctx));
    JS_SetPropertyStr(ctx, bridge.get(), "request", JS_NewCFunction(ctx, submit, "request", 2));
    JS_SetPropertyStr(ctx, bridge.get(), "cancel", JS_NewCFunction(ctx, cancel, "cancel", 1));
    return loader.installBuiltin("@minecraft/server-net", builtin::network, bridge.get(), error);
}

JSValue HttpClient::submit(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    auto* host = static_cast<HostContext*>(JS_GetContextOpaque(ctx));
    if (!host || !host->http) return JS_ThrowInternalError(ctx, "HTTP client unavailable");
    auto& client = *host->http;
    try {
        if (argc != 2 || !JS_IsString(argv[0])) throw StageError("EINVAL", "invalid HTTP request");
        const auto json = jsToString(ctx, argv[0]);
        if (json.size() > 64 * 1024) throw StageError("EMSGSIZE", "request metadata exceeds limit");
        const auto metadata = nlohmann::json::parse(json);
        http::Request request;
        request.url = metadata.at("url").get<std::string>();
        request.method = metadata.at("method").get<std::string>();
        const auto url = lowerCopy(request.url);
        const auto slash = url.find("://");
        if (!(url.starts_with("http://") || url.starts_with("https://")) || url.size() > 8192 ||
            url.find_first_of("\\\r\n\t ") != url.npos || url.find('\0') != url.npos ||
            slash == url.npos)
            throw StageError("EINVAL", "expected an HTTP(S) URL");
        const auto authority = std::string_view(url).substr(slash + 3);
        const auto name = authority.substr(0, authority.find_first_of("/?#"));
        if (name.empty() || name.find('@') != name.npos)
            throw StageError("EINVAL", "URL credentials are not supported");
        // Normalize only the scheme, preserving the resource path and query.
        request.url.replace(0, slash, url.substr(0, slash));
        if (!token(request.method) || request.method.size() > 32 ||
            request.method == "CONNECT" || request.method == "TRACE" || request.method == "TRACK")
            throw StageError("EINVAL", "unsupported HTTP method");
        const auto timeout = metadata.at("timeoutMs").get<double>();
        const auto maxBytes = metadata.at("maxResponseBytes").get<double>();
        if (!(timeout >= 1 && timeout <= kMaxTimeoutMs) ||
            !(maxBytes >= 1 && maxBytes <= kMaxBytes) || maxBytes != static_cast<std::size_t>(maxBytes))
            throw StageError("EINVAL", "timeout or response size is outside allowed limits");
        request.deadline = Clock::now() + std::chrono::milliseconds(static_cast<int>(timeout));
        request.maxBytes = static_cast<std::size_t>(maxBytes);
        for (const auto& pair : metadata.at("headers")) {
            auto key = pair.at(0).get<std::string>();
            auto value = pair.at(1).get<std::string>();
            if (!token(key) || forbiddenHeader(lowerCopy(key)) || key.size() > 256 ||
                value.size() > 8192 || request.headers.size() >= 128)
                throw StageError("EINVAL", "invalid or unsupported request header");
            for (const unsigned char ch : value)
                if ((ch < 32 && ch != '\t') || ch == 127)
                    throw StageError("EINVAL", "invalid control character in request header");
            request.headers.emplace_back(std::move(key), std::move(value));
        }
        if (JS_IsString(argv[1])) {
            auto text = jsToString(ctx, argv[1]);
            if (JS_HasException(ctx)) return JS_EXCEPTION;
            request.body = std::move(text);
        } else if (!JS_IsUndefined(argv[1])) {
            size_t size = 0;
            const auto* bytes = JS_IsArrayBuffer(argv[1])
                ? JS_GetArrayBuffer(ctx, &size, argv[1]) : JS_GetUint8Array(ctx, &size, argv[1]);
            if (JS_HasException(ctx)) return JS_EXCEPTION;
            if (size > kMaxBytes) throw StageError("EMSGSIZE", "request body exceeds limit");
            if (size && bytes) request.body.assign(reinterpret_cast<const char*>(bytes), size);
        }
        if (request.body.size() > kMaxBytes)
            throw StageError("EMSGSIZE", "request body exceeds limit");
        if (!request.body.empty() && (request.method == "GET" || request.method == "HEAD"))
            throw StageError("EINVAL", "GET and HEAD cannot carry a body");
        if (client.pending_.size() >= kMaxPending)
            throw StageError("EBUSY", "too many pending requests for this mod");
        auto pending = std::make_unique<Pending>();
        auto work = pending->work;
        pending->deadline = request.deadline;
        JSValue functions[2];
        JsValue promise(ctx, JS_NewPromiseCapability(ctx, functions));
        if (JS_IsException(promise.get())) return JS_EXCEPTION;
        pending->resolve = JsValue(ctx, functions[0]);
        pending->reject = JsValue(ctx, functions[1]);
        client.pending_.push_back(std::move(pending));
        if (workers.fetch_add(1) >= kMaxWorkers) {
            workers.fetch_sub(1);
            client.pending_.pop_back();
            throw StageError("EBUSY", "all HTTP workers are busy");
        }
        try {
            // The game DLL is pinned at startup. Workers never capture a host,
            // JS context, or callback, so mod reload cannot wait for I/O.
            std::thread([work, request = std::move(request)] {
                try {
                    work->result = http::transfer(request, work->cancelled);
                } catch (...) {
                    work->result.code = "EHTTP";
                }
                work->finished = Clock::now();
                workers.fetch_sub(1);
                work->done.store(true, std::memory_order_release);
            }).detach();
        } catch (...) {
            workers.fetch_sub(1);
            client.pending_.pop_back();
            throw StageError("EHTTP", "could not start HTTP worker");
        }
        return promise.release();
    } catch (const StageError& e) {
        return throwError(ctx, e.code.c_str(), e.what());
    } catch (const nlohmann::json::exception&) {
        return throwError(ctx, "EINVAL", "invalid HTTP configuration");
    } catch (...) {
        return JS_ThrowOutOfMemory(ctx);
    }
}

JSValue HttpClient::cancel(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    auto* host = static_cast<HostContext*>(JS_GetContextOpaque(ctx));
    if (!host || !host->http) return JS_ThrowInternalError(ctx, "HTTP client unavailable");
    try {
        const auto reason = argc ? jsToString(ctx, argv[0]).substr(0, 1024) : "HTTP request cancelled";
        if (JS_HasException(ctx)) return JS_EXCEPTION;
        for (auto& pending : host->http->pending_) {
            pending->reason = reason;
            pending->work->cancelled.store(true);
        }
        return JS_UNDEFINED;
    } catch (...) {
        return JS_ThrowOutOfMemory(ctx);
    }
}

void HttpClient::advance() {
    auto* ctx = runtime_.context();
    for (auto it = pending_.begin(); it != pending_.end();) {
        auto& pending = **it;
        const bool done = pending.work->done.load(std::memory_order_acquire);
        const auto now = Clock::now();
        if (!done && !pending.reason && now < pending.deadline) { ++it; continue; }
        const bool timedOut = !done || pending.work->finished >= pending.deadline;
        const auto& result = pending.work->result;
        const char* code = pending.reason ? "ECANCELED" :
            timedOut ? "ETIMEDOUT" : result.code;
        const std::string_view message = pending.reason ? std::string_view(*pending.reason) :
            timedOut ? std::string_view("HTTP request deadline exceeded") :
            result.message.empty() ? std::string_view("HTTP request failed") :
            std::string_view(result.message);
        JSValue value = code ? makeError(ctx, code, message) : makeResult(ctx, result);
        bool reject = code != nullptr;
        if (JS_IsException(value)) {
            value = JS_GetException(ctx);
            reject = true;
        }
        const JsValue arg(ctx, value);
        JSValue ignored = JS_UNDEFINED;
        std::string error;
        runtime_.callFunction(reject ? pending.reject.get() : pending.resolve.get(),
                              JS_UNDEFINED, {arg.get()}, ignored, error);
        JS_FreeValue(ctx, ignored);
        it = pending_.erase(it);
    }
}

} // namespace stageb
