#include "diet/traffic.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <vector>

namespace diet {
namespace {
constexpr std::uint64_t kMs = 1000000ull;

struct ClassRule {
    int id;
    PacketClass packetClass;
    SafetyClass safety;
};

// Protocol ids from the pinned 1.26.60 protocol roster
// (generated packet_ids.inc). Only ids whose handling is genuinely
// delay-tolerant in principle are marked DelayTolerant; everything not listed
// (and every id above 0x3FF) is Critical by rule.
constexpr ClassRule kRules[] = {
    // Session / handshake / disconnect class: never defer.
    {0, PacketClass::Session, SafetyClass::Critical},      // KeepAlive
    {2, PacketClass::Session, SafetyClass::Critical},      // PlayStatus
    {3, PacketClass::Session, SafetyClass::Critical},      // ServerToClientHandshake
    {5, PacketClass::Session, SafetyClass::Critical},      // Disconnect
    {85, PacketClass::Session, SafetyClass::Critical},     // Transfer
    {113, PacketClass::Session, SafetyClass::Critical},    // SetLocalPlayerAsInit
    {151, PacketClass::Session, SafetyClass::Critical},    // UpdatePlayerGameType
    {62, PacketClass::Session, SafetyClass::Critical},     // SetPlayerGameType
    {61, PacketClass::Session, SafetyClass::Critical},     // ChangeDimension
    {45, PacketClass::LifeCritical, SafetyClass::Critical}, // Respawn
    {42, PacketClass::LifeCritical, SafetyClass::Critical}, // SetHealth
    {38, PacketClass::LifeCritical, SafetyClass::Critical}, // HurtArmor
    {28, PacketClass::LifeCritical, SafetyClass::Critical}, // MobEffect
    {29, PacketClass::LifeCritical, SafetyClass::Critical}, // UpdateAttributes
    {41, PacketClass::LifeCritical, SafetyClass::Critical}, // SetActorLink
    {43, PacketClass::LifeCritical, SafetyClass::Critical}, // SetSpawnPosition
    {27, PacketClass::LifeCritical, SafetyClass::Critical}, // ActorEvent
    {44, PacketClass::LifeCritical, SafetyClass::Critical}, // Animate
    {30, PacketClass::Inventory, SafetyClass::Critical},    // InventoryTransaction
    {46, PacketClass::Inventory, SafetyClass::Critical},    // ContainerOpen
    {47, PacketClass::Inventory, SafetyClass::Critical},    // ContainerClose
    {48, PacketClass::Inventory, SafetyClass::Critical},    // PlayerHotbar
    {49, PacketClass::Inventory, SafetyClass::Critical},    // InventoryContent
    {50, PacketClass::Inventory, SafetyClass::Critical},    // InventorySlot
    {51, PacketClass::Inventory, SafetyClass::Critical},    // ContainerSetData
    {52, PacketClass::Inventory, SafetyClass::Critical},    // CraftingData
    {147, PacketClass::Inventory, SafetyClass::Critical},   // ItemStackRequest
    {148, PacketClass::Inventory, SafetyClass::Critical},   // ItemStackResponse
    {31, PacketClass::Inventory, SafetyClass::Critical},    // PlayerEquipment
    {32, PacketClass::Inventory, SafetyClass::Critical},    // MobArmorEquipment
    {12, PacketClass::World, SafetyClass::Critical},        // AddPlayer
    {63, PacketClass::World, SafetyClass::Critical},        // PlayerList
    {93, PacketClass::World, SafetyClass::Critical},        // PlayerSkin
    {19, PacketClass::EntityMove, SafetyClass::Critical},   // MovePlayer (local authority)
    {18, PacketClass::EntityMove, SafetyClass::Critical},   // MoveAbsoluteActor
    {39, PacketClass::EntityData, SafetyClass::Critical},   // SetActorData (flags/hurt time)
    {40, PacketClass::EntityMove, SafetyClass::Critical},   // SetActorMotion

    // Heavy but delay-tolerant in principle.
    {58, PacketClass::Chunk, SafetyClass::DelayTolerant},     // FullChunkData
    {174, PacketClass::Chunk, SafetyClass::DelayTolerant},    // SubChunkPacket
    {172, PacketClass::Chunk, SafetyClass::DelayTolerant},    // UpdateSubChunkBlocks
    {121, PacketClass::Chunk, SafetyClass::DelayTolerant},    // NetworkChunkPublisherUpdate
    {122, PacketClass::Chunk, SafetyClass::DelayTolerant},    // BiomeDefinitionList
    {21, PacketClass::BlockUpdate, SafetyClass::DelayTolerant},  // UpdateBlock
    {110, PacketClass::BlockUpdate, SafetyClass::DelayTolerant}, // UpdateBlockSynced
    {56, PacketClass::BlockUpdate, SafetyClass::DelayTolerant},  // BlockActorData
    {25, PacketClass::Effects, SafetyClass::DelayTolerant},   // LevelEvent
    {26, PacketClass::Effects, SafetyClass::DelayTolerant},   // TileEvent
    {123, PacketClass::Effects, SafetyClass::DelayTolerant},  // LevelSoundEvent
    {124, PacketClass::Effects, SafetyClass::DelayTolerant},  // LevelEventGeneric
    {118, PacketClass::Effects, SafetyClass::DelayTolerant},  // SpawnParticleEffect
    {111, PacketClass::EntityMove, SafetyClass::DelayTolerant}, // MoveDeltaActor
    {13, PacketClass::World, SafetyClass::DelayTolerant},     // AddActor
    {14, PacketClass::World, SafetyClass::DelayTolerant},     // RemoveActor
    {15, PacketClass::World, SafetyClass::DelayTolerant},     // AddItemActor
    {17, PacketClass::World, SafetyClass::DelayTolerant},     // TakeItemActor
    {66, PacketClass::World, SafetyClass::DelayTolerant},     // SpawnExperienceOrb
    {10, PacketClass::World, SafetyClass::DelayTolerant},     // SetTime
};

constexpr std::uint64_t kBurstSliceMs = 50;
} // namespace

PacketClassInfo classifyPacket(int rawId) noexcept {
    for (const auto& rule : kRules) {
        if (rule.id == rawId)
            return {rule.packetClass, rule.safety, packetClassName(rule.packetClass)};
    }
    return {PacketClass::Unknown, SafetyClass::Critical, "unknown"};
}

const char* packetClassName(PacketClass value) noexcept {
    switch (value) {
    case PacketClass::Session: return "session";
    case PacketClass::LifeCritical: return "life-critical";
    case PacketClass::Inventory: return "inventory";
    case PacketClass::Chunk: return "chunk";
    case PacketClass::BlockUpdate: return "block-update";
    case PacketClass::EntityMove: return "entity-move";
    case PacketClass::EntityData: return "entity-data";
    case PacketClass::Effects: return "effects";
    case PacketClass::World: return "world";
    case PacketClass::Other: return "other";
    default: return "unknown";
    }
}

bool mayDefer(int rawId) noexcept {
    // Rule: unknown ids are critical. Only an explicit delay-tolerant rule
    // returns true, and nothing in Diet consumes this yet (see evidence-02).
    return classifyPacket(rawId).safety == SafetyClass::DelayTolerant;
}

void TrafficAnalyzer::observePacket(std::uint64_t nowNs, int rawId, std::size_t bytes) noexcept {
    std::unique_lock lock(packetMutex_, std::try_to_lock);
    if (!lock.owns_lock()) {
        dropped_.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    auto& entry = packets_[packetNext_];
    entry.ns = nowNs;
    entry.rawId = rawId;
    entry.bytes = bytes;
    packetNext_ = (packetNext_ + 1) % kPacketRing;
    ++packetCount_;
}

void TrafficAnalyzer::observeFrame(std::uint64_t nowNs, double spikeThresholdMs) noexcept {
    std::unique_lock lock(frameMutex_, std::try_to_lock);
    if (!lock.owns_lock()) {
        dropped_.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    const std::uint64_t previous = lastFrameStampNs_;
    if (previous && nowNs > previous) {
        const std::uint64_t duration = nowNs - previous;
        lastFrameNs_.store(duration, std::memory_order_relaxed);
        auto& entry = frames_[frameNext_];
        entry.ns = nowNs;
        entry.durationNs = duration;
        frameNext_ = (frameNext_ + 1) % kFrameRing;
        ++frameCount_;
        if (spikeThresholdMs > 0 &&
            static_cast<double>(duration) / 1.0e6 >= spikeThresholdMs) {
            auto& spike = spikes_[spikeNext_];
            spike.startNs = previous;
            spike.endNs = nowNs;
            spike.durationNs = duration;
            spike.packetsInWindow = 0;
            spikeNext_ = (spikeNext_ + 1) % kSpikeRing;
            ++spikeCount_;
        }
    }
    lastFrameStampNs_ = nowNs;
}

void TrafficAnalyzer::collect(std::uint64_t nowNs, std::uint64_t windowMs, TrafficSnapshot& out,
                              TrafficRow* rows, std::size_t& rowCount) noexcept {
    rowCount = 0;
    const std::uint64_t span = windowMs * kMs;
    const std::uint64_t begin = nowNs > span ? nowNs - span : 0;

    struct Sample {
        std::uint64_t ns;
        std::uint64_t bytes;
        int rawId;
    };
    std::vector<Sample> window;
    {
        std::lock_guard lock(packetMutex_);
        window.reserve(std::min<std::size_t>(packetCount_, kPacketRing));
        for (const auto& entry : packets_) {
            if (!entry.ns || entry.ns < begin || entry.ns > nowNs) continue;
            window.push_back({entry.ns, entry.bytes, entry.rawId});
        }
    }
    std::sort(window.begin(), window.end(),
              [](const Sample& a, const Sample& b) { return a.ns < b.ns; });

    constexpr std::size_t classCount = static_cast<std::size_t>(PacketClass::Count);
    std::array<std::uint64_t, classCount> packets{};
    std::array<std::uint64_t, classCount> bytes{};
    std::array<std::uint64_t, classCount> tolerant{};
    std::array<std::uint64_t, classCount> burst{};
    for (const auto& sample : window) {
        const auto info = classifyPacket(sample.rawId);
        const auto index = static_cast<std::size_t>(info.packetClass);
        ++packets[index];
        bytes[index] += sample.bytes;
        if (info.safety == SafetyClass::DelayTolerant) ++tolerant[index];
        out.packets++;
        out.bytes += sample.bytes;
        if (info.safety == SafetyClass::DelayTolerant) {
            out.delayTolerantPackets++;
            out.delayTolerantBytes += sample.bytes;
        }
    }

    // Densest kBurstSliceMs window overall and per class (single sorted pass).
    const std::uint64_t slice = kBurstSliceMs * kMs;
    std::size_t windowStart = 0;
    std::array<std::uint64_t, classCount> sliceCounts{};
    for (std::size_t i = 0; i < window.size(); ++i) {
        const auto info = classifyPacket(window[i].rawId);
        ++sliceCounts[static_cast<std::size_t>(info.packetClass)];
        while (window[i].ns > window[windowStart].ns + slice) {
            const auto dropped = classifyPacket(window[windowStart].rawId);
            --sliceCounts[static_cast<std::size_t>(dropped.packetClass)];
            ++windowStart;
        }
        std::uint64_t total = 0;
        for (std::size_t c = 0; c < classCount; ++c) {
            burst[c] = std::max(burst[c], sliceCounts[c]);
            total += sliceCounts[c];
        }
        if (total > out.maxBurstPackets) {
            out.maxBurstPackets = total;
            out.maxBurstClass = info.packetClass;
            out.maxBurstMs = static_cast<double>(window[i].ns - window[windowStart].ns) / 1.0e6;
        }
    }

    {
        std::lock_guard lock(frameMutex_);
        std::vector<double> durations;
        for (const auto& entry : frames_) {
            if (!entry.ns || entry.ns < begin || entry.ns > nowNs) continue;
            durations.push_back(static_cast<double>(entry.durationNs) / 1.0e6);
            out.frames++;
            out.frameMsMax = std::max(out.frameMsMax, durations.back());
        }
        if (!durations.empty()) {
            std::sort(durations.begin(), durations.end());
            out.frameMsP50 = durations[durations.size() / 2];
            out.frameMsP95 = durations[std::min(durations.size() - 1,
                                                (durations.size() * 95) / 100)];
        }
        out.spikes = 0;
        for (const auto& spike : spikes_) {
            if (!spike.startNs || spike.startNs < begin || spike.startNs > nowNs) continue;
            ++out.spikes;
        }
    }
    out.windowSeconds = static_cast<double>(windowMs) / 1000.0;
    out.fps = out.windowSeconds > 0 ? static_cast<double>(out.frames) / out.windowSeconds : 0.0;
    out.droppedSamples = dropped_.load(std::memory_order_relaxed);

    rowCount = classCount;
    for (std::size_t c = 0; c < classCount; ++c) {
        rows[c].packetClass = static_cast<PacketClass>(c);
        rows[c].name = packetClassName(static_cast<PacketClass>(c));
        rows[c].packets = packets[c];
        rows[c].bytes = bytes[c];
        rows[c].delayTolerant = tolerant[c];
        rows[c].maxBurstPackets = burst[c];
    }
}

TrafficSnapshot TrafficAnalyzer::snapshot(std::uint64_t nowNs, std::uint64_t windowMs) {
    TrafficSnapshot out;
    TrafficRow rows[static_cast<std::size_t>(PacketClass::Count)]{};
    std::size_t rowCount = 0;
    collect(nowNs, windowMs, out, rows, rowCount);
    return out;
}

std::string TrafficAnalyzer::buildReport(std::uint64_t nowNs, std::uint64_t windowMs) {
    TrafficSnapshot out;
    TrafficRow rows[static_cast<std::size_t>(PacketClass::Count)]{};
    std::size_t rowCount = 0;
    collect(nowNs, windowMs, out, rows, rowCount);

    char line[512];
    std::string text;
    text.reserve(2048);
    std::snprintf(line, sizeof line,
                  "traffic window=%.1fs frames=%llu fps=%.1f frame_ms p50=%.2f p95=%.2f max=%.2f "
                  "spikes=%llu packets=%llu bytes=%llu burst=%llu/%.1fms dropped=%llu",
                  out.windowSeconds, static_cast<unsigned long long>(out.frames), out.fps,
                  out.frameMsP50, out.frameMsP95, out.frameMsMax,
                  static_cast<unsigned long long>(out.spikes),
                  static_cast<unsigned long long>(out.packets),
                  static_cast<unsigned long long>(out.bytes),
                  static_cast<unsigned long long>(out.maxBurstPackets), out.maxBurstMs,
                  static_cast<unsigned long long>(out.droppedSamples));
    text += line;
    text += '\n';
    for (std::size_t i = 0; i < rowCount; ++i) {
        if (!rows[i].packets) continue;
        std::snprintf(line, sizeof line,
                      "  class=%-13s packets=%llu bytes=%llu burst50ms=%llu defer_candidate=%llu\n",
                      rows[i].name, static_cast<unsigned long long>(rows[i].packets),
                      static_cast<unsigned long long>(rows[i].bytes),
                      static_cast<unsigned long long>(rows[i].maxBurstPackets),
                      static_cast<unsigned long long>(rows[i].delayTolerant));
        text += line;
    }
    std::snprintf(line, sizeof line,
                  "  defer_tolerant_total packets=%llu bytes=%llu (never deferred: no safe surface)\n",
                  static_cast<unsigned long long>(out.delayTolerantPackets),
                  static_cast<unsigned long long>(out.delayTolerantBytes));
    text += line;
    return text;
}

} // namespace diet
