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
#include <thread>
#include <vector>

#pragma comment(lib, "winhttp.lib")
namespace bedrock {
namespace {
constexpr wchar_t kHost[] = L"raw.githubusercontent.com";
constexpr wchar_t kManifest[] =
    L"/gamelist1990/BedrockDynamicSDK/main/data/manifest.json";
constexpr std::size_t kManifestLimit = 256 * 1024;
constexpr std::size_t kProfileLimit = 4 * 1024 * 1024;
constexpr auto kCacheFreshness = std::chrono::hours(24);
constexpr auto kRefreshInterval = std::chrono::hours(24);

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
    temporary += L"." + std::to_wstring(GetCurrentProcessId()) + L"." +
        std::to_wstring(GetCurrentThreadId()) + L".tmp";
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
        choice = std::move(selected);
        profileJson.assign(candidate);
        error.clear();
        return true;
    } catch (const std::exception& exception) {
        error = std::string("invalid local profile: ") + exception.what();
        return false;
    }
}

bool fetchRemoteProfile(const GameIdentity& identity,
                        const std::filesystem::path& cache,
                        std::string& profileJson, ProfileChoice& choice,
                        std::string& error) {
    std::string manifest;
    if (!httpsGet(kManifest, kManifestLimit, manifest, error)) return false;
    auto selected = chooseProfile(manifest, identity, error);
    if (!selected) return false;
    std::wstring resource = L"/gamelist1990/BedrockDynamicSDK/main/";
    for (char c : selected->path) resource.push_back(static_cast<wchar_t>(c));
    std::string downloaded;
    if (!httpsGet(resource, kProfileLimit, downloaded, error)) return false;
    std::string downloadedHash;
    if (!sha256(downloaded, downloadedHash) ||
        downloadedHash != selected->contentSha256) {
        error = "profile checksum mismatch";
        return false;
    }
    if (!validateDownloadedProfile(downloaded, *selected, identity, error)) return false;
    Resolver resolver;
    if (!resolver.loadJson(downloaded, error) ||
        !resolver.attachWindowsMainModule(error) || !resolver.exactBuild()) {
        error = "profile did not resolve against the loaded game";
        return false;
    }
    selected->source = ProfileSource::Remote;
    writeCache(cache, downloaded);
    profileJson = std::move(downloaded);
    choice = std::move(*selected);
    error.clear();
    return true;
}

void refreshRemoteProfile(GameIdentity identity, std::filesystem::path cache) {
    try {
        std::string ignoredProfile;
        ProfileChoice ignoredChoice;
        std::string ignoredError;
        fetchRemoteProfile(identity, cache, ignoredProfile, ignoredChoice, ignoredError);
    } catch (...) {
        // A background refresh must never affect the active client.
    }
}

bool markRefreshAttempt(const std::filesystem::path& cache) noexcept {
    if (cache.empty()) return true;
    std::wstring key = cache.stem().wstring();
    if (key.empty()) return true;
    std::wstring mutexName = L"Local\\BedrockDynamicSDKProfileRefresh_" + key;
    HANDLE mutex = CreateMutexW(nullptr, FALSE, mutexName.c_str());
    if (!mutex) return true;
    const DWORD wait = WaitForSingleObject(mutex, 0);
    if (wait != WAIT_OBJECT_0 && wait != WAIT_ABANDONED) {
        CloseHandle(mutex);
        return false;
    }

    auto stamp = cache;
    stamp += L".refresh";
    std::error_code ec;
    const auto attempted = std::filesystem::last_write_time(stamp, ec);
    if (!ec && std::filesystem::file_time_type::clock::now() - attempted <
                   kRefreshInterval) {
        ReleaseMutex(mutex);
        CloseHandle(mutex);
        return false;
    }
    // Persist the attempt before launching the thread. This throttles failures
    // too, so offline startup cannot retry GitHub on every DLL load.
    writeCache(stamp, "refresh-attempt");
    const auto marked = std::filesystem::last_write_time(stamp, ec);
    if (ec || std::filesystem::file_time_type::clock::now() - marked >=
                  kRefreshInterval) {
        ReleaseMutex(mutex);
        CloseHandle(mutex);
        return static_cast<bool>(ec); // Permit one attempt if storage is unavailable.
    }
    ReleaseMutex(mutex);
    CloseHandle(mutex);
    return true;
}

