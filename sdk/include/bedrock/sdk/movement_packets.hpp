#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>

namespace bedrock::sdk {

// Value copy of the pose carried by an outgoing movement packet. The byte
// offsets and the wire schema are owned by the SDK; callers only ever see
// this copy, so no native packet object crosses the facade boundary.
struct MovementPacketPose {
    std::uint64_t runtimeId{};
    std::uint8_t onGround{};
    float position_x{}, position_y{}, position_z{};
};

// Outcome of anchoring a movement packet to a previously observed pose.
// interaction=true means the packet also carried interaction input flags that
// the caller must keep forwarding.
struct MovementPacketEdit {
    bool applied{};
    bool interaction{};
};

// --------------------------------------------------------------------------
// MovementPackets — public facade over the SDK's movement wire codec.
// Freecam uses it to observe outgoing movement poses and to anchor outgoing
// position updates to the last observed pose. Both operations are pure wire
// edits on a copied frame; the game keeps ownership of the packet.
// --------------------------------------------------------------------------
class MovementPackets final {
public:
    MovementPackets();
    ~MovementPackets();
    MovementPackets(const MovementPackets&) = delete;
    MovementPackets& operator=(const MovementPackets&) = delete;
    MovementPackets(MovementPackets&&) noexcept;
    MovementPackets& operator=(MovementPackets&&) noexcept;

    // True when packetId is a movement packet (MovePlayer / PlayerAuthInput).
    bool handles(int packetId) const noexcept;

    // True when packetId is PlayerAuthInput (the auth-input movement kind;
    // freecam slots observed poses per kind).
    bool isAuthInput(int packetId) const noexcept;

    // Extract the pose from a complete wire frame (header + payload). Returns
    // nullopt when the frame is malformed, is not a movement packet, or does
    // not match the imported movement schema.
    std::optional<MovementPacketPose> pose(const std::string& packetBytes,
                                          int packetId) const noexcept;

    // Anchor a movement frame's position (and, for PlayerAuthInput, its
    // movement inputs) to `pose`. `packetBytes` is rewritten in place and keeps
    // its original header, conditional transactions and block actions.
    MovementPacketEdit anchor(std::string& packetBytes, int packetId,
                              const MovementPacketPose& pose,
                              std::uint64_t runtime) const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace bedrock::sdk
