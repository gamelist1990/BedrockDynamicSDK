// InputProbe — read-only diagnostic mod (main tree, throwaway).
//
// Dumps the local player's MoveInputComponent (the component the
// independent automation controls patch) whenever its flags word or
// analog move values change. Ground truth for the AutomationControls
// flag masks: hold REAL keys (ydotool) and compare the bits against
// the layout in data/<ver>/windows-x64/classes/AutomationControls.json.
//
// Logs to Documents/PEXData/InputProbe/InputProbe.log. Never writes
// game memory.

#include <bedrock/runtime/game_hooks.hpp>
#include <bedrock/api/game_api.hpp>
#include <bedrock/native/game_access.hpp>
#include <bedrock/native/world_access.hpp>
#include <bedrock/detail/native_player_context.hpp>
#include <bedrock/detail/native_profile_functions.hpp>
#include <bedrock/profiles/profile_manager.hpp>
#include <bedrock/profiles/profile_json.hpp>
#include <bedrock/runtime/hook_runtime.h>
#include <knownfolders.h>
#include <shlobj.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <deque>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>

namespace {

bool readRaw(void* p, std::size_t at, void* dst, std::size_t size) noexcept {
    __try {
        std::memcpy(dst, static_cast<const std::byte*>(p) + at, size);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

class InputProbeMod {
public:
    std::atomic<unsigned> status{0};
    void stop() { stopped_ = true; }
    void run() {
        status = 1;
        try {
            const auto dir = directory();
            std::filesystem::create_directories(dir);
            initialize();
            status = 2;
            std::ofstream out(dir / L"InputProbe.log", std::ios::trunc);
            while (!stopped_.load()) {
                flush(out);
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
            flush(out);
            status = 3;
        } catch (const std::exception& error) {
            status = 4;
            try {
                const auto dir = directory();
                std::filesystem::create_directories(dir);
                std::ofstream out(dir / L"InputProbe.log", std::ios::app);
                flush(out);
                out << "startup failed: " << error.what() << '\n';
            } catch (...) {
            }
        }
    }

private:
    static std::filesystem::path directory() {
        PWSTR path{};
        if (FAILED(SHGetKnownFolderPath(FOLDERID_Documents, KF_FLAG_CREATE, nullptr, &path)))
            throw std::runtime_error("Documents unavailable");
        const auto documents = std::filesystem::path(path);
        CoTaskMemFree(path);
        return documents / L"PEXData" / L"InputProbe";
    }
    void note(std::string text) {
        std::lock_guard lock(logMutex_);
        if (logs_.size() < 512) logs_.push_back(std::move(text));
    }
    void flush(std::ostream& out) {
        std::deque<std::string> batch;
        { std::lock_guard lock(logMutex_); batch.swap(logs_); }
        for (const auto& line : batch) out << line << '\n';
        if (!batch.empty()) out.flush();
    }
    void initialize() {
        bedrock::ProfileManager profiles;
        const bedrock::ProfileRequirements required{
            bedrock::Feature::Gameplay, bedrock::Feature::LevelTick, bedrock::Feature::World,
            bedrock::Feature::Motion, bedrock::Feature::Gui, bedrock::Feature::WorldHeight,
            bedrock::Feature::WorldEvents, bedrock::Feature::Actors, bedrock::Feature::Camera};
        const bedrock::ProfileOptions options{bedrock::UpdatePolicy::RefreshIfNeeded,
                                              bedrock::CandidatePolicy::AllowCandidates};
        std::string error;
        if (!profiles.prepare(required, options,
                [this](const bedrock::RuntimeProfile& profile, std::string& failure) {
                    const auto root = bedrock::parseNativeProfile(profile.json());
                    const auto& layout = root.at("layouts").at("AutomationControls");
                    hash_ = static_cast<std::uint32_t>(
                        bedrock::native_profile::number(layout.at("component_hash")));
                    size_ = static_cast<std::size_t>(
                        bedrock::native_profile::number(layout.at("component_size")));
                    if (!context_.load(profile, failure)) return false;
                    try { world_.load(profile.json()); }
                    catch (const std::exception& e) { failure = e.what(); return false; }
                    std::string accessError;
                    if (!access_.load(profile, accessError))
                        note("game access unavailable: " + accessError);
                    return true;
                }, error))
            throw std::runtime_error(error);
        game_.Event.Enum.Gameplay.then([this](const bedrock::GameplayEvent& event) {
            if (event.clientInstance) client_.store(event.clientInstance);
        });
        game_.Event.Enum.WorldChange.then([this](const bedrock::WorldChangeEvent& event) {
            if (event.clientInstance) client_.store(event.clientInstance);
            ++epoch_;
        });
        game_.Event.Enum.LevelTick.then([this](const bedrock::LevelTickEvent&) {
            if (stopped_.load()) return;
            ++ticks_;
            // GameMode dump: find the destroy-progress field while a real
            // (manual) mine runs. Throttled to every 5 ticks, on change.
            if (ticks_ % 5 == 0) {
                void* player = world_.localPlayer(client_.load());
                void* gm = player ? world_.gameMode(player) : nullptr;
                if (player) {
                    // Player bytes around +0xb4a: continueDestroyBlock
                    // gates its progress math on [player+0xb4a]==1
                    // (found by disassembly of the 1.26.65.1 build).
                    std::array<unsigned char, 32> praw{};
                    if (readRaw(player, 0xb38, praw.data(), praw.size()) && praw != lastPlayer_) {
                        lastPlayer_ = praw;
                        char buf[512];
                        int n = std::snprintf(buf, sizeof(buf), "player t=%llu +b38:",
                                              static_cast<unsigned long long>(ticks_));
                        for (std::size_t i = 0; i < praw.size(); ++i)
                            n += std::snprintf(buf + n, sizeof(buf) - n, " %02x", praw[i]);
                        note(buf);
                    }
                }
                if (player && gm && ticks_ % 20 == 0) {
                    // Identity check: is [gm+8] (the object the gate
                    // reads +0xb4a from) the same as localPlayer?
                    void* gmPlayer = nullptr;
                    unsigned char flagPlayer = 0xEE, flagGm = 0xEE;
                    readRaw(gm, 8, &gmPlayer, 8);
                    if (gmPlayer) readRaw(gmPlayer, 0xb4a, &flagGm, 1);
                    readRaw(player, 0xb4a, &flagPlayer, 1);
                    char buf[256];
                    std::snprintf(buf, sizeof(buf),
                                  "ident t=%llu local=%p gm8=%p same=%d flagLocal=%u flagGm=%u",
                                  static_cast<unsigned long long>(ticks_), player, gmPlayer,
                                  player == gmPlayer ? 1 : 0,
                                  static_cast<unsigned>(flagPlayer), static_cast<unsigned>(flagGm));
                    note(buf);
                }
                if (gm) {
                    std::array<unsigned char, 288> graw{};
                    if (readRaw(gm, 0, graw.data(), graw.size()) && graw != lastGm_) {
                        lastGm_ = graw;
                        char buf[2048];
                        int n = std::snprintf(buf, sizeof(buf), "gm t=%llu floats01:",
                                              static_cast<unsigned long long>(ticks_));
                        for (std::size_t off = 0; off + 4 <= graw.size(); off += 4) {
                            float f;
                            std::memcpy(&f, graw.data() + off, 4);
                            if (f > 0.0001f && f < 0.9999f)
                                n += std::snprintf(buf + n, sizeof(buf) - n, " +%zx=%.4f",
                                                   off, static_cast<double>(f));
                        }
                        note(buf);
                        double start28{}; int state84{}; long long at90{};
                        readRaw(gm, 0x28, &start28, 8);
                        readRaw(gm, 0x84, &state84, 4);
                        readRaw(gm, 0x90, &at90, 8);
                        n = std::snprintf(buf, sizeof(buf),
                                          "  gm fields: +28(start)=%.4f +84(state)=%d +90=%lld",
                                          start28, state84, at90);
                        note(buf);
                        for (std::size_t base = 0; base < 96; base += 16) {
                            n = std::snprintf(buf, sizeof(buf), "  gm+%02zx:", base);
                            for (std::size_t i = base; i < base + 16; ++i)
                                n += std::snprintf(buf + n, sizeof(buf) - n, " %02x", graw[i]);
                            note(buf);
                        }
                    }
                }
            }
            auto scope = context_.tick(client_.load(), bedrock::WorldEpoch{epoch_.load()});
            if (!context_.valid()) return;
            void* comp = context_.component(hash_);
            if (!comp) return;
            std::uint32_t flags{};
            float left{}, forward{};
            if (!readRaw(comp, 0, &flags, 4) || !readRaw(comp, 4, &left, 4) ||
                !readRaw(comp, 8, &forward, 4))
                return;
            std::array<unsigned char, 100> raw{};
            readRaw(comp, 0, raw.data(), size_ <= raw.size() ? size_ : raw.size());
            bool changed = false;
            for (std::size_t i = 0; i < 40 && i < size_; ++i)
                if (raw[i] != lastRaw_[i]) changed = true;
            if (!changed) return;
            lastRaw_ = raw;
            lastFlags_ = flags; lastLeft_ = left; lastForward_ = forward;
            char buf[2048];
            int n = std::snprintf(buf, sizeof(buf), "t=%llu flags=0x%08x left=%.3f fwd=%.3f",
                                  static_cast<unsigned long long>(ticks_), flags, left, forward);
            note(buf);
            for (std::size_t base = 0; base < 40 && base < size_; base += 16) {
                n = std::snprintf(buf, sizeof(buf), "  +%02zx:", base);
                for (std::size_t i = base; i < base + 16 && i < size_; ++i)
                    n += std::snprintf(buf + n, sizeof(buf) - n, " %02x", raw[i]);
                note(buf);
            }
        });
        const auto selected = profiles.snapshot();
        hooks_ = std::make_unique<bedrock::GameHooks>(game_);
        if (!hooks_->installGameplay(selected->resolver(), error) ||
            !hooks_->installLevelTick(selected->resolver(), error) ||
            !hooks_->installWorldEvents(selected->resolver(), selected->json(), error))
            throw std::runtime_error(error);
        note("input probe: subscribed");
    }

    bedrock::NativeGameAccess access_;
    bedrock::GameApi game_{access_};
    bedrock::NativeWorldAccess world_;
    std::array<unsigned char, 288> lastGm_{};
    std::array<unsigned char, 32> lastPlayer_{};
    bedrock::native_player::Context context_;
    std::unique_ptr<bedrock::GameHooks> hooks_;
    std::atomic<void*> client_{};
    std::atomic<std::uint64_t> epoch_{1};
    std::atomic<bool> stopped_{};
    std::uint32_t hash_{};
    std::size_t size_{100};
    std::uint64_t ticks_{};
    std::uint32_t lastFlags_{0xFFFFFFFF};
    float lastLeft_{1e9f}, lastForward_{1e9f};
    std::array<unsigned char, 100> lastRaw_{};
    std::mutex logMutex_;
    std::deque<std::string> logs_;
};

InputProbeMod mod;

DWORD WINAPI start(void*) {
    HMODULE pinned{};
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                       reinterpret_cast<LPCWSTR>(&start), &pinned);
    mod.run();
    return 0;
}
} // namespace

extern "C" __declspec(dllexport) unsigned InputProbe_GetStatus() { return mod.status.load(); }
extern "C" __declspec(dllexport) void InputProbe_Stop() { mod.stop(); }
BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(instance);
        if (const auto thread = CreateThread(nullptr, 0, start, nullptr, 0, nullptr)) CloseHandle(thread);
    }
    return TRUE;
}
