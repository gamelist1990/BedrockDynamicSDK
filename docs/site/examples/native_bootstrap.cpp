#ifdef _WIN32
#include <bedrock/native_world.hpp>
#include <bedrock/native_client_commands.hpp>
#include <bedrock/game_hooks.hpp>
#include <atomic>
#include <chrono>
#include <deque>
#include <mutex>

// Keep this object alive for the entire DLL lifetime. Stop callbacks before
// destroying it. Command providers are pinned after native registration.
class ExampleMod {
    bedrock::GameApi game;
    bedrock::ProfileManager profiles;
    bedrock::NativeWorld world;
    bedrock::NativeClientCommands commands;
    std::unique_ptr<bedrock::GameHooks> hooks;
    std::atomic<void*> client{};
    std::mutex queueMutex;
    std::deque<std::optional<int>> requests;
    unsigned ticks{};
public:
    bool start(std::string& error) {
        auto definition=bedrock::CommandBuilder::named(bedrock::CommandName{"inspect"})
            .description(bedrock::CommandDescription{"ブロックを調べる"})
            .optionalInteger(bedrock::ArgumentName{"distance"})
            .handles([this](std::optional<int> distance)->bedrock::CommandDecision {
                if(distance&&(*distance<0||*distance>64))return bedrock::CommandDecision::Rejected;
                std::lock_guard lock(queueMutex);
                if(requests.size()>=16)return bedrock::CommandDecision::Rejected;
                requests.push_back(distance);
                return bedrock::CommandDecision::Accepted;
            });
        if(!commands.add(std::move(definition))){error="command definition rejected";return false;}
        const bedrock::ProfileRequirements required{
            bedrock::Feature::Gameplay,bedrock::Feature::LevelTick,bedrock::Feature::World,bedrock::Feature::ClientCommands,bedrock::Feature::WorldEvents};
        const bedrock::ProfileOptions options{
            bedrock::UpdatePolicy::RefreshIfNeeded,bedrock::CandidatePolicy::AllowCandidates};
        if(!profiles.prepare(required,options,[this](const bedrock::RuntimeProfile& profile,std::string& failure){
            return world.load(profile,failure)&&commands.load(profile,failure);
        },error))return false;
        game.Event.Enum.Gameplay.then([this](const bedrock::GameplayEvent& event){client.store(event.clientInstance);});
        game.Event.Enum.WorldChange.then([this](const bedrock::WorldChangeEvent&){
            commands.reset();std::lock_guard lock(queueMutex);requests.clear();
        });
        game.Event.Enum.LevelTick.then([this](const bedrock::LevelTickEvent& event){
            if(++ticks%20==0)commands.update(client.load());
            auto scope=world.tick(client.load(),event.level,bedrock::WorldEpoch{game.worldEpoch()},
                bedrock::TickBudget{64,std::chrono::microseconds{250}});
            if(!scope)return;
            std::optional<std::optional<int>> request;
            {std::lock_guard lock(queueMutex);if(!requests.empty()){request=requests.front();requests.pop_front();}}
            if(request){
                // Example coordinates only. Resolve a player-relative position
                // through Actor APIs in a real mod, then process copied values.
                const bedrock::BlockPos position{0,64+request->value_or(0),0};
                const bedrock::BlockResult block=world.getBlock(position,bedrock::ReadPolicy::Fresh);
                (void)block;
            }
        });
        hooks=std::make_unique<bedrock::GameHooks>(game);
        const auto selected=profiles.snapshot();
        const auto& resolver=selected->resolver();
        return hooks->installGameplay(resolver,error)&&hooks->installLevelTick(resolver,error)&&
            hooks->installWorldEvents(resolver,selected->json(),error);
    }
};
#endif
