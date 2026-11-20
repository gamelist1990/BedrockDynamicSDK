#pragma once
#include "freecam/config.hpp"
#include <bedrock/native/camera_visual.hpp>
#include <bedrock/native/inside_block_effect.hpp>
#include <bedrock/native/player_bounds_tracker.hpp>
#include <array>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>

namespace dynamic_client::freecam_detail {
using Vec = bedrock::Vec3;
using Clock = std::chrono::steady_clock;
using Capture=bedrock::ActorBoundsFrame;
struct Mesh {
    std::uint64_t epoch{};
    Clock::time_point made{};
    unsigned count{};
    std::array<Vec, 128 * 24> vertices{}, fromVertices{};
};
enum : unsigned {
    Activated = 1,
    Disabled = 2,
    Damage = 4,
    ReadFailed = 8,
    World = 16,
    RenderFault = 32,
    Waiting = 64
};
extern std::atomic<bool> stop, active, packetReady;
extern std::atomic<unsigned> interactionPackets, blockedPackets, anchoredPackets, logBits;
extern std::atomic<std::uint64_t> epoch;
extern std::atomic<std::shared_ptr<const freecam::Config>> config;
extern std::atomic<std::shared_ptr<const Mesh>> mesh;
extern std::mutex mailboxMutex;
extern Capture mailbox;
extern bool mailboxReady;
extern bedrock::NativeCameraVisual cameraVisual;
extern bedrock::NativeInsideBlockEffect insideBlockEffect;
extern std::string profileGameVersion, profileExeSha;
extern unsigned visualRevision;
void log(std::string_view text);
bool installPacketSource();
void worker(const std::filesystem::path& directory);
}
