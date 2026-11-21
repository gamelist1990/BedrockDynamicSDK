#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <bcrypt.h>
#include <psapi.h>
#include <shlobj.h>

#include <bedrock/detail/mod_settings_oreui.hpp>
#include <bedrock/detail/native_profile_functions.hpp>
#include <bedrock/generated/mod_settings_js.hpp>
#include <bedrock/generated/symbols.hpp>
#include <bedrock/profiles/resolver.hpp>
#include <bedrock/runtime/hook_runtime.h>

#include <array>
#include <atomic>
#include <cstdio>
#include <fstream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <vector>

namespace bedrock::detail::oreui_settings {
namespace {
using namespace impl;

using Handler = unsigned (__fastcall *)(void*, const void*, void*);
using Allocate = void* (__fastcall *)(void*, std::size_t);
using SetHeader = void (__fastcall *)(void*, const char*, const char*);
using Advance = unsigned (__fastcall *)(void*, double);
using ExecuteScript = void (__fastcall *)(void*, const char*);
using URLString = const void* (__fastcall *)(const void*);

std::filesystem::path settingsPath();

void logDiagnostic(std::string_view message) noexcept {
    try {
        const auto line = "[BedrockModSettings] " + std::string(message) + "\n";
        OutputDebugStringA(line.c_str());
        static std::mutex diagnostic_mutex;
        std::lock_guard lock(diagnostic_mutex);
        const auto root = settingsPath();
        std::filesystem::create_directories(root);
        std::ofstream(root / "oreui.log", std::ios::app) << GetCurrentProcessId() << ' ' << line;
    } catch (...) {}
}

struct Abi {
    unsigned url, size, capacity, inline_bytes, range;
    unsigned implementation, allocate, set_header, handled;
};

struct ViewAbi {
    const void* vtable{};
    const std::byte* module{};
    std::uint32_t image_size{};
    ExecuteScript execute_script{};
    URLString serialized_url{};
    unsigned current_url{}, inline_tag{}, inline_offset{}, heap_offset{};
};

struct Runtime {
    Runtime(std::filesystem::path root, Abi layout)
        : store(std::move(root)), abi(layout) {}
    Store store;
    Abi abi;
    ViewAbi view;
    std::string prefix;
    std::string proof_script, adapter_script;
    std::string document_path, bootstrap_document;
    std::array<void*, 3> originals{};
    std::array<BDS_HookToken, 3> tokens{};
    std::atomic<bool> active{}, logged_advance{}, dumped_view{}, logged_endpoint{}, proof_received{}, adapter_received{};
    std::array<std::atomic<bool>, 2> logged_resource{};
};
Runtime* runtime{}; // Intentionally resident for the process lifetime.

constexpr std::string_view route = "/hbui/";
constexpr std::string_view endpoint_prefix = "__bedrock_mod_settings/";

template<std::size_t N> std::string hex(const std::array<unsigned char, N>& bytes) {
    constexpr char digits[] = "0123456789abcdef";
    std::string result;
    result.reserve(N * 2);
    for (const auto byte : bytes) {
        result += digits[byte >> 4];
        result += digits[byte & 15];
    }
    return result;
}

std::string digest(std::string_view bytes) {
    BCRYPT_ALG_HANDLE alg{};
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0) return {};
    std::array<unsigned char, 32> hash{};
    const auto status = BCryptHash(alg, nullptr, 0,
        reinterpret_cast<PUCHAR>(const_cast<char*>(bytes.data())),
        static_cast<ULONG>(bytes.size()), hash.data(), static_cast<ULONG>(hash.size()));
    BCryptCloseAlgorithmProvider(alg, 0);
    return status < 0 ? std::string{} : hex(hash);
}

std::filesystem::path settingsPath() {
    PWSTR raw{};
    if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &raw)))
        throw std::runtime_error("cannot locate LocalAppData");
    const std::unique_ptr<wchar_t, decltype(&CoTaskMemFree)> path(raw, &CoTaskMemFree);
    return std::filesystem::path(path.get()) / L"BedrockDynamicSDK" / L"ModSettings";
}

