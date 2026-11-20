#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <bedrock/sdk.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <optional>
#include <set>
#include <tuple>

namespace {

class LifecycleProbe final : public bedrock::sdk::Mod {
public:
    LifecycleProbe(HANDLE ready, std::ofstream& log)
        : Mod("stage-a-lifecycle-probe"), ready_(ready), log_(log) {}

private:
    void onEnable() override { log_ << "onEnable\n" << std::flush; }
    void onDisable() override { log_ << "onDisable\n" << std::flush; }
    void onPlayerSpawn(bedrock::sdk::Player& player) override {
        log_ << "onPlayerSpawn (valid=" << player.isValid() << ")\n" << std::flush;
        spawned_ = player.isValid();
        if (!spawned_) return;

        const auto added = player.addEffect("minecraft:speed", 600.0,
            bedrock::sdk::EffectOptions{0, true});
        if (!added) {
            log_ << "effect add failed: " << added.error().message << "\n" << std::flush;
            return;
        }
        log_ << "effect add: minecraft:speed durationTicks=600 amplifier=0 showParticles=true\n" << std::flush;

        if (const auto effects = player.getEffects()) {
            bool found = false;
            for (const auto& effect : effects.value()) {
                if (effect.typeId == "minecraft:speed") {
                    found = true;
                    log_ << "effect read: typeId=" << effect.typeId
                         << " duration=" << effect.duration
                         << " durationAvailable=true\n" << std::flush;
                }
            }
            log_ << "effect read: minecraft:speed "
                 << (found ? "present=true\n" : "present=false\n") << std::flush;
        } else {
            log_ << "effect get failed: " << effects.error().message << "\n" << std::flush;
        }

        const auto removed = player.removeEffect("minecraft:speed");
        if (!removed) {
            log_ << "effect remove failed: " << removed.error().message << "\n" << std::flush;
            return;
        }
        log_ << "effect remove: minecraft:speed removed=" << removed.value() << "\n" << std::flush;

        if (const auto effects = player.getEffects()) {
            bool found = false;
            for (const auto& effect : effects.value())
                found |= effect.typeId == "minecraft:speed";
            log_ << "effect read after remove: minecraft:speed present="
                 << (found ? "true\n" : "false\n") << std::flush;
        } else {
            log_ << "effect get after remove failed: " << effects.error().message << "\n" << std::flush;
        }
    }
    void onUpdate() override {
        ++ticks_;

        if (spawned_ && ticks_ == 2) {
            log_ << "onUpdate (second tick with a local player)\n" << std::flush;
            SetEvent(ready_);
        }

        if (!spawned_ || (ticks_ % 20) != 0) return;
        const auto dimension = world().currentDimension();
        if (!dimension) {
            log_ << "item scan: dimension unavailable\n" << std::flush;
            return;
        }

        const auto entities = dimension->getEntities();
        if (!scanStart_) scanStart_ = std::chrono::steady_clock::now();
        for (const auto& entity : entities) {
            const auto typeId = entity.typeId();
            // <none> means the entity was enumerated but its type could not be
            // read. Keep it distinct so a later BOL run can tell enumeration
            // problems from type/stack problems.
            seenTypeIds_.insert(typeId ? *typeId : std::string{"<none>"});
            if (!typeId || *typeId != "minecraft:item") continue;

            const auto stack = entity.getItemStack();
            if (!stack) continue;
            const auto location = entity.location();
            std::tuple<std::string, double, double, double, std::string, unsigned> key{
                *typeId,
                location ? location->x : 0.0f,
                location ? location->y : 0.0f,
                location ? location->z : 0.0f,
                stack->typeId(), stack->amount()};
            if (!seenItemEntities_.insert(key).second) continue;

            log_ << "item entity: typeId=" << *typeId
                 << " itemTypeId=" << stack->typeId()
                 << " amount=" << stack->amount();
            if (location) {
                log_ << " position=" << location->x << ',' << location->y << ',' << location->z;
            } else {
                log_ << " position=unavailable";
            }
            log_ << '\n' << std::flush;
        }

        if (!diagnosticLogged_ && scanStart_ &&
            std::chrono::steady_clock::now() - *scanStart_ >= std::chrono::seconds(20)) {
            log_ << "item-detection-diagnostic: distinct typeIds=";
            bool first = true;
            for (const auto& id : seenTypeIds_) {
                if (!first) log_ << ',';
                first = false;
                log_ << id;
            }
            log_ << " entities=" << entities.size() << '\n' << std::flush;
            diagnosticLogged_ = true;
        }
    }

    HANDLE ready_;
    std::ofstream& log_;
    bool spawned_{};
    unsigned ticks_{};
    std::set<std::tuple<std::string, double, double, double, std::string, unsigned>> seenItemEntities_;
    std::set<std::string> seenTypeIds_;
    std::optional<std::chrono::steady_clock::time_point> scanStart_;
    bool diagnosticLogged_{};
};

DWORD WINAPI run(void* module) {
    try {
        wchar_t path[32768]{};
        const auto length = GetModuleFileNameW(static_cast<HMODULE>(module), path, ARRAYSIZE(path));
        if (!length || length >= ARRAYSIZE(path)) return 1;
        std::ofstream log(std::filesystem::path(path).parent_path() / "StageALifecycleProbe.log");
        if (!log) return 2;
        const HANDLE ready = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!ready) return 3;
        struct EventOwner {
            HANDLE handle;
            ~EventOwner() { CloseHandle(handle); }
        } event{ready};
        LifecycleProbe probe(ready, log);
        bedrock::sdk::ClientHost host;
        if (const auto started = host.start(probe); !started) {
            log << "start failed: " << started.error().message << '\n';
            return 4;
        }
        const auto wait = WaitForSingleObject(ready, 180'000);
        if (wait == WAIT_OBJECT_0) {
            log << "item scan window: holding 20s before stop\n" << std::flush;
            Sleep(20'000);
            log << "item scan window done\n" << std::flush;
        }
        // Stop from this worker, not from onUpdate or DLL process detach.
        if (const auto stopped = host.stop(); !stopped) {
            log << "stop failed: " << stopped.error().message << '\n';
            return 5;
        }
        log << (wait == WAIT_OBJECT_0 ? "stop succeeded after spawn and updates\n"
                                    : "stop succeeded but spawn/updates were not observed\n");
        return wait == WAIT_OBJECT_0 ? 0 : 6;
    } catch (...) {
        return 7;
    }
}
}

BOOL WINAPI DllMain(HINSTANCE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(module);
        if (const auto worker = CreateThread(nullptr, 0, run, module, 0, nullptr)) CloseHandle(worker);
    }
    return TRUE;
}
