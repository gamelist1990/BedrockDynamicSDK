#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <winhttp.h>
#include <bcrypt.h>
#include "bedrock/generated_symbols.hpp"
#include "bedrock/remote_catalog.hpp"
#include "bedrock/resolver.hpp"
#include <nlohmann/json.hpp>
#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#pragma comment(lib, "winhttp.lib")
namespace bedrock {
namespace {
constexpr wchar_t kHost[] = L"raw.githubusercontent.com";
constexpr wchar_t kManifest[] =
    L"/gamelist1990/BedrockDynamicSDK/main/data/manifest.json";
constexpr std::size_t kManifestLimit = 256 * 1024;
constexpr std::size_t kProfileLimit = 4 * 1024 * 1024;
constexpr auto kCacheFreshness = std::chrono::minutes(30);

class HttpHandle {
public:
    explicit HttpHandle(HINTERNET h = nullptr) noexcept : h_(h) {}
    ~HttpHandle() { if (h_) WinHttpCloseHandle(h_); }
    HttpHandle(const HttpHandle&) = delete;
    HttpHandle& operator=(const HttpHandle&) = delete;
    operator HINTERNET() const noexcept { return h_; }
private:
    HINTERNET h_{};
};

bool httpsGet(const std::wstring& resource, std::size_t sizeLimit,
              std::string& result, std::string& error) {
    result.clear();
    HttpHandle session(WinHttpOpen(L"BedrockDynamicSDK/0.2",
        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME,
        WINHTTP_NO_PROXY_BYPASS, 0));
    if (!static_cast<HINTERNET>(session)) { error = "WinHttpOpen failed"; return false; }
    WinHttpSetTimeouts(session, 3500, 3500, 6000, 6000);
    HttpHandle connection(WinHttpConnect(session, kHost, INTERNET_DEFAULT_HTTPS_PORT, 0));
    if (!static_cast<HINTERNET>(connection)) { error = "WinHttpConnect failed"; return false; }
    HttpHandle request(WinHttpOpenRequest(connection, L"GET", resource.c_str(),
        nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
        WINHTTP_FLAG_SECURE));
    if (!static_cast<HINTERNET>(request)) { error = "WinHttpOpenRequest failed"; return false; }
    // Prevent an accidental redirect to any other host.
    DWORD disable = WINHTTP_DISABLE_REDIRECTS;
    if (!WinHttpSetOption(request, WINHTTP_OPTION_DISABLE_FEATURE,
                          &disable, sizeof(disable))) {
        error = "cannot enforce disabled redirects"; return false;
    }
    if (!WinHttpSendRequest(request,WINHTTP_NO_ADDITIONAL_HEADERS,0,
                            WINHTTP_NO_REQUEST_DATA,0,0,0) ||
        !WinHttpReceiveResponse(request, nullptr)) {
        error = "HTTPS fetch failed"; return false;
    }
    DWORD code = 0, codeSize = sizeof(code);
    if (!WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE |
                            WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                            &code, &codeSize, WINHTTP_NO_HEADER_INDEX) || code != 200) {
        error = "unexpected HTTP status"; return false;
    }
    std::array<char, 16384> buffer{};
    for (;;) {
        DWORD n = 0;
        if (!WinHttpReadData(request,buffer.data(),
                             static_cast<DWORD>(buffer.size()),&n)) {
            error = "HTTPS body read failed"; return false;
        }
        if (!n) break;
        if (result.size() + n > sizeLimit) {
            error = "remote JSON exceeds byte limit"; return false;
        }
        result.append(buffer.data(),n);
    }
    return true;
}
bool sha256(std::string_view text, std::string& out) {
    BCRYPT_ALG_HANDLE alg{};
    BCRYPT_HASH_HANDLE h{};
    std::array<unsigned char,32> digest{};
    bool success = false;
    if (BCryptOpenAlgorithmProvider(&alg,BCRYPT_SHA256_ALGORITHM,nullptr,0) < 0)
        return false;
    if (BCryptCreateHash(alg,&h,nullptr,0,nullptr,0,0) < 0) goto end;
    if (text.size() > 0xffffffffULL) goto end;
    if (BCryptHashData(h,reinterpret_cast<PUCHAR>(const_cast<char*>(text.data())),
        static_cast<ULONG>(text.size()),0) < 0) goto end;
    if (BCryptFinishHash(h,digest.data(),static_cast<ULONG>(digest.size()),0) < 0)
        goto end;
    {
        std::ostringstream stream;
        for (auto b:digest) stream << std::hex << std::setfill('0') << std::setw(2) << +b;
        out = stream.str();
    }
    success = true;
end:
    if (h) BCryptDestroyHash(h);
    BCryptCloseAlgorithmProvider(alg,0);
    return success;
}

std::filesystem::path cachePath(const GameIdentity& identity) {
    std::wstring localAppData(32768, L'\0');
    const DWORD length = GetEnvironmentVariableW(
        L"LOCALAPPDATA", localAppData.data(), static_cast<DWORD>(localAppData.size()));
    if (!length || length >= localAppData.size()) return {};
    localAppData.resize(length);
    const std::string& key = identity.textSha256.size() == 64
        ? identity.textSha256 : identity.fileSha256;
    if (key.size() != 64) return {};
    return std::filesystem::path(localAppData) / L"PEXData" /
        L"BedrockDynamicSDK" / L"profile-cache" /
        (std::wstring(key.begin(), key.end()) + L".json");
}

bool readCache(const std::filesystem::path& path, std::string& contents,
               bool& fresh) {
    contents.clear();
    fresh = false;
    if (path.empty()) return false;
    std::error_code ec;
    const auto size = std::filesystem::file_size(path, ec);
    if (ec || size == 0 || size > kProfileLimit) return false;
    std::ifstream stream(path, std::ios::binary);
    if (!stream) return false;
    contents.assign(std::istreambuf_iterator<char>(stream), {});
    if (contents.size() != size) return false;
    const auto written = std::filesystem::last_write_time(path, ec);
    if (!ec) fresh = std::filesystem::file_time_type::clock::now() - written <=
                     kCacheFreshness;
    return true;
}

void writeCache(const std::filesystem::path& path, std::string_view contents) {
    if (path.empty()) return;
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    if (ec) return;
    auto temporary = path;
    temporary += L"." + std::to_wstring(GetCurrentProcessId()) + L".tmp";
    {
        std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
        if (!stream) return;
        stream.write(contents.data(), static_cast<std::streamsize>(contents.size()));
        if (!stream) return;
    }
    if (!MoveFileExW(temporary.c_str(), path.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        DeleteFileW(temporary.c_str());
}

bool loadCandidate(std::string_view candidate, const GameIdentity& identity,
                   ProfileSource source, std::string& profileJson,
                   ProfileChoice& choice, std::string& error) {
    try {
        const auto document = nlohmann::json::parse(candidate.begin(), candidate.end());
        const auto version = document.at("target").at("version").get<std::string>();
        ProfileChoice selected{version, std::string(profileSourceName(source)), {}, source};
        if (!validateDownloadedProfile(candidate, selected, identity, error)) return false;
        Resolver resolver;
        if (!resolver.loadJson(candidate, error) ||
            !resolver.attachWindowsMainModule(error) || !resolver.exactBuild()) {
            error = "profile did not resolve against the loaded game";
            return false;
        }
        choice = std::move(selected);
        profileJson.assign(candidate);
        error.clear();
        return true;
    } catch (const std::exception& exception) {
        error = std::string("invalid local profile: ") + exception.what();
        return false;
    }
}
} // namespace

bool fetchSupportedProfile(std::string& profileJson,
                           ProfileChoice& choice, std::string& error) {
    profileJson.clear();
    GameIdentity identity;
    if (!inspectLoadedGame(identity,error))
        return false;
    const auto cachedPath = cachePath(identity);
    std::string cached;
    bool cacheFresh = false;
    const bool hasCache = readCache(cachedPath, cached, cacheFresh);
    std::string cacheError;
    if (hasCache && cacheFresh && loadCandidate(cached, identity,
            ProfileSource::Cache, profileJson, choice, cacheError))
        return true;

    std::string manifest;
    std::string remoteError;
    if (httpsGet(kManifest,kManifestLimit,manifest,remoteError)) {
        auto selected = chooseProfile(manifest,identity,remoteError);
        if (selected) {
            // The choice.path passed strict version and filename validation.
            std::wstring path = L"/gamelist1990/BedrockDynamicSDK/main/";
            for (char c:selected->path) path.push_back(static_cast<wchar_t>(c));
            std::string downloaded;
            if (httpsGet(path,kProfileLimit,downloaded,remoteError)) {
                std::string downloadedHash;
                if (!sha256(downloaded,downloadedHash) ||
                    downloadedHash != selected->contentSha256) {
                    remoteError = "downloaded version data SHA-256 does not match manifest";
                } else if (validateDownloadedProfile(downloaded,*selected,identity,remoteError)) {
                    Resolver resolver;
                    if (resolver.loadJson(downloaded,remoteError) &&
                        resolver.attachWindowsMainModule(remoteError) && resolver.exactBuild()) {
                        selected->source = ProfileSource::Remote;
                        choice = *selected;
                        profileJson = std::move(downloaded);
                        writeCache(cachedPath, profileJson);
                        error.clear();
                        return true;
                    }
                    remoteError = "profile did not resolve against the loaded game";
                }
            }
        }
    }

    // A stale cache remains a safe exact-build fallback when GitHub is down.
    if (hasCache && loadCandidate(cached, identity, ProfileSource::Cache,
                                  profileJson, choice, cacheError)) {
        error.clear();
        return true;
    }

    // The generated database makes the SDK's build version work on first run,
    // even when raw.githubusercontent.com is blocked or rate-limited.
    if (loadCandidate(generated::kSymbolDatabase, identity, ProfileSource::Embedded,
                      profileJson, choice, cacheError)) {
        writeCache(cachedPath, profileJson);
        error.clear();
        return true;
    }
    error = remoteError.empty() ? cacheError : remoteError;
    return false;
}
} // namespace bedrock
#endif