std::string loadAsset(const std::filesystem::path& file, std::size_t limit) {
    std::ifstream stream(file, std::ios::binary | std::ios::ate);
    const auto size = stream.tellg();
    if (!stream || size <= 0 || static_cast<std::size_t>(size) > limit)
        throw std::runtime_error("OreUI asset missing or oversized");
    std::string bytes(static_cast<std::size_t>(size), '\0');
    stream.seekg(0);
    if (!stream.read(bytes.data(), static_cast<std::streamsize>(bytes.size())))
        throw std::runtime_error("cannot read OreUI asset");
    return bytes;
}

void prepareBootstrap(Runtime& state, const nlohmann::json& bundle) {
    // Install in newly loaded documents before the deferred game bundle can
    // subscribe. Do not replace an unknown/custom shell or the game bundle.
    std::array<wchar_t, 32768> executable{};
    const auto length = GetModuleFileNameW(nullptr, executable.data(), DWORD(executable.size()));
    if (!length || length >= executable.size()) return;
    const auto document = bundle.at("document").get<std::string>();
    auto html = loadAsset(std::filesystem::path(executable.data()).parent_path() /
        bundle.at("directory").get<std::string>() / document, 64 * 1024);
    if (digest(html) != bundle.at("document_sha256").get_ref<const std::string&>()) return;
    const auto pos = html.find("<script");
    if (pos == std::string::npos) return;
    html.insert(pos, "<script src=\"/hbui/" + state.prefix + "adapter.js\"></script>");
    state.document_path = document;
    state.bootstrap_document = std::move(html);
}

