#ifdef _WIN32
#include <bedrock/native_command_service.hpp>
#include <bedrock/native_safe_teleport.hpp>
#include <bedrock/game_hooks.hpp>
#include <atomic>

// Own this object for the DLL lifetime. Definitions precede hooks; native
// registration pins the provider. Stop callbacks before destroying the object.
class ExampleMod {
    bedrock::GameApi game;
    bedrock::NativeCommandService commands;
    bedrock::NativeSafeTeleport teleport;
    std::unique_ptr<bedrock::GameHooks> hooks;
    std::atomic<void*> client{};
    std::atomic<std::uint64_t> epoch{1};
public:
    bool start(std::string& error) {
        if (!commands.add(bedrock::CommandBuilder(bedrock::CommandName{"top"}, bedrock::ArgumentName{"blocks"})
            .description(bedrock::CommandDescription{"安全な上方移動"})
            .then([this](std::optional<int> blocks) -> bedrock::CommandDecision {
                return teleport.request(blocks);
            }))) { error = "command definition rejected"; return false; }
        // A real mod should localize TeleportResult::error via onResult() and
        // commands.onRejected(), and use teleport.message() within tick scope.
        bedrock::ProfileManager profiles;
        const bedrock::ProfileRequirements required{
            bedrock::Feature::Gameplay, bedrock::Feature::LevelTick, bedrock::Feature::World,
            bedrock::Feature::Motion, bedrock::Feature::WorldHeight, bedrock::Feature::Gui,
            bedrock::Feature::ClientCommands, bedrock::Feature::CommandPackets,
            bedrock::Feature::PacketSender, bedrock::Feature::WorldEvents};
        const bedrock::ProfileOptions options{bedrock::UpdatePolicy::RefreshIfNeeded, bedrock::CandidatePolicy::AllowCandidates};
        if (!profiles.prepare(required, options, [this](const bedrock::RuntimeProfile& profile, std::string& failure) {
            return teleport.load(profile, failure) && commands.load(profile, failure);
        }, error)) return false;
        game.Event.Enum.Gameplay.then([this](const bedrock::GameplayEvent& event) { client.store(event.clientInstance); });
        game.Event.Enum.WorldChange.then([this](const bedrock::WorldChangeEvent&) {
            const auto current = ++epoch;
            commands.reset(current); teleport.reset(current);
        });
        game.Event.Enum.LevelTick.then([this](const bedrock::LevelTickEvent& event) {
            const auto current = epoch.load();
            auto scope = teleport.tick(client.load(), event.level, current);
            if (!scope) return;
            commands.update(client.load(), current);
            commands.dispatch(); // typed handlers execute here, never in packet callbacks
        });
        hooks = std::make_unique<bedrock::GameHooks>(game);
        const auto selected = profiles.snapshot();
        return hooks->installGameplay(selected->resolver(), error) && hooks->installLevelTick(selected->resolver(), error) &&
            hooks->installWorldEvents(selected->resolver(), selected->json(), error);
    }
};
#endif
