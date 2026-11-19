// BlockProbe — throwaway RE probe for the 1.26.65.1 port.
//
// AutoMine's block scan finds nothing on 1.26.65.1: every cell reads as
// air. This probe walks the exact path the SDK Reader takes (Bindings::
// block -> block_type_offset -> typeName) for the blocks under the
// player's feet, logs where it breaks, and brute-scans the Block and
// BlockLegacy objects for an MSVC std::string holding a "minecraft:*"
// identifier so the correct offsets can be read off directly.
#include <bedrock/runtime/game_hooks.hpp>
#include <bedrock/profiles/profile_manager.hpp>
#include <bedrock/profiles/profile_json.hpp>
#include <bedrock/detail/native_world_access.hpp>
#include <bedrock/runtime/hook_runtime.h>
#include <knownfolders.h>
#include <shlobj.h>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>

namespace {
using bedrock::world_native::Bindings;
using bedrock::world_native::Reader;
using bedrock::world_native::StringCopy;
using bedrock::world_native::copyString;
using bedrock::world_native::read;

bool heapPtr(std::uintptr_t p) {
    return p > 0x10000 && p < 0x7FFFFFFFFFFFull && (p & 7) == 0;
}
std::string toHex(std::uintptr_t v) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%llx", static_cast<unsigned long long>(v));
    return buf;
}
bool looksIdentifier(const std::string& s) {
    if (s.size() < 4 || s.rfind("minecraft:", 0) != 0) return false;
    for (unsigned char c : s)
        if (c <= 32 || c >= 127) return false;
    return true;
}

class BlockProbeMod {
  public:
    std::atomic<unsigned> status{};
    void signalStop() noexcept { stopped_ = true; }
    void run() noexcept {
        status = 1;
        try {
            PWSTR path{};
            if (FAILED(SHGetKnownFolderPath(FOLDERID_Documents, KF_FLAG_CREATE, nullptr, &path)))
                throw std::runtime_error("Documents directory unavailable");
            const auto directory = std::filesystem::path(path) / L"PEXData" / L"BlockProbe";
            CoTaskMemFree(path);
            std::filesystem::create_directories(directory);
            std::ofstream log(directory / L"BlockProbe.log", std::ios::trunc);
            if (!log) throw std::runtime_error("could not open BlockProbe.log");
            initialize();
            while (!stopped_) {
                flush(log);
                std::this_thread::sleep_for(std::chrono::milliseconds{100});
            }
            hooks_.reset();
            flush(log);
            status = 3;
        } catch (const std::exception& error) {
            note(std::string("startup failed: ") + error.what());
            status = 4;
            try {
                const auto* home = _wgetenv(L"USERPROFILE");
                if (home) {
                    auto directory = std::filesystem::path(home) / L"Documents" / L"PEXData" / L"BlockProbe";
                    std::filesystem::create_directories(directory);
                    std::ofstream log(directory / L"BlockProbe.log", std::ios::app);
                    flush(log);
                }
            } catch (...) {}
        } catch (...) { status = 4; }
    }

  private:
    void note(std::string value) {
        OutputDebugStringA(("[BlockProbe] " + value + "\n").c_str());
        std::lock_guard lock(logMutex_);
        if (logs_.size() < 1024) logs_.push_back(std::move(value));
    }
    void flush(std::ostream& log) {
        std::deque<std::string> batch;
        { std::lock_guard lock(logMutex_); batch.swap(logs_); }
        for (const auto& value : batch) log << value << '\n';
        if (!batch.empty()) log.flush();
    }
    void initialize() {
        bedrock::ProfileManager profiles;
        std::string error;
        const bedrock::ProfileOptions options{bedrock::UpdatePolicy::RefreshIfNeeded,
                                              bedrock::CandidatePolicy::AllowCandidates};
        if (!profiles.prepare(
                {bedrock::Feature::Gameplay, bedrock::Feature::LevelTick}, options,
                [this](const bedrock::RuntimeProfile& profile, std::string& failure) {
                    try {
                        try {
                            bindings_.load(profile.json());
                            note("bindings: load() ok");
                        } catch (const std::exception& e) {
                            note(std::string("bindings: load() failed (") + e.what() + "); trying loadWorld");
                            bindings_.loadWorld(profile.json());
                            note("bindings: loadWorld() ok");
                        }
                        return true;
                    } catch (const std::exception& e) { failure = e.what(); return false; }
                },
                error))
            throw std::runtime_error(error);
        game_.Event.Enum.Gameplay.then([this](const bedrock::GameplayEvent& event) {
            if (event.clientInstance) client_.store(event.clientInstance);
        });
        game_.Event.Enum.LevelTick.then([this](const bedrock::LevelTickEvent&) {
            if (stopped_ || probed_) return;
            probe();
        });
        const auto profile = profiles.snapshot();
        hooks_ = std::make_unique<bedrock::GameHooks>(game_);
        if (!hooks_->installGameplay(profile->resolver(), error) ||
            !hooks_->installLevelTick(profile->resolver(), error))
            throw std::runtime_error(error);
        note("BlockProbe attached");
    }