bool copyNative(void* to, const void* from, std::size_t length) noexcept {
    __try {
        std::memcpy(to, from, length);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

const void* checkedCohtmlEntry(const nlohmann::json& config, HMODULE module, const char* name) {
    using namespace native_profile;
    const auto& entry = config.at("symbols").at(name);
    const auto rva = number(entry.at("rva"));
    const auto text = number(config.at("text_rva"));
    const auto size = number(config.at("text_size"));
    const auto guard = entry.at("entry_check").get<std::string>();
    const auto length = (guard.size() + 1) / 3;
    if (length < 8 || (guard.size() + 1) % 3 || rva < text ||
        std::uint64_t(rva) + length > std::uint64_t(text) + size)
        throw std::runtime_error("Cohtml entry outside .text");
    const auto* address = reinterpret_cast<const std::byte*>(module) + rva;
    for (std::size_t i = 0; i < length; ++i) {
        unsigned char actual{};
        if (!read(address, i, actual) || actual != std::stoul(guard.substr(i * 3, 2), nullptr, 16))
            throw std::runtime_error("Cohtml entry guard mismatch");
    }
    return address;
}

void* bindView(Runtime& state, const nlohmann::json& config) {
    using namespace native_profile;
    const auto module = GetModuleHandleA(config.at("module").get<std::string>().c_str());
    std::array<wchar_t, 32768> file{};
    const auto len = module ? GetModuleFileNameW(module, file.data(), DWORD(file.size())) : 0;
    if (!len || len >= file.size() ||
        digest(loadAsset(file.data(), 32 * 1024 * 1024)) != config.at("sha256").get_ref<const std::string&>())
        throw std::runtime_error("Cohtml DLL fingerprint mismatch");
    IMAGE_DOS_HEADER dos{};
    IMAGE_NT_HEADERS64 nt{};
    if (!read(module, 0, dos) || dos.e_magic != IMAGE_DOS_SIGNATURE ||
        dos.e_lfanew <= 0 || dos.e_lfanew >= 0x10000 ||
        !read(module, dos.e_lfanew, nt) || nt.Signature != IMAGE_NT_SIGNATURE ||
        nt.OptionalHeader.SizeOfImage != number(config.at("image_size")))
        throw std::runtime_error("Cohtml DLL image mismatch");
    const auto advance = checkedCohtmlEntry(config, module, "cohtml::View::Advance");
    const auto url = checkedCohtmlEntry(config, module, "cohtml::Url::serializedString");
    const auto execute = checkedCohtmlEntry(config, module, "cohtml::View::ExecuteScript");
    const auto* base = reinterpret_cast<const std::byte*>(module);
    const auto table_rva = number(config.at("view_vtable_rva"));
    const auto advance_slot = number(config.at("advance_slot"));
    const auto execute_slot = number(config.at("execute_script_slot"));
    const void* slot_target{};
    if (advance_slot >= 128 || std::uint64_t(table_rva) + 128 * sizeof(void*) > nt.OptionalHeader.SizeOfImage ||
        !read(base + table_rva, advance_slot * sizeof(void*), slot_target) || slot_target != advance)
        throw std::runtime_error("Cohtml View Advance vtable entry mismatch");
    if (execute_slot >= 128 || !read(base + table_rva, execute_slot * sizeof(void*), slot_target) ||
        slot_target != execute)
        throw std::runtime_error("Cohtml View ExecuteScript vtable entry mismatch");
    const auto& str = config.at("url_string");
    state.view = {base + table_rva, base, nt.OptionalHeader.SizeOfImage,
        reinterpret_cast<ExecuteScript>(const_cast<void*>(execute)),
        reinterpret_cast<URLString>(const_cast<void*>(url)), number(config.at("current_url_offset")),
        number(str.at("inline_tag_mask")), number(str.at("inline_offset")), number(str.at("heap_pointer_offset"))};
    return const_cast<void*>(advance);
}

bool currentViewURL(void* view, const ViewAbi& abi, char* out, std::size_t capacity) noexcept {
    __try {
        if (!view || *static_cast<const void**>(view) != abi.vtable) return false;
        const auto* text = static_cast<const unsigned char*>(abi.serialized_url(
            static_cast<const std::byte*>(view) + abi.current_url));
        if (!text) return false;
        const auto* chars = (*text & abi.inline_tag) ? reinterpret_cast<const char*>(text + abi.inline_offset) :
            *reinterpret_cast<const char* const*>(text + abi.heap_offset);
        if (!chars) return false;
        for (std::size_t i = 0; i < capacity; ++i) {
            out[i] = chars[i];
            if (!out[i]) return i != 0;
        }
        return false;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

void observeView(Runtime& state, void* view) {
    const void* table{};
    const bool readable = view && copyNative(&table, view, sizeof table);
    const auto& abi = state.view;
    if (!state.logged_advance.exchange(true)) {
        char line[200];
        std::snprintf(line, sizeof line, "R9 Advance entered tid=%lu view=%p table=%p expected=%p",
            GetCurrentThreadId(), view, table, abi.vtable);
        logDiagnostic(line);
    }
    if (!readable || table != abi.vtable || state.dumped_view.exchange(true)) return;
    char url[512]{};
    const bool have_url = currentViewURL(view, abi, url, sizeof url);
    const auto base = reinterpret_cast<std::uintptr_t>(abi.module);
    char line[256];
    std::snprintf(line, sizeof line, "R9 LIVE VIEW DUMP-BEGIN view=%p table=%p module=%p", view, table, abi.module);
    std::string dump(line);
    dump += "\nurl=";
    dump += have_url ? url : "<unreadable>";
    for (std::size_t slot = 0; slot < 128; ++slot) {
        const void* target{};
        const auto ok = copyNative(&target, static_cast<const std::byte*>(table) + slot * sizeof(void*), sizeof target);
        const auto address = reinterpret_cast<std::uintptr_t>(target);
        if (ok && address >= base && address - base < abi.image_size)
            std::snprintf(line, sizeof line, "\nR9 VT SLOT %03zu RVA=0x%llX", slot, static_cast<unsigned long long>(address - base));
        else
            std::snprintf(line, sizeof line, "\nR9 VT SLOT %03zu %s target=%p", slot, ok ? "outside-module" : "unreadable", target);
        dump += line;
    }
    dump += "\nR9 LIVE VIEW DUMP-END";
    logDiagnostic(dump);
}

bool runViewScript(void* view, ExecuteScript execute, const char* script) noexcept {
    __try { execute(view, script); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

unsigned __fastcall advanceView(void* view, double time) {
    auto& state = *runtime;
    try { observeView(state, view); } catch (...) {}
    if (!state.active.load(std::memory_order_acquire))
        return reinterpret_cast<Advance>(state.originals[2])(view, time);
    try {
        // A boot response proves the bridge, not that the current document
        // still owns the adapter. Gameface can reload even at the same View
        // address and URL. The installer is idempotent within each document.
        struct DocumentProbe { void* view{}; unsigned remaining{}; };
        thread_local DocumentProbe probe;
        char url[512]{};
        if (currentViewURL(view, state.view, url, sizeof url)) {
            const bool isHbui = std::string_view(url) == "ui://./hbui/index.html";
            if (!isHbui && probe.view == view) probe = {};
            if (isHbui) {
                if (probe.view != view) probe = {view, 0};
                if (probe.remaining) {
                    --probe.remaining;
                } else {
                    const bool proved = state.proof_received.load(std::memory_order_acquire);
                    probe.remaining = proved ? 14 : 59;
                    if (!state.adapter_received.load(std::memory_order_acquire))
                        logDiagnostic(proved ? "R9 installing live settings query/command adapter" :
                            "R9 ExecuteScript marker dispatched on the View update thread");
                    const auto& script = proved ? state.adapter_script : state.proof_script;
                    if (!runViewScript(view, state.view.execute_script, script.c_str()))
                        logDiagnostic("R9 ExecuteScript native exception");
                }
            }
        }
    } catch (...) {
        OutputDebugStringA("[BedrockModSettings] View adapter probe failed\n");
    }
    return reinterpret_cast<Advance>(state.originals[2])(view, time);
}

struct RequestInfo {
    std::string path;
    bool range{};
};

RequestInfo requestInfo(const void* request, const Abi& abi) {
    // Full, pre-filesystem ResourceRequest URL (no CRT object crosses the DLL).
    if (!request) return {};
    const auto* url = static_cast<const std::byte*>(request) + abi.url;
    std::size_t count{}, capacity{};
    unsigned char range{};
    const void* chars = url;
    if (!native_profile::read(request, abi.range, range) ||
        !native_profile::read(url, abi.size, count) || !native_profile::read(url, abi.capacity, capacity) ||
        count == 0 || count > 26'000 || count > capacity ||
        (capacity >= abi.inline_bytes && (!native_profile::read(url, 0, chars) || !chars))) return {};
    std::string path(count, '\0');
    if (!copyNative(path.data(), chars, count)) return {};
    // Gameface's local scheme is ui://. Resolve the resource path, not its
    // authority; the unguessable SDK prefix is checked before serving any data.
    if (path.starts_with("ui://")) {
        const auto offset = path.find(route, 4);
        if (offset == std::string::npos) return {};
        path.erase(0, offset);
    }
    if (!path.starts_with(route)) return {};
    return {path.substr(route.size()), range != 0};
}

// MSVC C2712: a function containing __try must not require object unwinding.
// respond() needs a std::string for Content-Length, so the SEH-wrapped game
// calls live in this POD-only helper (same split as runViewScript above).
bool respondSeh(void* response, const Abi& abi, const void* data, std::size_t size,
                const char* type, const char* length) noexcept {
    __try {
        auto* implementation = *reinterpret_cast<void**>(static_cast<std::byte*>(response) + abi.implementation);
        if (!implementation) return false;
        const auto* table = *static_cast<void***>(implementation);
        const auto allocate = reinterpret_cast<Allocate>(table[abi.allocate]);
        const auto header = reinterpret_cast<SetHeader>(table[abi.set_header]);
        if (!allocate || !header) return false;
        auto* output = allocate(implementation, size);
        if (!output) return false;
        std::memcpy(output, data, size);
        header(implementation, "Content-Type", type);
        header(implementation, "Content-Length", length);
        header(implementation, "Cache-Control", "no-store");
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool respond(void* response, const Abi& abi, std::string_view bytes, const char* type) noexcept {
    if (!response || bytes.empty() || bytes.size() > 48 * 1024 * 1024) return false;
    const std::string length = std::to_string(bytes.size());
    return respondSeh(response, abi, bytes.data(), bytes.size(), type, length.c_str());
}

template<std::size_t Index> unsigned __fastcall resource(void* self, const void* request, void* response) {
    auto& state = *runtime;
    if (state.active.load(std::memory_order_acquire)) {
        try {
            const auto info = requestInfo(request, state.abi);
            const auto& path = info.path;
            if (!state.logged_resource[Index].exchange(true))
                logDiagnostic("R9 resource handler entered index=" + std::to_string(Index));
            if (!info.range && !state.bootstrap_document.empty() && path == state.document_path &&
                respond(response, state.abi, state.bootstrap_document, "text/html; charset=utf-8")) {
                logDiagnostic("OreUI bootstrap document served");
                return state.abi.handled;
            }
            if (!info.range && path.starts_with(state.prefix)) {
                auto endpoint = std::string_view(path).substr(state.prefix.size());
                if (endpoint == "adapter.js" &&
                    respond(response, state.abi, state.adapter_script, "application/javascript; charset=utf-8"))
                    return state.abi.handled;
                constexpr std::string_view boots[] = {
                    "marker-r9-execute-20261119", "started-with-engine", "started-without-engine", "executed", "script-error",
                    "runtime-error", "promise-error", "console-error", "route-empty", "route-settings", "route-other"};
                if (endpoint.starts_with("boot/") && endpoint.ends_with(".json") && endpoint.size() >= 10) {
                    const auto phase = endpoint.substr(5, endpoint.size() - 10);
                    bool allowed = false;
                    for (const auto allowed_phase : boots) allowed |= (phase == allowed_phase);
                    allowed |= phase.starts_with("startup") || phase.starts_with("engine-") ||
                        phase.starts_with("marker:") || phase.starts_with("script-error:") || phase.starts_with("promise-error:") ||
                        phase.starts_with("router-hook-error:") || phase.starts_with("route-") ||
                        phase.starts_with("menus-") || phase.starts_with("settings-") ||
                        phase.starts_with("sidebar-") || phase.starts_with("content-") ||
                        phase.starts_with("write-");
                    if (allowed && phase.size() <= 160) {
                        logDiagnostic("OreUI JavaScript: " + std::string(phase));
                        if (respond(response, state.abi, "{}", "application/json; charset=utf-8")) {
                            if (phase == "marker-r9-execute-20261119")
                                state.proof_received.store(true, std::memory_order_release);
                            if (phase == "settings-adapter-installed")
                                state.adapter_received.store(true, std::memory_order_release);
                            return state.abi.handled;
                        }
                    }
                }
                const auto reply = settingsResource(state.store, endpoint);
                if (reply && respond(response, state.abi, *reply, "application/json; charset=utf-8")) {
                    if (!state.logged_endpoint.exchange(true)) logDiagnostic("OreUI bridge connected");
                    return state.abi.handled;
                }
            }
        } catch (...) {
            OutputDebugStringA("[BedrockModSettings] Resource request failed\n");
        }
    }
    return reinterpret_cast<Handler>(state.originals[Index])(self, request, response);
}

void install() noexcept {
    try {
        using namespace native_profile;
        logDiagnostic("R9 install entered (live observation + execution marker)");
        const auto profile = nlohmann::json::parse(generated::kSymbolDatabase);
        const auto& target = profile.at("target");
        if (target.at("version") != "1.26.65.1") return;
        const auto& layout = profile.at("layouts").at("OreUISettings");
        if (layout.at("schema") != 1) return;
        Resolver resolver;
        std::string error;
        if (!resolver.loadJson(generated::kSymbolDatabase, error) ||
            !resolver.attachWindowsMainModule(error) || !resolver.exactBuild())
            throw std::runtime_error("EXE fingerprint mismatch");
        const auto& req = layout.at("request");
        const auto& res = layout.at("response");
        auto state = std::make_unique<Runtime>(settingsPath(), Abi{
            number(req.at("url_offset")), number(req.at("string_size_offset")), number(req.at("string_capacity_offset")),
            number(req.at("string_inline_bytes")), number(req.at("has_range_offset")), number(res.at("implementation_offset")),
            number(res.at("allocate_buffer_vslot")), number(res.at("set_header_vslot")), number(res.at("handled_status"))});
        std::array<unsigned char, 16> secret{};
        if (BCryptGenRandom(nullptr, secret.data(), static_cast<ULONG>(secret.size()), BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0)
            throw std::runtime_error("cannot generate OreUI bridge token");
        const auto token = hex(secret);
        state->prefix = std::string(endpoint_prefix) + token + "/";
        state->adapter_script = renderer_source;
        constexpr std::string_view placeholder = "__BDMODTOKEN__";
        const auto token_pos = state->adapter_script.find(placeholder);
        if (token_pos == std::string::npos) throw std::runtime_error("missing OreUI bridge token placeholder");
        state->adapter_script.replace(token_pos, placeholder.size(), token);
        state->proof_script = "(()=>{const x=new XMLHttpRequest();x.open('GET','/hbui/"
            + state->prefix + "boot/marker-r9-execute-20261119.json');"
            "x.onload=()=>{if(x.status>=200&&x.status<300){"
            + state->adapter_script + "}};x.send()})();";
        try {
            prepareBootstrap(*state, layout.at("bundle"));
        } catch (const std::exception& error) {
            logDiagnostic(std::string("OreUI early bootstrap unavailable: ") + error.what());
        }
        auto* advance = bindView(*state, layout.at("cohtml"));
        const auto handler = [&](const char* name) -> void* {
            const auto symbol = resolver.resolve(layout.at(name).at("symbol").get<std::string>(), true);
            if (!symbol || !symbol->exact_build || symbol->method != "exact-rva")
                throw std::runtime_error(std::string("OreUI entry check failed: ") + name);
            return reinterpret_cast<void*>(symbol->address);
        };
        auto* hybrid = handler("hybrid_resource");
        auto* pack = handler("pack_resource");
        HMODULE pinned{};
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                                reinterpret_cast<LPCWSTR>(&install), &pinned))
            throw std::runtime_error("cannot pin OreUI runtime");
        runtime = state.release();
        if (!BDS_HookDirectInstall(advance, reinterpret_cast<void*>(&advanceView), &runtime->originals[2], &runtime->tokens[2])) {
            throw std::runtime_error("could not hook the Cohtml view update");
        }
        logDiagnostic("R9 Advance detour installed");
        if (!BDS_HookDirectInstall(hybrid, reinterpret_cast<void*>(&resource<0>), &runtime->originals[0], &runtime->tokens[0]) ||
            !BDS_HookDirectInstall(pack, reinterpret_cast<void*>(&resource<1>), &runtime->originals[1], &runtime->tokens[1]))
            throw std::runtime_error("could not hook both OreUI resource handlers");
        runtime->active.store(true, std::memory_order_release);
        logDiagnostic("OreUI hooks installed");
    } catch (const std::exception& error) {
        logDiagnostic(error.what());
    } catch (...) {
        logDiagnostic("Runtime unavailable");
    }
}

bool available() noexcept {
    return runtime && runtime->active.load(std::memory_order_acquire);
}

// Only this POD function table crosses DLL boundaries. Each Mod can carry
// the implementation; the first one to register owns the shared store/hooks.
struct Service {
    Result (*add)(const PageDescriptor*) noexcept;
    Result (*remove)(const char*) noexcept;
    Result (*read)(const char*, const char*, RawValue*, char*, std::uint32_t) noexcept;
};

Result add(const PageDescriptor* page) noexcept {
    try {
        return runtime->store.add(*page);
    } catch (...) {
        return Result::StorageError;
    }
}

Result remove(const char* id) noexcept {
    try {
        return runtime->store.remove(id);
    } catch (...) {
        return Result::StorageError;
    }
}

Result read(const char* mod_id, const char* control_id, RawValue* out,
            char* buffer, std::uint32_t capacity) noexcept {
    try {
        return runtime->store.read(mod_id, control_id, *out, buffer, capacity);
    } catch (...) {
        return Result::StorageError;
    }
}

constexpr Service local_service{add, remove, read};
constexpr auto service_export = "BedrockSDK_ModSettingsServiceV3";
std::atomic<const Service*> service{};

const Service* findService(bool create) {
    if (const auto* cached = service.load(std::memory_order_acquire)) return cached;
    // Serialize discovery and installation across all SDK copies in this
    // process. Never hold the election lock while dispatching store callbacks.
    const auto name = L"Local\\BedrockSDK.ModSettings." + std::to_wstring(GetCurrentProcessId());
    const auto mutex = CreateMutexW(nullptr, FALSE, name.c_str());
    if (!mutex) return nullptr;
    struct MutexScope {
        HANDLE handle;
        bool acquired{};
        ~MutexScope() { if (acquired) ReleaseMutex(handle); CloseHandle(handle); }
    } lock{mutex};
    const auto wait = WaitForSingleObject(mutex, 30'000);
    if (wait != WAIT_OBJECT_0 && wait != WAIT_ABANDONED) return nullptr;
    lock.acquired = true;

    std::vector<HMODULE> modules(128);
    DWORD bytes{};
    for (;;) {
        if (!K32EnumProcessModules(GetCurrentProcess(), modules.data(),
                static_cast<DWORD>(modules.size() * sizeof(HMODULE)), &bytes)) return nullptr;
        if (bytes <= modules.size() * sizeof(HMODULE)) break;
        modules.resize(bytes / sizeof(HMODULE));
    }
    bool legacy_active = false;
    for (std::size_t i = 0; i < bytes / sizeof(HMODULE); ++i) {
        HMODULE retained{};
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                                reinterpret_cast<LPCWSTR>(modules[i]), &retained)) continue;
        const auto query = reinterpret_cast<const Service* (*)() noexcept>(GetProcAddress(retained, service_export));
        const auto* found = query ? query() : nullptr;
        const auto legacy = reinterpret_cast<const void* (*)() noexcept>(
            GetProcAddress(retained, "BedrockSDK_ModSettingsServiceV2"));
        if (legacy && legacy()) legacy_active = true;
        FreeLibrary(retained);
        if (found) {
            service.store(found, std::memory_order_release);
            return found;
        }
    }
    if (!create || legacy_active) return nullptr;
    static std::once_flag once;
    std::call_once(once, install);
    if (!available()) return nullptr;
    service.store(&local_service, std::memory_order_release);
    return &local_service;
}
} // namespace

extern "C" __declspec(dllexport) const Service* BedrockSDK_ModSettingsServiceV3() noexcept {
    return available() ? &local_service : nullptr;
}

Result registerPage(const PageDescriptor& page) noexcept {
    try {
        const auto* shared = findService(true);
        return shared ? shared->add(&page) : Result::Unavailable;
    } catch (...) { return Result::StorageError; }
}

Result unregisterPage(const char* mod_id) noexcept {
    if (!mod_id) return Result::Invalid;
    try {
        const auto* shared = findService(false);
        return shared ? shared->remove(mod_id) : Result::Unavailable;
    } catch (...) { return Result::StorageError; }
}

Result readValue(const char* mod_id, const char* control_id, RawValue& out,
                 char* buffer, std::uint32_t capacity) noexcept {
    if (!mod_id || !control_id) return Result::Invalid;
    try {
        const auto* shared = findService(false);
        return shared ? shared->read(mod_id, control_id, &out, buffer, capacity) : Result::Unavailable;
    } catch (...) { return Result::StorageError; }
}
} // namespace bedrock::detail::oreui_settings
