#include "http_transport.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <memory>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <winhttp.h>
#elif defined(SCRIPTCLIENT_HAS_CURL)
#include <curl/curl.h>
#endif

namespace stageb::http {

void parseHeader(std::string_view line, Headers& headers) {
    const auto colon = line.find(':');
    if (colon == std::string_view::npos || colon == 0) return;
    auto value = line.substr(colon + 1);
    while (!value.empty() && (value.front() == ' ' || value.front() == '\t'))
        value.remove_prefix(1);
    while (!value.empty() && (value.back() == ' ' || value.back() == '\t' ||
                              value.back() == '\r' || value.back() == '\n'))
        value.remove_suffix(1);
    headers.emplace_back(line.substr(0, colon), value);
}

#ifdef _WIN32
namespace {
struct CloseHandle { void operator()(void* h) const { WinHttpCloseHandle(h); } };
using Handle = std::unique_ptr<void, CloseHandle>;

std::wstring wide(std::string_view value) {
    if (value.empty()) return {};
    const int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                                          static_cast<int>(value.size()), nullptr, 0);
    if (!length) throw StageError("EINVAL", "invalid UTF-8 in request");
    std::wstring out(length, L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                        static_cast<int>(value.size()), out.data(), length);
    return out;
}

std::string utf8(std::wstring_view value) {
    if (value.empty()) return {};
    const auto size = static_cast<int>(value.size());
    const int length = WideCharToMultiByte(CP_UTF8, 0, value.data(), size, nullptr, 0, nullptr, nullptr);
    std::string out(length, '\0');
    if (length)
        WideCharToMultiByte(CP_UTF8, 0, value.data(), size, out.data(), length, nullptr, nullptr);
    return out;
}

Result winError(const char* operation) {
    const auto code = GetLastError();
    const char* name = code == ERROR_WINHTTP_TIMEOUT ? "ETIMEDOUT" :
                       code == ERROR_WINHTTP_NAME_NOT_RESOLVED ? "ENOTFOUND" :
                       code == ERROR_WINHTTP_CANNOT_CONNECT ? "ECONNREFUSED" :
                       code == ERROR_WINHTTP_SECURE_FAILURE ? "ETLS" :
                       code == ERROR_WINHTTP_HEADER_SIZE_OVERFLOW ? "EMSGSIZE" : "EHTTP";
    return failure(name, std::string(operation) + " failed (WinHTTP " + std::to_string(code) + ")");
}
} // namespace

