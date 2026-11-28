#pragma once

#include <bedrock/sdk/world.hpp>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace bedrock::sdk::detail {

struct WorldSession {
    using Properties = std::unordered_map<std::string, DynamicProperty>;
    Properties worldProperties;
    std::unordered_map<std::uint64_t, Properties> entityProperties;

    mutable std::mutex stateMutex;
    std::optional<std::string> currentDimension;
    // Item registry / live item actors live in NativeGameBindings so packets
    // received before the first level tick are not dropped with a session.
    struct LocalPlayerState {
        float hunger{};
        std::vector<Effect> effects;
    };
    std::optional<std::uint64_t> localRuntimeId;
    LocalPlayerState localState;
};

template<class T = void>
Result<T> unavailable(std::string message) {
    return Result<T>::error(ErrorCode::Unavailable, std::move(message));
}

// Only the owning tick may resolve a public game object. No native handles or
// pointers are stored by those objects or exposed to SDK consumers.
class GameFrame {
public:
    virtual ~GameFrame() = default;
    const World* world{};
    std::shared_ptr<WorldSession> session;
    std::uint64_t dimensionRevision{};

    virtual std::optional<std::uint64_t> localPlayerId() const = 0;
    virtual const std::vector<std::uint64_t>& entityIds() = 0;
    virtual bool contains(std::uint64_t id) = 0;
    virtual bool isPlayer(std::uint64_t id) = 0;
    virtual std::optional<std::string> typeId(std::uint64_t id) = 0;
    virtual std::optional<std::string> name(std::uint64_t id) = 0;
    virtual std::optional<Vector3> location(std::uint64_t id) = 0;
    virtual std::optional<Rotation> rotation(std::uint64_t id) = 0;
    virtual std::optional<std::string> dimensionTypeId() const { return std::nullopt; }
    virtual std::optional<std::string> blockTypeId(BlockPosition) = 0;
    virtual std::optional<HealthComponent> health(std::uint64_t) const { return std::nullopt; }
    virtual std::optional<FoodComponent> food(std::uint64_t) const { return std::nullopt; }
    virtual std::optional<MovementComponent> movement(std::uint64_t) { return std::nullopt; }
    virtual std::optional<InventoryComponent> inventory(std::uint64_t) = 0;
    virtual std::optional<EquippableComponent> equippable(std::uint64_t) = 0;
    virtual std::optional<ItemStack> itemStack(std::uint64_t) const { return std::nullopt; }
    virtual Result<void> sendMessage(std::uint64_t, std::string_view) = 0;
    virtual Result<void> teleport(std::uint64_t, Vector3) {
        return unavailable("General teleport is not available in this client");
    }
    virtual Result<bool> addTag(std::uint64_t, std::string_view) {
        return unavailable<bool>("Native entity tags are not available in this client");
    }
    virtual Result<void> setBlockType(BlockPosition, std::string_view) {
        return unavailable("Direct block mutation is not available in this client");
    }
    virtual Result<Entity> spawnEntity(std::string_view, Vector3) {
        return unavailable<Entity>("Entity creation is not available in this client");
    }
    virtual Result<void> addEffect(std::uint64_t, std::string_view, double, EffectOptions) {
        return unavailable("Local effect application is not bound for this client");
    }
    virtual Result<bool> removeEffect(std::uint64_t, std::string_view) {
        return unavailable<bool>("Local effect removal is not bound for this client");
    }
    virtual Result<std::vector<Effect>> effects(std::uint64_t) const {
        return unavailable<std::vector<Effect>>("Local effect enumeration is not bound for this client");
    }
    virtual Result<void> clearEffects(std::uint64_t) {
        return unavailable("Local effect clearing is not bound for this client");
    }
protected:
    static EquippableComponent equipment(std::array<std::optional<ItemStack>, 6> slots) {
        return EquippableComponent{std::move(slots)};
    }
};

inline thread_local GameFrame* activeFrame{};
inline GameFrame* frameFor(const std::weak_ptr<WorldSession>& session) {
    return activeFrame && activeFrame->session && session.lock() == activeFrame->session
        ? activeFrame : nullptr;
}
inline GameFrame* frameFor(const World* world) {
    return activeFrame && activeFrame->session && activeFrame->world == world
        ? activeFrame : nullptr;
}

std::optional<DynamicProperty> getProperty(const WorldSession::Properties&, std::string_view key);
Result<void> setProperty(WorldSession::Properties&, std::string_view key,
                         std::optional<DynamicProperty> value);

class GameFrameScope {
public:
    explicit GameFrameScope(GameFrame& frame) : previous_(std::exchange(activeFrame, &frame)) {}
    ~GameFrameScope() { activeFrame = previous_; }
    GameFrameScope(const GameFrameScope&) = delete;
    GameFrameScope& operator=(const GameFrameScope&) = delete;
private:
    GameFrame* previous_;
};

} // namespace bedrock::sdk::detail
