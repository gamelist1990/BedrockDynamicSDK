#include "bedrock/sdk/movement_packets.hpp"

#include <bedrock/packets/movement_packets.hpp>
#include <bedrock/packets/wire_packet.hpp>

#include <string>

namespace bedrock::sdk {

// The movement codec is header-only and free of native object layouts, so the
// same implementation serves the Windows build and the host test build.
struct MovementPackets::Impl {
    bedrock::MovementPackets native;
};

MovementPackets::MovementPackets() : impl_(std::make_unique<Impl>()) {}
MovementPackets::~MovementPackets() = default;
MovementPackets::MovementPackets(MovementPackets&&) noexcept = default;
MovementPackets& MovementPackets::operator=(MovementPackets&&) noexcept = default;

bool MovementPackets::handles(int packetId) const noexcept {
    return bedrock::MovementPackets::handles(static_cast<bedrock::PacketId>(packetId));
}

std::optional<MovementPacketPose> MovementPackets::pose(const std::string& packetBytes,
                                                        int packetId) const noexcept {
    try {
        const bedrock::WirePacket packet(packetBytes);
        if (static_cast<int>(packet.id()) != packetId) return std::nullopt;
        bedrock::MovementPacketPose nativePose;
        if (!impl_->native.pose(packet, nativePose)) return std::nullopt;
        MovementPacketPose result;
        result.runtimeId = nativePose.runtimeId;
        result.onGround = nativePose.onGround;
        result.position_x = nativePose.position.x;
        result.position_y = nativePose.position.y;
        result.position_z = nativePose.position.z;
        return result;
    } catch (...) {
        return std::nullopt;
    }
}

MovementPacketEdit MovementPackets::anchor(std::string& packetBytes, int packetId,
                                           const MovementPacketPose& pose,
                                           std::uint64_t runtime) const noexcept {
    try {
        bedrock::WirePacket packet(packetBytes);
        if (static_cast<int>(packet.id()) != packetId) return {};
        bedrock::MovementPacketPose nativePose;
        nativePose.runtimeId = pose.runtimeId;
        nativePose.onGround = pose.onGround;
        nativePose.position = {pose.position_x, pose.position_y, pose.position_z};
        const auto result = impl_->native.anchor(packet, nativePose, runtime);
        MovementPacketEdit edit;
        edit.applied = result.applied;
        edit.interaction = result.interaction;
        // Return the complete frame: the caller hands these bytes back to the
        // packet bridge, which needs the original header preserved.
        if (result.applied) packetBytes = packet.bytes();
        return edit;
    } catch (...) {
        return {};
    }
}

} // namespace bedrock::sdk