Result transfer(const Request& input, const std::atomic<bool>& cancelled) {
    const auto stopped = [&] { return cancelled.load() || Clock::now() >= input.deadline; };
    if (stopped()) return failure("ETIMEDOUT", "request expired before transfer");
    auto url = wide(input.url);
    URL_COMPONENTS parts{};
    parts.dwStructSize = sizeof(parts);
    parts.dwHostNameLength = parts.dwUrlPathLength = parts.dwExtraInfoLength =
        parts.dwUserNameLength = parts.dwPasswordLength = static_cast<DWORD>(-1);
    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &parts) || parts.dwHostNameLength == 0 ||
        parts.dwUserNameLength || parts.dwPasswordLength ||
        (parts.nScheme != INTERNET_SCHEME_HTTP && parts.nScheme != INTERNET_SCHEME_HTTPS))
        return failure("EINVAL", "expected an HTTP(S) URL without user information");
    const std::wstring host(parts.lpszHostName, parts.dwHostNameLength);
    std::wstring resource = parts.dwUrlPathLength
        ? std::wstring(parts.lpszUrlPath, parts.dwUrlPathLength) : L"/";
    if (parts.dwExtraInfoLength) resource.append(parts.lpszExtraInfo, parts.dwExtraInfoLength);
    if (const auto hash = resource.find(L'#'); hash != std::wstring::npos) resource.resize(hash);
    // WinHTTP is also used by the SDK profile downloader. No TLS bypasses.
    Handle session(WinHttpOpen(L"ScriptClient/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                               WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
    if (!session) return winError("WinHttpOpen");
    const auto ms = std::max(1, static_cast<int>(
        std::chrono::duration_cast<std::chrono::milliseconds>(input.deadline - Clock::now()).count()));
    if (!WinHttpSetTimeouts(session.get(), ms, ms, ms, ms)) return winError("WinHttpSetTimeouts");
    Handle connection(WinHttpConnect(session.get(), host.c_str(), parts.nPort, 0));
    if (!connection) return winError("WinHttpConnect");
    const auto method = wide(input.method);
    Handle request(WinHttpOpenRequest(connection.get(), method.c_str(), resource.c_str(),
                nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                parts.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0));
    if (!request) return winError("WinHttpOpenRequest");
    DWORD disabled = WINHTTP_DISABLE_COOKIES | WINHTTP_DISABLE_AUTHENTICATION;
    if (!WinHttpSetOption(request.get(), WINHTTP_OPTION_DISABLE_FEATURE, &disabled, sizeof(disabled)))
        return winError("setting WinHTTP features");
    // Never forward mod-supplied headers to a redirected origin.
    DWORD redirects = input.headers.empty() ? WINHTTP_OPTION_REDIRECT_POLICY_DISALLOW_HTTPS_TO_HTTP
                                            : WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
    DWORD limit = 5, headerLimit = 64 * 1024;
    if (!WinHttpSetOption(request.get(), WINHTTP_OPTION_REDIRECT_POLICY, &redirects, sizeof(redirects)) ||
        !WinHttpSetOption(request.get(), WINHTTP_OPTION_MAX_HTTP_AUTOMATIC_REDIRECTS, &limit, sizeof(limit)) ||
        !WinHttpSetOption(request.get(), WINHTTP_OPTION_MAX_RESPONSE_HEADER_SIZE, &headerLimit, sizeof(headerLimit)))
        return winError("setting WinHTTP limits");
    std::wstring headers;
    for (const auto& [name, value] : input.headers)
        headers += wide(name) + L": " + wide(value) + L"\r\n";
    if (stopped()) return failure("ETIMEDOUT", "request expired");
    const auto bytes = static_cast<DWORD>(input.body.size());
    if (!WinHttpSendRequest(request.get(), headers.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : headers.c_str(),
                            static_cast<DWORD>(headers.size()),
                            bytes ? const_cast<char*>(input.body.data()) : WINHTTP_NO_REQUEST_DATA,
                            bytes, bytes, 0))
        return winError("WinHttpSendRequest");
    if (stopped()) return failure("ETIMEDOUT", "request expired");
    if (!WinHttpReceiveResponse(request.get(), nullptr)) return winError("WinHttpReceiveResponse");

    Result out;
    DWORD status = 0, size = sizeof(status);
    if (!WinHttpQueryHeaders(request.get(), WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                             WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX))
        return winError("querying status");
    out.status = static_cast<int>(status);
    size = 0;
    WinHttpQueryHeaders(request.get(), WINHTTP_QUERY_RAW_HEADERS_CRLF, WINHTTP_HEADER_NAME_BY_INDEX,
                        nullptr, &size, WINHTTP_NO_HEADER_INDEX);
    if (GetLastError() != ERROR_INSUFFICIENT_BUFFER) return winError("querying headers");
    if (size > headerLimit * sizeof(wchar_t))
        return failure("EMSGSIZE", "response headers exceed limit");
    std::wstring raw(size / sizeof(wchar_t), L'\0');
    if (!WinHttpQueryHeaders(request.get(), WINHTTP_QUERY_RAW_HEADERS_CRLF, WINHTTP_HEADER_NAME_BY_INDEX,
                             raw.data(), &size, WINHTTP_NO_HEADER_INDEX))
        return winError("reading headers");
    const auto text = utf8(std::wstring_view(raw.c_str()));
    auto lines = std::string_view(text);
    if (const auto end = lines.find("\r\n"); end != lines.npos) lines.remove_prefix(end + 2);
    while (!lines.empty()) {
        const auto end = lines.find("\r\n");
        parseHeader(lines.substr(0, end), out.headers);
        if (end == lines.npos) break;
        lines.remove_prefix(end + 2);
    }
    std::array<char, 8192> buffer;
    while (!stopped()) {
        DWORD got = 0;
        if (!WinHttpReadData(request.get(), buffer.data(), static_cast<DWORD>(buffer.size()), &got))
            return winError("reading HTTP response");
        if (!got) return out;
        if (got > input.maxBytes - out.body.size())
            return failure("EMSGSIZE", "response body exceeds limit");
        out.body.append(buffer.data(), got);
    }
    return failure("ETIMEDOUT", "request expired");
}

#elif defined(SCRIPTCLIENT_HAS_CURL)
namespace {
struct Transfer {
    const Request& input;
    const std::atomic<bool>& cancelled;
    Result result;
    std::size_t headerBytes = 0;
    bool overflow = false;
    bool ignoreBody = false;
};

std::size_t onHeader(char* text, std::size_t size, std::size_t count, void* opaque) noexcept {
    auto& t = *static_cast<Transfer*>(opaque);
    const std::string_view line(text, size * count);
    if ((t.headerBytes += line.size()) > 64 * 1024) { t.overflow = true; return 0; }
    try {
        if (line.starts_with("HTTP/")) {
            t.result.headers.clear();
            t.result.body.clear();
            const auto space = line.find(' ');
            int status = 0;
            if (space != line.npos)
                std::from_chars(line.data() + space + 1, line.data() + line.size(), status);
            t.ignoreBody = t.input.headers.empty() &&
                (status == 301 || status == 302 || status == 303 || status == 307 || status == 308);
        } else {
            parseHeader(line, t.result.headers);
            // A redirect code without Location is a normal final response.
            if ((line == "\r\n" || line == "\n") && t.ignoreBody &&
                std::none_of(t.result.headers.begin(), t.result.headers.end(), [](const auto& h) {
                    return lowerCopy(h.first) == "location";
                })) t.ignoreBody = false;
        }
        return line.size();
    } catch (...) { return 0; }
}

std::size_t onBody(char* data, std::size_t size, std::size_t count, void* opaque) noexcept {
    auto& t = *static_cast<Transfer*>(opaque);
    const auto got = size * count;
    if (t.ignoreBody) return got;
    if (got > t.input.maxBytes - t.result.body.size()) { t.overflow = true; return 0; }
    try { t.result.body.append(data, got); return got; } catch (...) { return 0; }
}

int progress(void* opaque, curl_off_t, curl_off_t, curl_off_t, curl_off_t) {
    return static_cast<Transfer*>(opaque)->cancelled.load() ? 1 : 0;
}
} // namespace

Result transfer(const Request& input, const std::atomic<bool>& cancelled) {
    static const auto init = curl_global_init(CURL_GLOBAL_DEFAULT);
    if (init != CURLE_OK) return failure("EHTTP", "initializing libcurl failed");
    std::unique_ptr<CURL, decltype(&curl_easy_cleanup)> handle(curl_easy_init(), curl_easy_cleanup);
    if (!handle) return failure("EHTTP", "creating libcurl request failed");
    auto* curl = handle.get();
    Transfer t{input, cancelled, {}};
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(input.deadline - Clock::now()).count();
    if (ms <= 0 || cancelled.load()) return failure("ETIMEDOUT", "request expired before transfer");
    std::unique_ptr<curl_slist, decltype(&curl_slist_free_all)> headers(nullptr, curl_slist_free_all);
    for (const auto& [key, value] : input.headers) {
        auto* head = curl_slist_append(headers.get(), (key + (value.empty() ? ";" : ": " + value)).c_str());
        if (!head) return failure("EHTTP", "building libcurl headers failed");
        headers.release();
        headers.reset(head);
    }
    if (curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "http,https") != CURLE_OK ||
        curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS_STR,
                         input.url.starts_with("https:") ? "https" : "http,https") != CURLE_OK ||
        curl_easy_setopt(curl, CURLOPT_DISALLOW_USERNAME_IN_URL, 1L) != CURLE_OK)
        return failure("EHTTP", "configuring HTTP protocols failed");
    curl_easy_setopt(curl, CURLOPT_URL, input.url.c_str());
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, static_cast<long>(ms));
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, input.headers.empty() ? 1L : 0L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 5L);
    curl_easy_setopt(curl, CURLOPT_SUPPRESS_CONNECT_HEADERS, 1L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "ScriptClient/1.0");
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, onHeader);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, &t);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, onBody);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &t);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, progress);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &t);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers.get());
    if (input.method == "HEAD") {
        curl_easy_setopt(curl, CURLOPT_NOBODY, 1L);
    } else if (input.method != "GET") {
        if (!input.body.empty() || input.method == "POST" || input.method == "PUT") {
            curl_easy_setopt(curl, CURLOPT_POSTFIELDS, input.body.data());
            curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(input.body.size()));
        }
        if (input.method != "POST") curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, input.method.c_str());
    }
    const auto rc = curl_easy_perform(curl);
    if (t.overflow) return failure("EMSGSIZE", "response exceeds limit");
    if (rc != CURLE_OK) {
        const char* code = rc == CURLE_OPERATION_TIMEDOUT ? "ETIMEDOUT" :
                           rc == CURLE_COULDNT_RESOLVE_HOST ? "ENOTFOUND" :
                           rc == CURLE_COULDNT_CONNECT ? "ECONNREFUSED" :
                           rc == CURLE_PEER_FAILED_VERIFICATION ? "ETLS" : "EHTTP";
        return failure(code, curl_easy_strerror(rc));
    }
    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    t.result.status = static_cast<int>(status);
    return std::move(t.result);
}

#else
Result transfer(const Request&, const std::atomic<bool>&) {
    return failure("Unavailable", "HTTP on this host build requires libcurl development files");
}
#endif
} // namespace stageb::http