bool fetchRemoteProfileSingleFlight(const GameIdentity& identity,
                                    const std::filesystem::path& cache,
                                    std::string& profileJson,
                                    ProfileChoice& choice,
                                    std::string& error) {
    if (cache.empty())
        return fetchRemoteProfile(identity, cache, profileJson, choice, error);
    std::wstring key = cache.stem().wstring();
    std::wstring mutexName = L"Local\\BedrockDynamicSDKProfileRefresh_" + key;
    HANDLE mutex = CreateMutexW(nullptr, FALSE, mutexName.c_str());
    if (!mutex) return fetchRemoteProfile(identity, cache, profileJson, choice, error);
    const DWORD wait = WaitForSingleObject(mutex, INFINITE);
    if (wait != WAIT_OBJECT_0 && wait != WAIT_ABANDONED) {
        CloseHandle(mutex);
        error = "profile refresh lock unavailable";
        return false;
    }
    const auto release = [&] {
        ReleaseMutex(mutex);
        CloseHandle(mutex);
    };

    std::string cached;
    bool ignoredFreshness = false;
    if (readCache(cache, cached, ignoredFreshness) &&
        loadCandidate(cached, identity, ProfileSource::Cache,
                      profileJson, choice, error)) {
        release();
        return true;
    }
    auto stamp = cache;
    stamp += L".refresh";
    std::error_code ec;
    const auto attempted = std::filesystem::last_write_time(stamp, ec);
    if (!ec && std::filesystem::file_time_type::clock::now() - attempted <
                   kRefreshInterval) {
        release();
        error = "profile refresh is limited to once per 24 hours; no matching cached profile";
        return false;
    }
    writeCache(stamp, "refresh-attempt");
    // Keep the mutex while fetching so other DLLs wait for the one shared
    // download and then consume its cache instead of issuing duplicate GETs.
    const bool fetched = fetchRemoteProfile(identity, cache, profileJson, choice, error);
    release();
    return fetched;
}

void startRemoteRefresh(const GameIdentity& identity,
                        const std::filesystem::path& cache) noexcept {
    if (!markRefreshAttempt(cache)) return;
    try {
        std::thread(refreshRemoteProfile, identity, cache).detach();
    } catch (...) {
        // The durable attempt marker prevents repeated launch retries.
    }
}
} // namespace

bool fetchSupportedProfile(std::string& profileJson,
                           ProfileChoice& choice, std::string& error,
                           ProfileProgressCallback progress) {
    profileJson.clear();
    if (progress) progress("inspect game");
    GameIdentity identity;
    if (!inspectLoadedGame(identity,error))
        return false;
    if (progress) progress("check cache");
    const auto cachedPath = cachePath(identity);
    std::string cached;
    bool cacheFresh = false;
    const bool hasCache = readCache(cachedPath, cached, cacheFresh);
    std::string cacheError;
    if (hasCache && loadCandidate(cached, identity,
            ProfileSource::Cache, profileJson, choice, cacheError)) {
        if (progress) progress(cacheFresh ? "cache ready" : "stale cache ready");
        if (!cacheFresh) startRemoteRefresh(identity, cachedPath);
        return true;
    }

    // Prefer a bundled exact-build profile for immediate startup. The network
    // refresh updates the cache for the next launch without blocking this one.
    if (loadCandidate(generated::kSymbolDatabase, identity, ProfileSource::Embedded,
                      profileJson, choice, cacheError)) {
        writeCache(cachedPath, profileJson);
        if (progress) progress("embedded profile ready");
        startRemoteRefresh(identity, cachedPath);
        error.clear();
        return true;
    }

    // A newer game build has no usable local profile, so GitHub is required.
    if (progress) progress("GitHub profile required");
    return fetchRemoteProfileSingleFlight(identity, cachedPath, profileJson,
                                          choice, error);
}
} // namespace bedrock
#endif
