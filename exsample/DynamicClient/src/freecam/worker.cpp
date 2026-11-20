#include "worker.hpp"
#include <fstream>
#include <thread>

namespace dynamic_client::freecam_detail {
void worker(const std::filesystem::path &directory) {
    std::ofstream observations;

    std::uint64_t generation{};
    Clock::time_point nextWaitingLog{}, nextPacketLog{};
    constexpr std::array<std::array<unsigned, 2>, 12> edges{{{0, 1},
                                                             {1, 3},
                                                             {3, 2},
                                                             {2, 0},
                                                             {4, 5},
                                                             {5, 7},
                                                             {7, 6},
                                                             {6, 4},
                                                             {0, 4},
                                                             {1, 5},
                                                             {2, 6},
                                                             {3, 7}}};
    while (!stop.load()) {
        const auto now = Clock::now();
        bedrock::CameraVisibilitySample sample;
        bool observed = false;
        if (const auto taken = cameraVisual.takeSample()) {
            sample = *taken;
            observed = true;
        }
        if (config.load()->visibilityDiagnostics && observed) {
            if (!observations.is_open())
                observations.open(directory / L"DynamicClient-Freecam-visibility.jsonl", std::ios::app);
            if (observations) {
                auto vector = [](Vec v) { return nlohmann::json::array({v.x, v.y, v.z}); };
                observations << nlohmann::json{{"schema", 1},
                                               {"build", profileGameVersion},
                                               {"visual_revision", visualRevision},
                                               {"exe_sha256", profileExeSha},
                                               {"monotonic_ms", sample.milliseconds},
                                               {"epoch", sample.epoch},
                                               {"freecam", sample.flying},
                                               {"actor_game_type", sample.gameType},
                                               {"valid_camera_read", sample.valid},
                                               {"candidate_camera_654", vector(sample.cameraPosition)},
                                               {"render_origin", vector(sample.origin)},
                                               {"raw_camera_970_976", sample.rawFlags},
                                               {"culler_type", sample.cullerType},
                                               {"culler_vtable_rva", sample.cullerVtable},
                                               {"camera_view_flags_valid", sample.viewValid},
                                               {"raw_view_flags_7c_88", sample.viewFlags},
                                               {"draw_inside_cubes",
                                                sample.drawInsideCubes
                                                    ? nlohmann::json(*sample.drawInsideCubes)
                                                    : nlohmann::json(nullptr)}}
                                    .dump()
                             << '\n';
                observations.flush();
            }
        } else if (!config.load()->visibilityDiagnostics && observations.is_open())
            observations.close();
        if (config.load()->enabled && !packetReady.load())
            installPacketSource();
        const auto e = epoch.load();
        if (generation != e) {
            generation = e;
            mesh.store(nullptr);
        }
        Capture batch;
        bool have = false;
        {
            std::lock_guard lock(mailboxMutex);
            if (mailboxReady) {
                batch = mailbox;
                mailboxReady = false;
                have = true;
            }
        }
        if (have && batch.epoch == generation && active.load()) {
            const auto c = config.load();
            auto result = std::make_shared<Mesh>();
            result->epoch = generation;
            result->made = batch.made;
            unsigned boxes = 0;
            for (unsigned i = 0; i < batch.count && boxes < c->maxPlayers; ++i) {
                const auto &box = batch.boxes[i];
                const auto &b = box.bounds;
                const auto &from = box.blendFrom;
                const float x = (b.minimum.x + b.maximum.x) * 0.5f - batch.origin.x,
                            y = (b.minimum.y + b.maximum.y) * 0.5f - batch.origin.y,
                            z = (b.minimum.z + b.maximum.z) * 0.5f - batch.origin.z;
                if (x * x + y * y + z * z > c->radius * c->radius)
                    continue;
                for (const auto &edge : edges)
                    for (unsigned corner : edge) {
                        result->fromVertices[result->count] = {
                            (corner & 1) ? from.maximum.x : from.minimum.x,
                            (corner & 2) ? from.maximum.y : from.minimum.y,
                            (corner & 4) ? from.maximum.z : from.minimum.z};
                        result->vertices[result->count++] = {
                            (corner & 1) ? b.maximum.x : b.minimum.x,
                            (corner & 2) ? b.maximum.y : b.minimum.y,
                            (corner & 4) ? b.maximum.z : b.minimum.z};
                    }
                ++boxes;
            }
            if (active.load() && generation == epoch.load())
                mesh.store(result);
        }
        if (!active.load())
            mesh.store(nullptr);
        const auto visualPeek = cameraVisual.statistics();
        if ((active.load() || interactionPackets.load() || blockedPackets.load() ||
             visualPeek.terrainPending || visualPeek.terrainRestores || visualPeek.terrainFaults) &&
            now >= nextPacketLog) {
            nextPacketLog = now + std::chrono::seconds(5);
            const auto visualStats = cameraVisual.takeStatistics();
            log("Freecam spectator culler: ready=" + std::to_string(cameraVisual.terrainReady()) +
                " mode=" + std::to_string(visualStats.terrainObservedMode) +
                " pending=" + std::to_string(visualStats.terrainPending) +
                " calls=" + std::to_string(visualStats.terrainCalls) +
                " changed=" + std::to_string(visualStats.terrainChanges) +
                " restored=" + std::to_string(visualStats.terrainRestores) +
                " faults=" + std::to_string(visualStats.terrainFaults) +
                " native_requests=" + std::to_string(visualStats.selectorRequests) +
                " rewritten=" + std::to_string(visualStats.selectorRewrites) +
                " caller_rva=" + std::to_string(visualStats.selectorCaller) +
                " churn_guard=" + std::to_string(visualStats.terrainRejected) +
                " [native selector mode5; underground view needs live confirmation]");
            log("Freecam view factory: ready=" + std::to_string(cameraVisual.createViewReady()) +
                " created=" + std::to_string(visualStats.viewCreated) +
                " changed=" + std::to_string(visualStats.viewChanged) +
                " failed=" + std::to_string(visualStats.viewWriteFailed) +
                " camera_calls=" + std::to_string(visualStats.cameraRenderCalls) +
                " [fresh output only; 5s; visual_revision=" + std::to_string(visualRevision) + "]");
            const auto effectStats = insideBlockEffect.takeStatistics();
            log("Freecam RenderDragon inside effect: ready=" +
                std::to_string(insideBlockEffect.ready()) +
                " calls=" + std::to_string(effectStats.calls) +
                " eligible=" + std::to_string(effectStats.eligible) +
                " skipped=" + std::to_string(effectStats.skipped) +
                " other_caller=" + std::to_string(effectStats.otherCaller) +
                " caller_rva=" + std::to_string(effectStats.callerRva) +
                " [SDK tag/caller pairs; 5s; visual_revision=" + std::to_string(visualRevision) +
                "]");
            log("Freecam packet flow: anchored=" + std::to_string(anchoredPackets.exchange(0)) +
                " item_inputs=" + std::to_string(interactionPackets.exchange(0)) +
                " rejected=" + std::to_string(blockedPackets.exchange(0)));
        }
        const auto bits = logBits.exchange(0);
        if (bits & Activated) {
            log("Freecam active [Borion local Player proxy; packet positions anchored; "
                "interactions forwarded; Player boxes: every tick + 50ms interpolation]");
            const auto c = config.load();
            log(std::string("Freecam view: spectator_mode=") + (c->spectatorMode ? "on" : "off") +
                " spectator_culler=" + (cameraVisual.terrainReady() ? "mode5 ready" : "unavailable") +
                " visual_revision=" + std::to_string(visualRevision));
        }
        if (bits & Waiting && now >= nextWaitingLog) {
            log("Freecam waiting: sender hook / fresh exact-build movement packet pose required "
                "before native writes");
            nextWaitingLog = now + std::chrono::seconds(5);
        }
        if (bits & Disabled)
            log("Freecam disabled");
        if (bits & Damage)
            log("Freecam disabled: local Player hurt");
        if (bits & ReadFailed)
            log("Freecam disabled: native Player / movement packet validation failed");
        if (bits & World)
            log("world changed: Freecam and Player boxes reset");
        if (bits & RenderFault)
            log("Freecam disabled: native render fault");
        std::this_thread::sleep_for(std::chrono::milliseconds(config.load()->enabled ? 20 : 100));
    }
}
}
