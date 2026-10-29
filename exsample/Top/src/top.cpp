#ifdef _WIN32
#include <bedrock/runtime/game_hooks.hpp>
#include <bedrock/native/command_service.hpp>
#include <bedrock/native/safe_teleport.hpp>
#include <knownfolders.h>
#include <shlobj.h>
#include <filesystem>
#include <fstream>
#include <thread>

#if !defined(_MSC_VER) || _ITERATOR_DEBUG_LEVEL != 0
#error Top requires MSVC Release STL ABI.
#endif

namespace {
using namespace std::chrono_literals;

std::string reason(bedrock::TeleportError error) {
    using enum bedrock::TeleportError;
    switch (error) {
    case InvalidDistance: return "使い方: /top または /top <正の整数ブロック数>";
    case DistanceLimit: return "設定された移動距離の上限を超えています";
    case UnavailableContext: return "現在のプレイヤー・当たり判定・高さ範囲を取得できません";
    case HeightLimit: return "ディメンションの高さ範囲外です";
    case NoSearchSpace: return "頭上に探索可能な範囲がありません";
    case WorldChanged: return "ワールドまたはプレイヤーが変わりました";
    case PlayerMoved: return "探索中に位置または姿勢が変わりました。もう一度実行してください";
    case ChunkUnavailable: return "移動先のチャンクまたはブロックを読み取れません";
    case BodyObstructed: return "移動先に体・頭が埋まるブロックまたは液体があります";
    case UnsupportedHeight: return "足元の安全な支持面を確認できません";
    case MissingFloor: return "足元に床がありません";
    case UnsafeFloor: return "足元が危険または安全性未確認のブロックです";
    case UnsupportedFloor: return "足元が安全な全面支持ブロックではありません";
    case NativeWriteFailed: return "位置情報の更新に失敗しました";
    case NoSafeDestination: return "頭上に安全な着地点がありません";
    default: return "内部処理に失敗しました";
    }
}

std::filesystem::path directory() {
    PWSTR path{};
    if (FAILED(SHGetKnownFolderPath(FOLDERID_Documents, KF_FLAG_CREATE, nullptr, &path)))
        throw std::runtime_error("Documents unavailable");
    const auto documents = std::filesystem::path(path);
    CoTaskMemFree(path);
    return documents / L"PEXData" / L"Top";
}

class TopMod {
public:
    std::atomic<unsigned> status{};
    void signalStop() { stopped_ = true; }
    void stop() { signalStop(); commands_.stop(); teleport_.stop(); }
    void run() {
        status = 1;
        try {
            const auto dir = directory();
            std::filesystem::create_directories(dir);
            loadConfig(dir);
            initialize();
            status = 2;
            std::ofstream out(dir / L"Top.log", std::ios::trunc);
            while (!stopped_.load()) {
                flush(out);
                std::this_thread::sleep_for(100ms);
            }
            flush(out);
            status = 3;
        } catch (const std::exception& error) {
            status = 4;
            try {
                const auto dir = directory();
                std::filesystem::create_directories(dir);
                std::ofstream out(dir / L"Top.log", std::ios::app);
                flush(out);
                out << "startup failed: " << error.what() << '\n';
            } catch (...) {}
        }
        stop();
    }
private:
    void note(std::string text) {
        std::lock_guard lock(logMutex_);
        if (logs_.size() < 128) logs_.push_back(std::move(text));
    }
    void flush(std::ostream& out) {
        std::deque<std::string> batch;
        { std::lock_guard lock(logMutex_); batch.swap(logs_); }
        for (const auto& line : batch) out << line << '\n';
        if (!batch.empty()) out.flush();
    }
    void reply(const std::string& text) {
        note(text);
        if (!teleport_.message("[Top] " + text)) note("game message unavailable; see Top.log");
    }
    void fail(const std::string& text) { reply("テレポートできません！ " + text); }
    void loadConfig(const std::filesystem::path& dir) {
        const auto path = dir / L"config.json";
        nlohmann::json config;
        if (std::filesystem::exists(path)) { std::ifstream file(path); file >> config; }
        else {
            config = {{"enabled", true}, {"max_rise", 0}, {"scan_levels_per_tick", 8}, {"cooperative_budget_us", 500}};
            std::ofstream file(path); file << config.dump(2) << '\n';
        }
        enabled_ = config.value("enabled", true);
        const auto rise = config.value("max_rise", 0LL);
        if (rise < 0 || rise > 65535) throw std::runtime_error("max_rise must be 0..65535 (0 means unlimited)");
        teleport_.configure({static_cast<unsigned>(rise),
            static_cast<unsigned>(std::clamp(config.value("scan_levels_per_tick", 8), 1, 32)),
            std::chrono::microseconds{std::clamp(config.value("cooperative_budget_us", 500), 100, 2000)}});
    }
    void initialize() {
        commands_.onDiagnostic([this](const std::string& text) { note(text); });
        commands_.onRejected([this](bedrock::CommandSubmission result) {
            fail(result == bedrock::CommandSubmission::Full ? "コマンドが多すぎます。少し待ってから実行してください"
                                                          : reason(bedrock::TeleportError::InvalidDistance));
        });
        teleport_.onResult([this](const bedrock::TeleportResult& result) {
            using enum bedrock::TeleportPhase;
            if (result.phase == Moved) reply("移動しました。足元 Y=" + std::to_string(*result.feetY));
            else if (result.phase == Failed) fail(reason(result.error));
            else note("upward search started");
        });
        if (!commands_.add(bedrock::CommandBuilder(bedrock::CommandName{"top"}, bedrock::ArgumentName{"blocks"})
            .description(bedrock::CommandDescription{"安全な上方移動 / safe upward teleport"})
            .then([this](std::optional<int> blocks) -> bedrock::CommandDecision {
                if (enabled_) return teleport_.request(blocks);
                fail("設定で無効になっています");
                return bedrock::CommandDecision::Rejected;
            }))) throw std::runtime_error("command definition rejected");

        bedrock::ProfileManager profiles;
        const bedrock::ProfileRequirements required{
            bedrock::Feature::Gameplay, bedrock::Feature::LevelTick, bedrock::Feature::World,
            bedrock::Feature::Motion, bedrock::Feature::Gui, bedrock::Feature::WorldHeight,
            bedrock::Feature::PacketSender, bedrock::Feature::CommandPackets,
            bedrock::Feature::ClientCommands, bedrock::Feature::WorldEvents};
        const bedrock::ProfileOptions options{bedrock::UpdatePolicy::RefreshIfNeeded, bedrock::CandidatePolicy::AllowCandidates};
        std::string error;
        if (!profiles.prepare(required, options, [this](const bedrock::RuntimeProfile& profile, std::string& failure) {
            return teleport_.load(profile, failure) && commands_.load(profile, failure);
        }, error)) throw std::runtime_error(error);

        game_.Event.Enum.Gameplay.then([this](const bedrock::GameplayEvent& event) {
            if (event.clientInstance) client_.store(event.clientInstance);
        });
        game_.Event.Enum.WorldChange.then([this](const bedrock::WorldChangeEvent& event) {
            if (event.clientInstance) client_.store(event.clientInstance);
            const auto epoch = ++epoch_;
            commands_.reset(epoch); teleport_.reset(epoch);
        });
        game_.Event.Enum.LevelTick.then([this](const bedrock::LevelTickEvent& event) {
            if (stopped_.load()) return;
            try {
                const auto epoch = epoch_.load();
                auto scope = teleport_.tick(client_.load(), event.level, epoch);
                if (!scope) return;
                commands_.update(client_.load(), epoch);
                commands_.dispatch();
            } catch (const std::exception& error) {
                teleport_.cancel(); note(std::string("tick failed: ") + error.what());
            }
        });
        const auto selected = profiles.snapshot();
        hooks_ = std::make_unique<bedrock::GameHooks>(game_);
        if (!hooks_->installGameplay(selected->resolver(), error) ||
            !hooks_->installLevelTick(selected->resolver(), error) ||
            !hooks_->installWorldEvents(selected->resolver(), selected->json(), error)) throw std::runtime_error(error);
        note("Top revision3; SDK CommandBuilder + native autocomplete + safe upward teleport");
        note("Press Esc and return to gameplay if the client has not been observed yet.");
        note(BDS_HookRuntimeDiagnostic());
    }
    bedrock::GameApi game_;
    bedrock::NativeCommandService commands_;
    bedrock::NativeSafeTeleport teleport_;
    std::unique_ptr<bedrock::GameHooks> hooks_;
    std::atomic<void*> client_{};
    std::atomic<std::uint64_t> epoch_{1};
    std::atomic<bool> stopped_{};
    bool enabled_{true};
    std::mutex logMutex_;
    std::deque<std::string> logs_;
};
TopMod mod;

DWORD WINAPI start(void*) {
    HMODULE pinned{};
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                      reinterpret_cast<LPCWSTR>(&start), &pinned);
    mod.run();
    return 0;
}
} // namespace

extern "C" __declspec(dllexport) unsigned Top_GetStatus() { return mod.status.load(); }
extern "C" __declspec(dllexport) void Top_Stop() { mod.stop(); }
BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(instance);
        if (const auto thread = CreateThread(nullptr, 0, start, nullptr, 0, nullptr)) CloseHandle(thread);
        else mod.status = 4;
    } else if (reason == DLL_PROCESS_DETACH) mod.signalStop();
    return TRUE;
}
#endif