    void scanStrings(const void* base, std::size_t limit, const std::string& label) {
        for (std::size_t off = 0; off + 32 <= limit; off += 4) {
            StringCopy sc;
            if (!copyString(static_cast<const std::byte*>(base) + off, &sc)) continue;
            if (sc.size < 4 || sc.size > 128) continue;
            std::string s(sc.bytes, sc.size);
            if (looksIdentifier(s))
                note("  string hit " + label + "+0x" + toHex(off) + ": " + s);
        }
    }

    void probe() {
        void* client = client_.load();
        if (!client) return;
        void* player = bindings_.local(client);
        void* region = bindings_.region(client);
        if (!player || !region) return;
        probed_ = true;
        note("player=0x" + toHex(reinterpret_cast<std::uintptr_t>(player)) +
             " region=0x" + toHex(reinterpret_cast<std::uintptr_t>(region)));
        bedrock::Vec3 pos{};
        std::uint64_t rt{};
        if (!bindings_.pose(player, pos, rt)) { note("pose failed"); return; }
        note("feet: " + std::to_string(pos.x) + "," + std::to_string(pos.y) + "," + std::to_string(pos.z));
        const int fx = static_cast<int>(std::floor(pos.x));
        const int fy = static_cast<int>(std::floor(pos.y));
        const int fz = static_cast<int>(std::floor(pos.z));
        for (int dy : {-1, -2}) {
            bedrock::BlockPos bp; bp.x = fx; bp.y = fy + dy; bp.z = fz;
            void* state = bindings_.block(region, bp);
            note("block(" + std::to_string(bp.x) + "," + std::to_string(bp.y) + "," +
                 std::to_string(bp.z) + ") state=0x" +
                 toHex(reinterpret_cast<std::uintptr_t>(state)));
            if (!state) continue;
            Reader reader(bindings_);
            auto* info = reader.describe(state);
            note(std::string("  reader.describe -> ") + (info ? ("name=" + info->name) : "nullptr"));
            void* type{};
            if (read(state, bindings_.layout("block_type_offset"), type)) {
                note("  type@block_type_offset(0x" + toHex(bindings_.layout("block_type_offset")) +
                     ")=0x" + toHex(reinterpret_cast<std::uintptr_t>(type)));
                if (type) {
                    note("  reader.typeName -> '" + reader.typeName(type) + "'");
                    std::uint8_t flags{};
                    if (read(type, bindings_.layout("block_type_flags_offset"), flags))
                        note("  flags@0x" + toHex(bindings_.layout("block_type_flags_offset")) +
                             "=" + std::to_string(flags));
                }
            } else note("  type read failed");
            if (dy == -1) {
                note("  scanning state object for identifier strings:");
                scanStrings(state, 0x200, "state");
                for (std::size_t off1 = 0; off1 + 8 <= 0x100; off1 += 8) {
                    void* t{};
                    if (!read(state, off1, t) || !t) continue;
                    if (!heapPtr(reinterpret_cast<std::uintptr_t>(t))) continue;
                    scanStrings(t, 0x600, "*(state+0x" + toHex(off1) + ")");
                }
            }
        }
        note("probe done");
    }

    Bindings bindings_{};
    bedrock::GameApi game_;
    std::unique_ptr<bedrock::GameHooks> hooks_;
    std::atomic<void*> client_{};
    std::atomic<bool> stopped_{};
    bool probed_{};
    std::mutex logMutex_;
    std::deque<std::string> logs_;
};
std::atomic<BlockProbeMod*> mod{};
std::atomic<unsigned> initialStatus{};
std::atomic<bool> stopRequested{};
DWORD WINAPI start(void*) {
    HMODULE pinned{};
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                            reinterpret_cast<LPCWSTR>(&start), &pinned)) {
        initialStatus = 4;
        return 1;
    }
    try {
        auto* instance = new BlockProbeMod;
        mod = instance;
        if (stopRequested) instance->signalStop();
        instance->run();
    } catch (...) { initialStatus = 4; }
    return 0;
}
} // namespace
extern "C" __declspec(dllexport) unsigned BlockProbe_GetStatus() {
    auto* instance = mod.load();
    return instance ? instance->status.load() : initialStatus.load();
}
extern "C" __declspec(dllexport) void BlockProbe_Stop() {
    stopRequested = true;
    if (auto* instance = mod.load()) instance->signalStop();
}
BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(instance);
        if (HANDLE thread = CreateThread(nullptr, 0, start, nullptr, 0, nullptr)) CloseHandle(thread);
        else initialStatus = 4;
    } else if (reason == DLL_PROCESS_DETACH) stopRequested = true;
    return TRUE;
}
