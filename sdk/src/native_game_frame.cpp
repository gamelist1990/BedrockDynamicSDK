#include "native_game_frame.hpp"
#include "item_packet_decode.hpp"

#include <bedrock/detail/actor_identity.hpp>
#include <bedrock/detail/block_types.hpp>
#include <bedrock/detail/effect_types.hpp>
#include <bedrock/detail/native_profile_functions.hpp>
#include <bedrock/enums/packet_id.hpp>
#include <bedrock/enums/world.hpp>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>

// ---------------------------------------------------------------------------
// MobEffectsComponent access (layout and game functions identified by
// targeted Ghidra decompiles and verified live on BOL, 2026-11-21). Effects
// used to be tracked in a session-local mirror fed by MobEffect packets;
// entries written through addEffect never reached the game, so nothing
// physically applied (no lift, no HUD icon). Writing the component's slots
// directly is not enough either: the game only ticks instances that went
// through its own add path (vector growth, attribute modifiers, events).
// So writes call the game's own functions, resolved from the profile
// (MobEffect::add / MobEffect::remove), with an instance built from the
// game's default-instance template (MobEffect::instanceTemplate) patched
// with the requested id / duration / amplifier / showParticles. When the
// profile does not resolve them, writes fall back to the mirror.
//
// The component (EnTT type hash 0xE6A1B550 = FNV-1a32 of the component name,
// the same scheme as the profile's other component hashes) begins with a
// std::vector of per-effect slots, one slot per effect id; the game grows
// it to (highest applied id + 1) slots on apply. Slot stride is 0xA8.
// Instance fields inside a slot (everything else is static per-effect
// definition data owned by the game), used for readback:
//   +0x00 int32  effect id (0 = inactive)
//   +0x04 int32  duration remaining, in ticks (the game counts it down)
//   +0x20 int32  amplifier
//   +0x43 bool   showParticles while active
// ---------------------------------------------------------------------------
namespace {
constexpr std::uint32_t kMobEffectsComponentHash = 0xE6A1B550u;
constexpr std::size_t kEffectSlotStride = 0xA8;

bool readGameMemory(const void* src, void* dst, std::size_t size) {
    // Memory-misuse guard: VirtualQuery range validation + SEH second line
    // of defence. Range must be committed & readable; access is wrapped in
    // __try/__except (SEH code lives in small functions with POD-only
    // parameters/locals per MSVC C2712).
    auto [validBase, validBytes] = bedrock::detail::validateReadRange(src, size);
    if (!validBase || validBytes < size) return false;
    __try {
        std::memcpy(dst, src, size);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool callGameAddEffect(void* fn, void* actor, void* instance) {
    __try {
        reinterpret_cast<void(__fastcall*)(void*, void*)>(fn)(actor, instance);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool callGameRemoveEffect(void* fn, void* actor, int effectId) {
    __try {
        reinterpret_cast<void(__fastcall*)(void*, int)>(fn)(actor, effectId);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

struct MobEffectsSlots {
    std::uint64_t begin{};
    std::uint64_t end{};
    [[nodiscard]] std::size_t count() const {
        return static_cast<std::size_t>((end - begin) / kEffectSlotStride);
    }
    [[nodiscard]] void* slotAddress(std::size_t index) const {
        return reinterpret_cast<void*>(begin + index * kEffectSlotStride);
    }
};

void* mobEffectsComponent(const bedrock::NativeActorComponents& components, void* actor) {
    if (!actor) return nullptr;
    return components.component(actor, kMobEffectsComponentHash);
}

std::optional<MobEffectsSlots> mobEffectsSlots(
    const bedrock::NativeActorComponents& components, void* actor) {
    void* component = mobEffectsComponent(components, actor);
    if (!component) return std::nullopt;
    std::uint64_t head[3]{};
    if (!readGameMemory(component, head, sizeof(head))) return std::nullopt;
    if (!head[0] || head[1] < head[0] || head[2] < head[1] ||
        (head[1] - head[0]) % kEffectSlotStride != 0)
        return std::nullopt;
    return MobEffectsSlots{head[0], head[1]};
}

bool readEffectSlot(const MobEffectsSlots& slots, std::size_t index,
                    unsigned char (&slot)[kEffectSlotStride]) {
    return readGameMemory(slots.slotAddress(index), slot, kEffectSlotStride);
}

std::int32_t slotInt(const unsigned char* slot, std::size_t offset) {
    std::int32_t value{};
    std::memcpy(&value, slot + offset, sizeof(value));
    return value;
}

void putSlotInt(unsigned char* slot, std::size_t offset, std::int32_t value) {
    std::memcpy(slot + offset, &value, sizeof(value));
}

} // namespace

namespace bedrock::sdk::detail {
namespace {
Vector3 copyVector(Vec3 value) { return {value.x, value.y, value.z}; }
std::optional<ItemStack> copyItem(const std::optional<bedrock::ItemStack>& item) {
    if (!item || item->identifier.empty() || !item->count) return std::nullopt;
    return ItemStack{item->identifier, item->count};
}

std::string dimensionName(int id) {
    switch (id) {
    case 0: return "minecraft:overworld";
    case 1: return "minecraft:nether";
    case 2: return "minecraft:the_end";
    default: return {};
    }
}

std::optional<std::string> parseDimensionValue(const nlohmann::json& value) {
    if (value.is_string()) {
        const auto& name = value.get<std::string>();
        if (name == "overworld") return std::string{"minecraft:overworld"};
        if (name == "nether") return std::string{"minecraft:nether"};
        if (name == "end") return std::string{"minecraft:the_end"};
    } else if (value.is_number_integer()) {
        const auto named = dimensionName(value.get<int>());
        if (!named.empty()) return std::string{named};
    }
    return std::nullopt;
}

Result<void> blockErrorResult(bedrock::BlockError error, std::string_view context) {
    switch (error) {
    case bedrock::BlockError::none:
        return Result<void>::ok();
    case bedrock::BlockError::unsupported:
        return detail::unavailable(std::string(context) + " is not supported in this client");
    case bedrock::BlockError::wrongThread:
        return Result<void>::error(ErrorCode::Internal, std::string(context) + " called outside the owning tick");
    case bedrock::BlockError::staleWorld:
        return detail::unavailable(std::string(context) + " lost the active world");
    case bedrock::BlockError::budgetExceeded:
        return detail::unavailable(std::string(context) + " exceeded the native tick budget");
    case bedrock::BlockError::unavailable:
        return detail::unavailable(std::string(context) + " native state is unavailable");
    case bedrock::BlockError::invalidRequest:
        return Result<void>::error(ErrorCode::Invalid, std::string(context) + " request is invalid");
    case bedrock::BlockError::missingItem:
        return Result<void>::error(ErrorCode::NotFound, std::string(context) + " requires the item in the hotbar");
    case bedrock::BlockError::stateMismatch:
        return Result<void>::error(ErrorCode::Invalid, std::string(context) + " placement state did not match");
    case bedrock::BlockError::nativeRejected:
        return Result<void>::error(ErrorCode::Unavailable, std::string(context) + " was rejected by the game");
    }
    return detail::unavailable(std::string(context) + " failed");
}

std::shared_ptr<WorldSession> lockedSession(std::atomic<std::shared_ptr<WorldSession>>& atomic) {
    return atomic.load(std::memory_order_acquire);
}
} // namespace

bool NativeGameBindings::load(const RuntimeProfile& profile, std::string& error) {
    if (!access.load(profile, error) || !parseNativeActorLayout(profile.json(), actors, error))
        return false;
    access.setDisplayNamesEnabled(false);
    const auto root = parseNativeProfile(profile.json());
    components.configure(actors, root);
    // Best-effort: the game's own MobEffect add/remove functions and its
    // default-instance template. Missing or unverifiable symbols never fail
    // the load; effect writes then keep the packet-mirror behaviour.
    try {
        const auto& symbols = root.at("symbols");
        if (const auto it = symbols.find("MobEffect::add");
            it != symbols.end() && it->contains("rva") && it->contains("entry_check"))
            effectAddFn = native_profile::checked(root, *it);
        if (const auto it = symbols.find("MobEffect::remove");
            it != symbols.end() && it->contains("rva") && it->contains("entry_check"))
            effectRemoveFn = native_profile::checked(root, *it);
        if (const auto it = symbols.find("MobEffect::instanceTemplate");
            it != symbols.end() && it->contains("rva")) {
            const auto rva = native_profile::number(it->at("rva"));
            effectInstanceTemplate = reinterpret_cast<const void*>(
                reinterpret_cast<std::uintptr_t>(::GetModuleHandleW(nullptr)) + rva);
        }
    } catch (const std::exception&) {
        effectAddFn = nullptr;
        effectRemoveFn = nullptr;
        effectInstanceTemplate = nullptr;
    }
    const auto& type = root.at("native_actors").at("actor_type");
    if (native_profile::number(type.at("value_size")) == sizeof(std::uint32_t)) {
        typeOffset = native_profile::number(type.at("value_offset"));
        if (typeOffset <= 4096) playerType = native_profile::number(type.at("player_type_id"));
    }
    if (profile.supports({Feature::Motion}, error))
        motion = native_motion::Layout::fromProfile(profile.json());
    terrainReady = terrain.load(profile, error);
    guiReady = gui.load(profile, error);
    error.clear();
    return true;
}

bool NativeGameBindings::installPacketReceive(const RuntimeProfile& profile, std::string& error) {
    if (packetReceiveReady) return true;
    const auto add = [&](bedrock::PacketId id) {
        packets.onReceive(id, [this](const bedrock::PacketReadEvent& event) { observePacket(event); });
    };
    add(bedrock::PacketId::SetHealth);
    add(bedrock::PacketId::UpdateAttributes);
    add(bedrock::PacketId::MobEffect);
    add(bedrock::PacketId::AddItemActor);
    add(bedrock::PacketId::TakeItemActor);
    add(bedrock::PacketId::ItemRegistryPacket);
    add(bedrock::PacketId::ChangeDimension);
    add(bedrock::PacketId::StartGame);
    add(bedrock::PacketId::SetSpawnPosition);
    if (!packets.installReceive(profile.json(), error)) return false;
    packetReceiveReady = true;
    error.clear();
    return true;
}

void NativeGameBindings::observePacket(const bedrock::PacketReadEvent& event) {
    try {
        switch (event.id()) {
        case bedrock::PacketId::SetHealth:
            onSetHealth(codec.decode(event.id(), event.payload()));
            break;
        case bedrock::PacketId::UpdateAttributes:
            onUpdateAttributes(codec.decode(event.id(), event.payload()));
            break;
        case bedrock::PacketId::MobEffect:
            onMobEffect(codec.decode(event.id(), event.payload()));
            break;
        case bedrock::PacketId::AddItemActor:
            // ItemV4.extra is an `encapsulated` native type, so the generic
            // ProtocolCodec cannot decode this packet at all. The dedicated
            // decoder reads runtime ID / network ID / count and skips NBT.
            onAddItemEntity(event.payload());
            break;
        case bedrock::PacketId::TakeItemActor:
            onTakeItemEntity(codec.decode(event.id(), event.payload()));
            break;
        case bedrock::PacketId::ItemRegistryPacket:
            // This packet has exactly one top-level field (itemstates), whose
            // entries end in an `nbt` native type. decodePrefix(.., 2) always
            // threw "invalid protocol prefix"; use the dedicated decoder.
            onItemRegistry(event.payload());
            break;
        case bedrock::PacketId::ChangeDimension:
            onDimensionPacket(codec.decode(event.id(), event.payload()), "dimension");
            break;
        case bedrock::PacketId::StartGame: {
            auto prefix = codec.decodePrefix(event.id(), event.payload(), 9);
            onDimensionPacket(std::move(prefix), "dimension");
            break;
        }
        case bedrock::PacketId::SetSpawnPosition: {
            auto prefix = codec.decodePrefix(event.id(), event.payload(), 3);
            onDimensionPacket(std::move(prefix), "dimension");
            break;
        }
        default:
            break;
        }
    } catch (...) {
        // Observation must never interrupt the game's packet processing.
    }
}

void NativeGameBindings::onSetHealth(const nlohmann::json& payload) {
    const auto health = payload.value("health", 0);
    if (health <= 0) return;
    auto session = lockedSession(currentSession);
    if (!session) return;
    std::lock_guard lock(session->stateMutex);
    session->localState.health = static_cast<float>(health);
}

void NativeGameBindings::onUpdateAttributes(const nlohmann::json& payload) {
    auto session = lockedSession(currentSession);
    if (!session) return;
    const auto attributesIt = payload.find("attributes");
    if (attributesIt == payload.end() || !attributesIt->is_array()) return;
    std::lock_guard lock(session->stateMutex);
    for (const auto& attr : *attributesIt) {
        if (!attr.is_object()) continue;
        const auto name = attr.value("name", std::string{});
        const auto current = attr.value("current", 0.0f);
        if (name == "minecraft:health") session->localState.health = current;
        else if (name == "minecraft:player.hunger" || name == "minecraft:hunger")
            session->localState.hunger = current;
    }
}

void NativeGameBindings::onMobEffect(const nlohmann::json& payload) {
    auto session = lockedSession(currentSession);
    if (!session) return;
    const auto runtimeId = payload.value("runtime_entity_id", std::uint64_t{});
    const auto eventId = payload.value("event_id", std::string{});
    const auto effectId = payload.value("effect_id", 0);
    const auto duration = payload.value("duration", 0);
    const auto amplifier = payload.value("amplifier", 0);
    const auto particles = payload.value("particles", true);
    std::lock_guard lock(session->stateMutex);
    if (runtimeId == 0 || runtimeId != session->localRuntimeId) return;
    const auto typeId = ::bedrock::detail::effectName(effectId);
    auto& effects = session->localState.effects;
    if (eventId == "remove" || duration <= 0) {
        std::erase_if(effects, [&](const Effect& e) { return e.typeId == typeId; });
        return;
    }
    Effect entry{typeId, static_cast<double>(duration), EffectOptions{
        static_cast<std::uint8_t>(std::clamp(amplifier, 0, 255)), particles}};
    const auto it = std::find_if(effects.begin(), effects.end(),
                                 [&](const Effect& e) { return e.typeId == typeId; });
    if (it == effects.end()) effects.push_back(std::move(entry));
    else *it = std::move(entry);
}

void NativeGameBindings::onAddItemEntity(std::string_view payload) {
    const auto decoded = decodeAddItemActorPrefix(payload);
    if (!decoded) return;
    std::string typeId;
    {
        std::lock_guard lock(itemStateMutex);
        const auto it = itemRuntimeIds.find(static_cast<std::uint16_t>(decoded->networkId));
        if (it != itemRuntimeIds.end()) typeId = it->second;
    }
    if (typeId.empty()) return;
    if (typeId.find(':') == std::string::npos) typeId.insert(0, "minecraft:");
    std::lock_guard lock(itemStateMutex);
    itemEntities.insert_or_assign(decoded->runtimeId,
                                  ItemStack{std::move(typeId), decoded->count});
}

void NativeGameBindings::onTakeItemEntity(const nlohmann::json& payload) {
    const auto runtimeId = payload.value("runtime_entity_id", std::uint64_t{});
    if (runtimeId == 0) return;
    std::lock_guard lock(itemStateMutex);
    itemEntities.erase(runtimeId);
}

void NativeGameBindings::onItemRegistry(std::string_view payload) {
    std::lock_guard lock(itemStateMutex);
    decodeItemRegistry(payload, itemRuntimeIds);
}

void NativeGameBindings::onDimensionPacket(const nlohmann::json& payload, const char* field) {
    auto session = lockedSession(currentSession);
    if (!session) return;
    const auto it = payload.find(field);
    if (it == payload.end()) return;
    const auto name = parseDimensionValue(*it);
    if (!name) return;
    std::lock_guard lock(session->stateMutex);
    session->currentDimension = *name;
}

NativeGameFrame::NativeGameFrame(NativeGameBindings& bindings, void* client, void* level,
                                 std::uint64_t epoch)
    : bindings_(bindings), localActor_(bindings.access.localPlayer()), level_(level),
      localId_(localActor_ ? bindings.access.actorRuntimeId(localActor_) : std::nullopt),
      terrain_(bindings.terrain, bindings.terrainReady ? client : nullptr, level, epoch, {}),
      gui_(bindings.gui.tick(bindings.guiReady ? client : nullptr, level, epoch)) {
    if (session) {
        std::lock_guard lock(session->stateMutex);
        session->localRuntimeId = localId_;
    }
    bindings.currentSession.store(session, std::memory_order_release);
}

const std::vector<std::uint64_t>& NativeGameFrame::entityIds() {
    if (!enumerated_) {
        for (auto* actor : bindings_.access.actorHandles(level_)) {
            if (auto id = bindings_.access.actorRuntimeId(actor); id && id != localId_)
                if (handles_.try_emplace(*id, actor).second) ids_.push_back(*id);
        }
        if (localId_) ids_.push_back(*localId_);
        enumerated_ = true;
    }
    return ids_;
}

void* NativeGameFrame::resolve(std::uint64_t id) {
    if (id == localId_) return localActor_;
    entityIds();
    if (const auto it = handles_.find(id); it != handles_.end() &&
        bindings_.access.actorRuntimeId(it->second) == id) return it->second;
    return nullptr;
}

bool NativeGameFrame::contains(std::uint64_t id) { return resolve(id) != nullptr; }

std::optional<std::uint32_t> NativeGameFrame::actorType(std::uint64_t id) {
    if (!bindings_.playerType) return std::nullopt;
    std::uint32_t value{};
    auto* component = bindings_.components.component(resolve(id), bindings_.actors.actorTypeComponentHash);
    if (native_profile::read(component, bindings_.typeOffset, value)) return value;
    return std::nullopt;
}

bool NativeGameFrame::isPlayer(std::uint64_t id) {
    return id == localId_ || (bindings_.playerType && actorType(id) == bindings_.playerType);
}

std::optional<std::string> NativeGameFrame::typeId(std::uint64_t id) {
    if (isPlayer(id)) return "minecraft:player";
    if (actorType(id) == static_cast<std::uint32_t>(bedrock::ActorType::ItemEntity))
        return "minecraft:item";
    return std::nullopt;
}

std::optional<std::string> NativeGameFrame::name(std::uint64_t id) {
    auto name = bedrock::detail::readActorIdentity(bindings_.access, resolve(id)).name;
    if (!name.empty()) return name;
    return std::nullopt;
}

std::optional<Vector3> NativeGameFrame::location(std::uint64_t id) {
    const auto& layout = bindings_.actors;
    const auto* state = bindings_.components.component(resolve(id), layout.stateVectorComponentHash);
    Vec3 value{};
    if (native_motion::read(state, layout.statePositionOffset, value) && native_motion::finite(value))
        return copyVector(value);
    return std::nullopt;
}

std::optional<Rotation> NativeGameFrame::rotation(std::uint64_t id) {
    const auto& layout = bindings_.actors;
    const auto* state = bindings_.components.component(resolve(id), layout.actorRotationComponentHash);
    Rotation value{};
    if (native_motion::read(state, layout.rotationPitchOffset, value.x) &&
        native_motion::read(state, layout.rotationYawOffset, value.y) &&
        std::isfinite(value.x) && std::isfinite(value.y)) return value;
    return std::nullopt;
}

std::optional<std::string> NativeGameFrame::dimensionTypeId() const {
    if (!session) return std::nullopt;
    std::lock_guard lock(session->stateMutex);
    return session->currentDimension;
}

std::optional<MovementComponent> NativeGameFrame::movement(std::uint64_t id) {
    if (!bindings_.motion) return std::nullopt;
    const auto* state = bindings_.components.component(resolve(id), bindings_.actors.stateVectorComponentHash);
    Vec3 velocity{};
    if (native_motion::read(state, bindings_.motion->velocity, velocity) && native_motion::finite(velocity))
        return MovementComponent{copyVector(velocity)};
    return std::nullopt;
}

std::optional<std::string> NativeGameFrame::blockTypeId(BlockPosition position) {
    if (!terrain_) return std::nullopt;
    auto block = bindings_.terrain.getBlock({position.x, position.y, position.z});
    if (block && !block.block.identifier.empty()) return std::move(block.block.identifier);
    return std::nullopt;
}

std::optional<InventoryComponent> NativeGameFrame::inventory(std::uint64_t id) {
    if (id != localId_) return std::nullopt;
    const auto count = bindings_.access.localInventorySize(localActor_);
    if (!count || !bindings_.access.selectedHotbarSlot(localActor_)) return std::nullopt;
    std::vector<std::optional<ItemStack>> items;
    items.reserve(count);
    for (std::size_t slot = 0; slot < count; ++slot)
        items.push_back(copyItem(bindings_.access.localInventoryItem(localActor_, slot)));
    return InventoryComponent{std::move(items)};
}

std::optional<EquippableComponent> NativeGameFrame::equippable(std::uint64_t id) {
    auto* actor = resolve(id);
    if (!actor) return std::nullopt;
    constexpr bedrock::EquipmentSlot nativeSlots[]{
        bedrock::EquipmentSlot::MainHand, bedrock::EquipmentSlot::Offhand,
        bedrock::EquipmentSlot::Head, bedrock::EquipmentSlot::Chest,
        bedrock::EquipmentSlot::Legs, bedrock::EquipmentSlot::Feet};
    std::array<std::optional<ItemStack>, std::size(nativeSlots)> items;
    bool available = false;
    for (std::size_t i = 0; i < items.size(); ++i) {
        auto snapshot = bindings_.access.equipmentSnapshot(actor, nativeSlots[i]);
        available |= snapshot.available;
        items[i] = copyItem(snapshot.item);
    }
    if (available) return equipment(std::move(items));
    return std::nullopt;
}

std::optional<ItemStack> NativeGameFrame::itemStack(std::uint64_t id) const {
    std::lock_guard lock(bindings_.itemStateMutex);
    const auto it = bindings_.itemEntities.find(id);
    return it != bindings_.itemEntities.end() ? std::optional{it->second} : std::nullopt;
}

std::optional<HealthComponent> NativeGameFrame::health(std::uint64_t id) const {
    if (!session || id != localId_) return std::nullopt;
    std::lock_guard lock(session->stateMutex);
    if (session->localState.health <= 0) return std::nullopt;
    return HealthComponent{session->localState.health};
}

std::optional<FoodComponent> NativeGameFrame::food(std::uint64_t id) const {
    if (!session || id != localId_) return std::nullopt;
    std::lock_guard lock(session->stateMutex);
    if (session->localState.hunger <= 0) return std::nullopt;
    return FoodComponent{session->localState.hunger};
}

Result<void> NativeGameFrame::sendMessage(std::uint64_t id, std::string_view text) {
    if (id == localId_ && gui_ && bindings_.gui.message(std::string(text))) return Result<void>::ok();
    return unavailable("Client messages require a local player and a valid client HUD");
}

Result<void> NativeGameFrame::setBlockType(BlockPosition position, std::string_view typeId) {
    if (!terrain_) return unavailable("Native terrain access is not available");
    const auto result = bindings_.terrain.setBlock({position.x, position.y, position.z}, typeId);
    return blockErrorResult(result.error, "setBlock");
}

Result<void> NativeGameFrame::addEffect(std::uint64_t id, std::string_view typeId, double duration,
                                        EffectOptions options) {
    if (!session || id != localId_)
        return unavailable("Effect application is limited to the local player in the owning tick");
    if (typeId.empty() || !(duration > 0))
        return Result<void>::error(ErrorCode::Invalid, "An effect ID and positive duration are required");
    const auto effectId = ::bedrock::detail::effectIdForName(typeId);
    if (effectId && bindings_.effectAddFn && bindings_.effectInstanceTemplate &&
        mobEffectsComponent(bindings_.components, localActor_)) {
        // Build the instance from the game's own default instance, exactly
        // what the game itself fills new slots with, and let the game's add
        // function merge it in (growth, attributes, events included).
        unsigned char instance[kEffectSlotStride];
        if (readGameMemory(bindings_.effectInstanceTemplate, instance, kEffectSlotStride)) {
            const auto ticks = duration >= 2147483647.0
                                   ? std::numeric_limits<std::int32_t>::max()
                                   : static_cast<std::int32_t>(duration);
            putSlotInt(instance, 0x00, *effectId);
            putSlotInt(instance, 0x04, ticks);
            putSlotInt(instance, 0x20, static_cast<std::int32_t>(options.amplifier));
            instance[0x43] = options.showParticles ? 1 : 0;
            instance[0xA1] = 1;
            if (callGameAddEffect(bindings_.effectAddFn, localActor_, instance)) {
                // The game owns this effect now; drop any mirror entry so
                // it cannot shadow the real one.
                std::lock_guard lock(session->stateMutex);
                std::erase_if(session->localState.effects, [&](const Effect& e) {
                    return ::bedrock::detail::effectIdForName(e.typeId) == effectId;
                });
                return Result<void>::ok();
            }
        }
    }
    // Fallback: the game's add path is unavailable (profile symbols
    // unresolved). Keep the historical mirror behaviour so the request is
    // not lost; it stays visible to getEffects but does not apply.
    std::lock_guard lock(session->stateMutex);
    auto& effects = session->localState.effects;
    Effect entry{::bedrock::detail::canonicalEffectName(typeId), duration, options};
    const auto it = std::find_if(effects.begin(), effects.end(),
                                 [&](const Effect& e) { return e.typeId == entry.typeId; });
    if (it == effects.end()) effects.push_back(std::move(entry));
    else *it = std::move(entry);
    return Result<void>::ok();
}

Result<bool> NativeGameFrame::removeEffect(std::uint64_t id, std::string_view typeId) {
    if (!session || id != localId_)
        return unavailable<bool>("Effect removal is limited to the local player in the owning tick");
    if (typeId.empty()) return Result<bool>::error(ErrorCode::Invalid, "An effect ID is required");
    bool removed = false;
    const auto effectId = ::bedrock::detail::effectIdForName(typeId);
    if (effectId && bindings_.effectRemoveFn &&
        mobEffectsComponent(bindings_.components, localActor_)) {
        // The game's remove assumes a resolvable component (present here)
        // and early-outs for inactive ids; it also undoes attribute
        // modifiers, which a raw slot clear cannot.
        bool active = false;
        if (auto slots = mobEffectsSlots(bindings_.components, localActor_);
            slots && static_cast<std::size_t>(*effectId) < slots->count()) {
            unsigned char slot[kEffectSlotStride];
            if (readEffectSlot(*slots, static_cast<std::size_t>(*effectId), slot))
                active = slotInt(slot, 0x00) > 0;
        }
        if (active) removed = callGameRemoveEffect(bindings_.effectRemoveFn, localActor_, *effectId);
    }
    std::lock_guard lock(session->stateMutex);
    auto& effects = session->localState.effects;
    const auto before = effects.size();
    const auto canonical = ::bedrock::detail::canonicalEffectName(typeId);
    std::erase_if(effects, [&](const Effect& e) {
        return ::bedrock::detail::canonicalEffectName(e.typeId) == canonical;
    });
    return Result<bool>::ok(removed || before != effects.size());
}

Result<std::vector<Effect>> NativeGameFrame::effects(std::uint64_t id) const {
    if (!session || id != localId_)
        return unavailable<std::vector<Effect>>("Effect enumeration is limited to the local player");
    std::vector<Effect> out;
    std::vector<int> seen;
    if (auto slots = mobEffectsSlots(bindings_.components, localActor_)) {
        for (std::size_t i = 0; i < slots->count(); ++i) {
            unsigned char slot[kEffectSlotStride];
            if (!readEffectSlot(*slots, i, slot)) continue;
            const auto effectId = slotInt(slot, 0x00);
            if (effectId <= 0) continue;
            seen.push_back(effectId);
            out.push_back(Effect{::bedrock::detail::effectName(effectId),
                                 static_cast<double>(slotInt(slot, 0x04)),
                                 EffectOptions{static_cast<std::uint8_t>(slotInt(slot, 0x20)),
                                               slot[0x43] != 0}});
        }
    }
    // Mirror entries (packet-observed or slot-less writes) surface only when
    // the real store does not have that effect, so the two never shadow.
    std::lock_guard lock(session->stateMutex);
    for (const auto& entry : session->localState.effects) {
        const auto mirrorId = ::bedrock::detail::effectIdForName(entry.typeId);
        if (mirrorId && std::find(seen.begin(), seen.end(), *mirrorId) != seen.end()) continue;
        out.push_back(entry);
    }
    return Result<std::vector<Effect>>::ok(std::move(out));
}

Result<void> NativeGameFrame::clearEffects(std::uint64_t id) {
    if (!session || id != localId_)
        return unavailable("Effect clearing is limited to the local player in the owning tick");
    if (bindings_.effectRemoveFn &&
        mobEffectsComponent(bindings_.components, localActor_)) {
        if (auto slots = mobEffectsSlots(bindings_.components, localActor_)) {
            for (std::size_t i = 0; i < slots->count(); ++i) {
                unsigned char slot[kEffectSlotStride];
                if (!readEffectSlot(*slots, i, slot)) continue;
                const auto effectId = slotInt(slot, 0x00);
                if (effectId <= 0) continue;
                callGameRemoveEffect(bindings_.effectRemoveFn, localActor_, effectId);
            }
        }
    }
    std::lock_guard lock(session->stateMutex);
    session->localState.effects.clear();
    return Result<void>::ok();
}
} // namespace bedrock::sdk::detail
