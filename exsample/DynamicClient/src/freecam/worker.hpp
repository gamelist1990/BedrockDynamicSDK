#pragma once
#include "freecam/config.hpp"
#include <bedrock/sdk/camera_control.hpp>
#include <array>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>

namespace dynamic_client::freecam_detail {
using Vec = bedrock::sdk::Vec3Value;
using Clock = std::chrono::steady_clock;
using Capture = bedrock::sdk::PlayerBoundsFrame;
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
extern bedrock::sdk::CameraControl cameraControl;
extern std::string profileGameVersion, profileExeSha;
extern unsigned visualRevision;
void log(std::string_view text);
bool installPacketSource();
void worker(const std::filesystem::path& directory);
}
