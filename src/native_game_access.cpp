#include <bedrock/native/game_access.hpp>
#include <bedrock/native/runtime_actors.hpp>
#include <bedrock/native/actor_components.hpp>
#include <bedrock/native/player_list.hpp>
#include <bedrock/native/local_player.hpp>
#include <bedrock/native/items.hpp>
#include <bedrock/native/actors.hpp>
#include <bedrock/profiles/profile_json.hpp>
#include <bedrock/detail/owned_msvc_string.hpp>
#include <atomic>
#include <chrono>
#include <cstring>
#include <unordered_map>

namespace bedrock {
struct NativeGameAccess::Impl {
    using ItemHoverNameFn = void*(__fastcall*)(void*, void*);
    using Deallocate = void(__fastcall*)(void*);
    using ClientLevel = void*(__fastcall*)(void*);
    Deallocate deallocate{};
    ClientLevel clientLevel{};
    detail::OwnedMsvcStringLayout returnedString{};

    NativeRuntimeActors runtimeActors;
    NativeActorComponents actorComponents;
    NativePlayerList playerRoster;
    NativeLocalPlayerGetter localPlayerGetter;
    std::optional<NativeItemLayout> nativeItemLayout;
    std::optional<NativeActorLayout> nativeActorLayout;
    std::atomic<void*> clientInstance{}, levelInstance{}, itemHoverNameTarget{};
    std::unordered_map<std::string, std::string> itemDisplayNames;
    std::unordered_map<void*, std::string> nativeItemIdentifiers;
    std::chrono::steady_clock::time_point lastItemDisplayNameResolve{};
    std::atomic<bool> displayNamesEnabled{true};
    std::atomic<DWORD> ownerThread{};
    unsigned tickDepth{};
    bool wireRoster{};
    RosterProvider rosterProvider;
    NameProvider nameProvider;
    bool onTick() const noexcept {
        return ownerThread.load(std::memory_order_acquire) == GetCurrentThreadId() && tickDepth;
    }
struct ReadRegion { std::uintptr_t begin{},end{}; };
inline static thread_local std::array<ReadRegion,32> nativeReadRegions{};
inline static thread_local std::size_t nativeReadRegionCursor{};
inline static thread_local bool nativeReadRegionsActive=false;

bool readable(const void* pointer, std::size_t bytes) noexcept {
    if (!pointer || !bytes) return false;
    const auto start = reinterpret_cast<std::uintptr_t>(pointer);
    if (start > UINTPTR_MAX - bytes) return false;
    const auto wantedEnd = start + bytes;
    if(nativeReadRegionsActive) for(const auto& region:nativeReadRegions)
        if(start>=region.begin && wantedEnd<=region.end) return true;
    auto cursor = start;
    while (cursor < wantedEnd) {
        MEMORY_BASIC_INFORMATION region{};
        if (!VirtualQuery(reinterpret_cast<const void*>(cursor), &region, sizeof(region)) ||
            region.State != MEM_COMMIT ||
            (region.Protect & (PAGE_GUARD | PAGE_NOACCESS))) return false;
        const DWORD protection = region.Protect & 0xFF;
        if (protection != PAGE_READONLY && protection != PAGE_READWRITE &&
            protection != PAGE_WRITECOPY && protection != PAGE_EXECUTE_READ &&
            protection != PAGE_EXECUTE_READWRITE &&
            protection != PAGE_EXECUTE_WRITECOPY) return false;
        const auto regionStart = reinterpret_cast<std::uintptr_t>(region.BaseAddress);
        const auto regionEnd = regionStart + region.RegionSize;
        if (regionEnd <= cursor) return false;
        if(nativeReadRegionsActive)
            nativeReadRegions[nativeReadRegionCursor++%nativeReadRegions.size()]={regionStart,regionEnd};
        cursor = regionEnd;
    }
    return true;
}


template <class T>
bool safeRead(const void* base, std::size_t offset, T& result) noexcept {
    if (!base) return false;
    const auto address = static_cast<const std::byte*>(base) + offset;
    if (!readable(address, sizeof(T))) return false;
    __try {
        result = *reinterpret_cast<const T*>(address);
        return true;
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool safeCopyBytes(const char* source, char* destination, std::size_t size) noexcept {
    if (!source || !destination || !size || !readable(source, size)) return false;
    __try {
        std::memcpy(destination, source, size);
        return true;
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool copyNativeString(const void* object, std::string& output,
                      const bedrock::NativeItemLayout& layout,
                      std::size_t maximum) noexcept {
    if (!object) return false;
    std::uintptr_t data = 0;
    std::size_t size = 0;
    std::size_t capacity = 0;
    if (!safeRead(object, 0, data) ||
        !safeRead(object, layout.itemStringSizeOffset, size) ||
        !safeRead(object, layout.itemStringCapacityOffset, capacity) ||
        size > maximum || capacity < size)
        return false;
    if (size == 0) { output.clear(); return true; }
    const auto* bytes = capacity <= 15
        ? static_cast<const char*>(object)
        : reinterpret_cast<const char*>(data);
    if (!readable(bytes, size)) return false;
    try {
        std::string result(size, '\0');
        if (!safeCopyBytes(bytes, result.data(), size)) return false;
        output = std::move(result);
        return true;
    } catch (...) {
        return false;
    }
}

bool tryCallInventoryGetItem(void* inventory, std::size_t slot,
                             const bedrock::NativeItemLayout& layout,
                             void** output) noexcept {
    if (output) *output = nullptr;
    if (!inventory || !output || slot >= layout.localInventorySlots) return false;
    void** vtable = nullptr;
    if (!safeRead(inventory, 0, vtable) || !vtable) return false;
    static std::atomic<void**> cachedVtable{nullptr};
    static std::atomic<void*> cachedTarget{nullptr};
    void* target = nullptr;
    if (cachedVtable.load(std::memory_order_acquire) == vtable) {
        target = cachedTarget.load(std::memory_order_acquire);
    } else {
        if (!readable(vtable, sizeof(void*) * (layout.inventoryGetItemVtableSlot + 1)) ||
            !safeRead(vtable, sizeof(void*) * layout.inventoryGetItemVtableSlot, target) ||
            !executable(target)) return false;
        cachedTarget.store(target, std::memory_order_release);
        cachedVtable.store(vtable, std::memory_order_release);
    }
    if (!target) return false;
    using GetItemFn = void*(__fastcall*)(void*, int);
    __try {
        *output = reinterpret_cast<GetItemFn>(target)(inventory, static_cast<int>(slot));
        // A successfully invoked getter returning nullptr is a confirmed empty slot.
        return true;
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        *output = nullptr;
        return false;
    }
}

void* callInventoryGetItem(void* inventory, std::size_t slot,
                           const bedrock::NativeItemLayout& layout) noexcept {
    void* stack = nullptr;
    return tryCallInventoryGetItem(inventory, slot, layout, &stack) ? stack : nullptr;
}

bool validateClient(void* client) noexcept {
    std::uintptr_t vtable = 0;
    return safeRead(client, 0, vtable) && vtable != 0 && readable(
        reinterpret_cast<const void*>(vtable), sizeof(void*));
}


bool executable(const void* pointer) noexcept {
    MEMORY_BASIC_INFORMATION info{};
    if (!pointer || !VirtualQuery(pointer, &info, sizeof(info)) ||
        info.State != MEM_COMMIT || info.Type != MEM_IMAGE) return false;
    const DWORD protection = info.Protect & 0xFFu;
    return protection == PAGE_EXECUTE || protection == PAGE_EXECUTE_READ ||
           protection == PAGE_EXECUTE_READWRITE || protection == PAGE_EXECUTE_WRITECOPY;
}

bool looksLikeActor(void* actor) noexcept {
    if (!actor || !readable(actor, sizeof(void*))) return false;
    void** vtable = nullptr;
    if (!safeRead(actor, 0, vtable)) return false;
    if (!vtable || !readable(vtable, sizeof(void*) * 3)) return false;
    unsigned executableSlots = 0;
    for (std::size_t i = 0; i < 3; ++i) {
        void* entry = nullptr;
        std::memcpy(&entry, vtable + i, sizeof(entry));
        if (executable(entry)) ++executableSlots;
    }
    return executableSlots >= 2;
}

bool isActorValid(void* actor) const noexcept {
    return looksLikeActor(actor);
}

std::vector<void*> actorHandles(void* world) const {
    std::vector<void*> result;
    if (!onTick() || !world ||
        world != levelInstance.load(std::memory_order_acquire)) return result;
    auto captured=runtimeActors.snapshot(world);
    if(!captured)return result;
    return std::move(*captured);
}

bool invokeItemHoverName(void* stack, void* target, void* output) noexcept {
    if (!stack || !target || !output || !executable(target)) return false;
    __try {
        return reinterpret_cast<ItemHoverNameFn>(target)(stack, output) == output;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void populateItemDisplayName(void* stack, bedrock::ItemStack& item) {
    if (item.identifier.empty()) return;
    {
        const auto it = itemDisplayNames.find(item.identifier);
        if (it != itemDisplayNames.end()) {
            item.displayName = it->second;
            return;
        }
        const auto now = std::chrono::steady_clock::now();
        constexpr auto minimumResolveInterval = std::chrono::milliseconds(500);
        if (lastItemDisplayNameResolve != std::chrono::steady_clock::time_point{} &&
            now - lastItemDisplayNameResolve < minimumResolveInterval) return;
        lastItemDisplayNameResolve = now;
    }
    std::string displayName;
    auto* target = itemHoverNameTarget.load(std::memory_order_acquire);
    if(!target || !deallocate) return;
    alignas(8) std::array<std::byte,32> storage{};
    if (!invokeItemHoverName(stack, target, storage.data())) return;
    try {
        displayName=detail::copyOwnedMsvcString(storage,returnedString,deallocate,
            [](const void* address,void*& value) { return native_profile::read(address,0,value); });
    } catch (...) { return; }
    if(displayName.empty()) return;
    item.displayName = displayName;
    try {
        if (itemDisplayNames.size() >= 512) itemDisplayNames.clear();
        itemDisplayNames.insert_or_assign(item.identifier, std::move(displayName));
    } catch (...) {
        // Display-name caching is optional; preserve the copied item snapshot.
    }
}

bool readActorEquipment(void* actor,const bedrock::NativeItemLayout& layout,void** output) noexcept {
    if(!output)return false;*output=actorComponents.component(actor,layout.equipmentComponentHash);return *output!=nullptr;
}
bool readActorRuntimeIdValue(void* actor,std::uint64_t* output) noexcept {
    if(!onTick()||!output)return false;const auto value=actorComponents.runtimeId(actor);*output=value.value_or(0);return value.has_value();
}
std::optional<std::uint64_t> readActorRuntimeId(void* actor) noexcept {
    std::uint64_t runtimeId = 0;
    return readActorRuntimeIdValue(actor, &runtimeId)
        ? std::optional<std::uint64_t>{runtimeId} : std::nullopt;
}

std::optional<std::uint64_t> actorRuntimeId(void* actor) const {
    return onTick() ? readActorRuntimeId(actor) : std::nullopt;
}

bool readActorGameType(void* actor,std::int32_t& gameType) noexcept {
    if(!onTick()) return false;
    const auto value=actorComponents.gameType(actor);if(!value)return false;gameType=*value;return true;
}

void* localPlayer() const noexcept {
    if (!onTick() || !nativeItemLayout) return nullptr;
    void* client = clientInstance.load(std::memory_order_acquire);
    void* player=localPlayerGetter.get(client);
    return looksLikeActor(player)?player:nullptr;
}

enum class EquipmentReadStatus { Failed, Empty, Item };

EquipmentReadStatus readEquipmentItem(void* actor, bedrock::EquipmentSlot slot,
                                     bedrock::ItemStack* output) {
    if (output) *output = {};
    if (!onTick() || !nativeItemLayout || !actor || !output) return EquipmentReadStatus::Failed;
    const auto& layout = *nativeItemLayout;
    void* component = nullptr;
    if (!readActorEquipment(actor, layout, &component) || !component)
        return EquipmentReadStatus::Failed;
    std::size_t slotIndex = 0;
    std::size_t containerOffset = layout.handContainerOffset;
    switch (slot) {
    case bedrock::EquipmentSlot::MainHand: slotIndex = layout.handSlots[0]; break;
    case bedrock::EquipmentSlot::Offhand: slotIndex = layout.handSlots[1]; break;
    case bedrock::EquipmentSlot::Head:
        containerOffset = layout.armorContainerOffset; slotIndex = layout.armorSlots[0]; break;
    case bedrock::EquipmentSlot::Chest:
        containerOffset = layout.armorContainerOffset; slotIndex = layout.armorSlots[1]; break;
    case bedrock::EquipmentSlot::Legs:
        containerOffset = layout.armorContainerOffset; slotIndex = layout.armorSlots[2]; break;
    case bedrock::EquipmentSlot::Feet:
        containerOffset = layout.armorContainerOffset; slotIndex = layout.armorSlots[3]; break;
    }
    void* container = nullptr;
    if (!safeRead(component, containerOffset, container) || !container)
        return EquipmentReadStatus::Failed;
    void* stack = nullptr;
    if (!tryCallInventoryGetItem(container, slotIndex, layout, &stack))
        return EquipmentReadStatus::Failed;
    if (!stack) return EquipmentReadStatus::Empty;
    bedrock::ItemStack item;
    std::uint8_t count = 0;
    void** itemReference = nullptr;
    void* nativeItem = nullptr;
    if (!safeRead(stack, layout.itemCountOffset, count))
        return EquipmentReadStatus::Failed;
    if (count == 0) return EquipmentReadStatus::Empty;
    if (!safeRead(stack, layout.itemReferenceOffset, itemReference) || !itemReference ||
        !safeRead(itemReference, 0, nativeItem) || !nativeItem)
        return EquipmentReadStatus::Failed;
    item.count = count;
    const auto cachedId = nativeItemIdentifiers.find(nativeItem);
    if (cachedId != nativeItemIdentifiers.end()) {
        item.identifier = cachedId->second;
    } else {
        const auto* id = static_cast<const std::byte*>(nativeItem) + layout.itemNamespacedIdOffset;
        if (!copyNativeString(id, item.identifier, layout, layout.maxStringBytes))
            return EquipmentReadStatus::Failed;
        if (!item.identifier.empty() && item.identifier.find(':') == std::string::npos)
            item.identifier.insert(0, "minecraft:");
        if (!item.identifier.empty() && nativeItemIdentifiers.size() < 2048)
            nativeItemIdentifiers.emplace(nativeItem, item.identifier);
    }
    // The display name is cached by item identifier, so the native resolver
    // runs only for the first observation of each item type.
    if (displayNamesEnabled.load(std::memory_order_relaxed)) populateItemDisplayName(stack, item);
    if (!item.valid()) return EquipmentReadStatus::Failed;
    *output = std::move(item);
    return EquipmentReadStatus::Item;
}

std::optional<bedrock::ItemStack> equipmentItem(
    void* actor, bedrock::EquipmentSlot slot) const {
    bedrock::ItemStack item;
    return readEquipmentItem(actor, slot, &item) == EquipmentReadStatus::Item
        ? std::optional<bedrock::ItemStack>{std::move(item)} : std::nullopt;
}

std::size_t localInventorySize(void* player) const {
    return nativeItemLayout && player && player == localPlayer()
        ? nativeItemLayout->localInventorySlots : 0;
}

std::optional<bedrock::ItemStack> localInventoryItem(
    void* player, std::size_t slot) const {
    if (!nativeItemLayout || !player || player != localPlayer() ||
        slot >= nativeItemLayout->localInventorySlots) return std::nullopt;
    const auto& layout = *nativeItemLayout;
    void* supplies = nullptr;
    void* inventory = nullptr;
    if (!safeRead(player, layout.localPlayerSuppliesOffset, supplies) || !supplies ||
        !safeRead(supplies, layout.suppliesInventoryOffset, inventory) || !inventory)
        return std::nullopt;
    void* stack = callInventoryGetItem(inventory, slot, layout);
    if (!stack) return std::nullopt;
    bedrock::ItemStack item;
    std::uint8_t count = 0;
    void** itemReference = nullptr;
    void* nativeItem = nullptr;
    if (!safeRead(stack, layout.itemCountOffset, count) || count == 0 ||
        !safeRead(stack, layout.itemReferenceOffset, itemReference) || !itemReference ||
        !safeRead(itemReference, 0, nativeItem) || !nativeItem) return std::nullopt;
    item.count = count;
    const auto* id = static_cast<const std::byte*>(nativeItem) + layout.itemNamespacedIdOffset;
    const auto* translation = static_cast<const std::byte*>(nativeItem) +
                              layout.itemTranslationKeyOffset;
    if (!copyNativeString(id, item.identifier, layout, layout.maxStringBytes))
        return std::nullopt;
    (void)copyNativeString(translation, item.translationKey, layout, layout.maxStringBytes);
    if (!item.identifier.empty() && item.identifier.find(':') == std::string::npos)
        item.identifier.insert(0, "minecraft:");
    return item.valid() ? std::optional<bedrock::ItemStack>{std::move(item)} : std::nullopt;
}

std::optional<std::size_t> selectedHotbarSlot(void* player) const {
    if (!nativeItemLayout || !player || player != localPlayer()) return std::nullopt;
    void* supplies = nullptr;
    std::uint32_t selected = 0;
    if (!safeRead(player, nativeItemLayout->localPlayerSuppliesOffset, supplies) || !supplies ||
        !safeRead(supplies, nativeItemLayout->suppliesSelectedSlotOffset, selected) || selected > 8)
        return std::nullopt;
    return static_cast<std::size_t>(selected);
}

void* worldOf(void* actor) const noexcept {
    return looksLikeActor(actor) ? levelInstance.load(std::memory_order_acquire) : nullptr;
}


    void clear() noexcept {
        nativeItemIdentifiers.clear(); itemDisplayNames.clear();
        lastItemDisplayNameResolve = {};
    }
};
NativeGameAccess::NativeGameAccess() : impl_(std::make_unique<Impl>()) {}
NativeGameAccess::~NativeGameAccess() = default;
bool NativeGameAccess::load(const RuntimeProfile& profile, std::string& error) {
    try {
        auto next = std::make_unique<Impl>();
        const auto& text = profile.json();
        const auto root = parseNativeProfile(text);
        NativeItemLayout items; NativeActorLayout actors;
        if (!parseNativeItemLayout(text, items, error) ||
            !parseNativeActorLayout(text, actors, error) ||
            !next->runtimeActors.load(text, error)) return false;
        next->nativeItemLayout = items; next->nativeActorLayout = actors;
        next->actorComponents.configure(actors, root);
        next->localPlayerGetter.load(root);
        next->wireRoster = PlayerListPackets::supports(text);
        if (next->wireRoster) {
            if (!root.contains("layouts") || !root.at("layouts").contains("PacketRead") ||
                root.at("layouts").at("PacketRead").value("revision",0) < 2)
                throw std::runtime_error("packet roster requires current receive layout");
        } else next->playerRoster.load(text);
        if(root.at("symbols").contains("ClientInstance::getLevel"))
            next->clientLevel=reinterpret_cast<Impl::ClientLevel>(native_profile::checked(root,
                root.at("symbols").at("ClientInstance::getLevel")));
        const auto& itemMetadata=root.at("native_items");
        if(itemMetadata.contains("returned_string_abi") && itemMetadata.contains("string_deallocate")) {
            const auto& abi=itemMetadata.at("returned_string_abi");
            const auto number=[&](const char* key) {return native_profile::number(abi.at(key));};
            next->returnedString={number("size_offset"),number("capacity_offset"),number("inline_bytes"),
                number("maximum_bytes"),number("large_allocation_threshold"),number("alignment_overhead")};
            next->deallocate=reinterpret_cast<Impl::Deallocate>(native_profile::checked(root,itemMetadata.at("string_deallocate")));
            const auto& method=root.at("symbols").at("ItemStackBase::getHoverName");
            if(method.at("abi")!="std::string* __fastcall(ItemStackBase*, std::string*)")
                throw std::runtime_error("unsupported item display-name ABI");
            if(method.value("abi_verified",false) || profile.candidatePolicy()==CandidatePolicy::AllowCandidates)
                next->itemHoverNameTarget=native_profile::checked(root,method);
        }
        impl_ = std::move(next);
        error.clear(); return true;
    } catch (const std::exception& e) { error=e.what(); return false; }
}
void NativeGameAccess::setRosterProvider(RosterProvider provider) { impl_->rosterProvider=std::move(provider); }
void NativeGameAccess::setNameProvider(NameProvider provider) { impl_->nameProvider=std::move(provider); }
bool NativeGameAccess::packetRosterSupported() const noexcept { return impl_->wireRoster; }
bool NativeGameAccess::displayNamesAvailable() const noexcept { return impl_->itemHoverNameTarget.load()!=nullptr; }
void NativeGameAccess::setDisplayNamesEnabled(bool enabled) noexcept { impl_->displayNamesEnabled=enabled; }
void NativeGameAccess::clearWorldCaches() noexcept { if(impl_->onTick()) impl_->clear(); }
void NativeGameAccess::observeClient(void* client) noexcept {
    if (impl_->validateClient(client)) impl_->clientInstance.store(client, std::memory_order_release);
}
namespace {
bool readClientLevel(void*(__fastcall* getter)(void*),void* client,void*& output) noexcept {
    __try { output=getter(client);return true; }
    __except(EXCEPTION_EXECUTE_HANDLER) {return false;}
}
}
bool NativeGameAccess::beginWorldTick(void* level) noexcept {
    if(!level) return false;
    if(impl_->clientLevel) {
        auto* client=impl_->clientInstance.load(std::memory_order_acquire);
        void* currentLevel{};
        if(!client || !readClientLevel(impl_->clientLevel,client,currentLevel) || currentLevel!=level) return false;
    }
    DWORD expected{}; const auto current=GetCurrentThreadId();
    impl_->ownerThread.compare_exchange_strong(expected,current);
    if(impl_->ownerThread.load()!=current) return false;
    if(!impl_->tickDepth++) {
        impl_->nativeReadRegions.fill({}); impl_->nativeReadRegionCursor=0;
        impl_->nativeReadRegionsActive=true;
        if (impl_->levelInstance.exchange(level)!=level) impl_->clear();
    }
    return true;
}
void NativeGameAccess::endWorldTick() noexcept {
    if(impl_->ownerThread.load()==GetCurrentThreadId() && impl_->tickDepth && !--impl_->tickDepth)
        impl_->nativeReadRegionsActive=false;
}
bool NativeGameAccess::isActorValid(void* actor) const noexcept { return impl_->looksLikeActor(actor); }
void* NativeGameAccess::localPlayer() const noexcept { return impl_->localPlayer(); }
void* NativeGameAccess::worldOf(void* actor) const noexcept { return impl_->onTick()?impl_->worldOf(actor):nullptr; }
std::vector<void*> NativeGameAccess::actorHandles(void* world) const { return impl_->actorHandles(world); }
std::optional<std::uint64_t> NativeGameAccess::actorRuntimeId(void* actor) const { return impl_->actorRuntimeId(actor); }
std::optional<std::string> NativeGameAccess::nameTag(void* actor) const {
    return impl_->nameProvider?impl_->nameProvider(actor):std::nullopt;
}
std::optional<GameType> NativeGameAccess::gameType(void* actor) const noexcept {
    std::int32_t value{};
    return impl_->readActorGameType(actor,value)?std::optional<GameType>{static_cast<GameType>(value)}:std::nullopt;
}
EquipmentSnapshot NativeGameAccess::equipmentSnapshot(void* actor, EquipmentSlot slot) const {
    ItemStack item;
    const auto status=impl_->readEquipmentItem(actor,slot,&item);
    return {status!=Impl::EquipmentReadStatus::Failed,
        status==Impl::EquipmentReadStatus::Item?std::optional<ItemStack>{std::move(item)}:std::nullopt};
}
std::optional<ItemStack> NativeGameAccess::equipmentItem(void* actor,EquipmentSlot slot) const { return equipmentSnapshot(actor,slot).item; }
std::size_t NativeGameAccess::localInventorySize(void* player) const { return impl_->localInventorySize(player); }
std::optional<ItemStack> NativeGameAccess::localInventoryItem(void* player,std::size_t slot) const { return impl_->localInventoryItem(player,slot); }
std::optional<std::size_t> NativeGameAccess::selectedHotbarSlot(void* player) const { return impl_->selectedHotbarSlot(player); }
std::vector<PacketPlayer> NativeGameAccess::rosterSnapshot(void* world) const {
    if(!impl_->onTick() || !world || world!=impl_->levelInstance.load()) return {};
    if(impl_->wireRoster) {
        const auto players=impl_->rosterProvider?impl_->rosterProvider():nullptr;
        return players?*players:std::vector<PacketPlayer>{};
    }
    auto source=impl_->playerRoster.snapshot(world);
    std::vector<PacketPlayer> rows;
    if(!source) return rows;
    rows.reserve(source->size());
    for (const auto& row:*source) {
        PacketPlayer value; value.name=row.name; value.uniqueId=row.uniqueId;
        std::memcpy(value.uuid.data(),&row.uuid,16); rows.push_back(std::move(value));
    }
    return rows;
}
std::vector<std::string> NativeGameAccess::playerListNames(void* world) const {
    std::vector<std::string> names;
    for(const auto& row:rosterSnapshot(world)) names.push_back(row.name);
    return names;
}
bool NativeGameAccess::face(void* world,const std::array<std::byte,16>& uuid,
                           std::array<std::uint32_t,64>& output) const {
    if(!impl_->onTick() || world!=impl_->levelInstance.load()) return false;
    if(impl_->wireRoster) {
        const auto rows=impl_->rosterProvider?impl_->rosterProvider():nullptr;
        if(rows) for(const auto& row:*rows) if(row.uuid==uuid) {output=row.face;return row.hasFace;}
        return false;
    }
    PlayerUuid id; std::memcpy(&id,uuid.data(),16);
    return impl_->playerRoster.face(world,id,output);
}
} // namespace bedrock
