#include "bedrock/sdk/item_stack_requests.hpp"

#include "profile_facade.hpp"

#ifdef _WIN32
#include <bedrock/native/item_stack_requests.hpp>
#endif

namespace bedrock::sdk {

struct ItemStackRequests::Impl {
#ifdef _WIN32
    bedrock::NativeItemStackRequests native;
#endif
    // statusDetail()/transferDetail() return references; on host builds
    // there is no native engine, so keep stable empty strings here.
    std::string emptyDetail;
};

ItemStackRequests::ItemStackRequests() : impl_(std::make_unique<Impl>()) {}
ItemStackRequests::~ItemStackRequests() = default;
ItemStackRequests::ItemStackRequests(ItemStackRequests&&) noexcept = default;
ItemStackRequests& ItemStackRequests::operator=(ItemStackRequests&&) noexcept = default;

#ifdef _WIN32
namespace {
using NativeKind = bedrock::NativeItemStackRequests::BackgroundKind;
NativeKind toNative(BackgroundKindValue kind) noexcept {
    switch (kind) {
    case BackgroundKindValue::Table: return NativeKind::Table;
    case BackgroundKindValue::Furnace: return NativeKind::Furnace;
    case BackgroundKindValue::Inventory: return NativeKind::Inventory;
    }
    return NativeKind::Inventory;
}
CraftJobStatusValue fromNative(bedrock::CraftJobStatus status) noexcept {
    return static_cast<CraftJobStatusValue>(static_cast<int>(status));
}
bedrock::CraftIngredient toNative(const CraftIngredientValue& ingredient) {
    bedrock::CraftIngredient result;
    result.kind = static_cast<bedrock::CraftIngredient::Kind>(static_cast<int>(ingredient.kind));
    result.value = ingredient.value;
    result.count = ingredient.count;
    return result;
}
CraftIngredientValue fromNative(const bedrock::CraftIngredient& ingredient) {
    CraftIngredientValue result;
    result.kind = static_cast<CraftIngredientValue::Kind>(static_cast<int>(ingredient.kind));
    result.value = ingredient.value;
    result.count = ingredient.count;
    return result;
}
bedrock::CraftRecipe toNative(const CraftRecipeValue& recipe) {
    bedrock::CraftRecipe result;
    result.recipeId = recipe.recipeId;
    result.networkId = recipe.networkId;
    result.shaped = recipe.shaped;
    result.width = recipe.width;
    result.height = recipe.height;
    result.cells.reserve(recipe.cells.size());
    for (const auto& cell : recipe.cells)
        result.cells.push_back(toNative(cell));
    result.outputLegacyId = recipe.outputLegacyId;
    result.outputCount = recipe.outputCount;
    result.outputMetadata = recipe.outputMetadata;
    result.outputBlockRuntimeId = recipe.outputBlockRuntimeId;
    result.outputExtra = recipe.outputExtra;
    return result;
}
CraftRecipeValue fromNative(const bedrock::CraftRecipe& recipe) {
    CraftRecipeValue result;
    result.recipeId = recipe.recipeId;
    result.networkId = recipe.networkId;
    result.shaped = recipe.shaped;
    result.width = recipe.width;
    result.height = recipe.height;
    result.cells.reserve(recipe.cells.size());
    for (const auto& cell : recipe.cells)
        result.cells.push_back(fromNative(cell));
    result.outputLegacyId = recipe.outputLegacyId;
    result.outputCount = recipe.outputCount;
    result.outputMetadata = recipe.outputMetadata;
    result.outputBlockRuntimeId = recipe.outputBlockRuntimeId;
    result.outputExtra = recipe.outputExtra;
    return result;
}
CraftInventorySlotValue fromNative(const bedrock::CraftInventorySlot& slot) {
    CraftInventorySlotValue result;
    result.slot = slot.slot;
    result.item = slot.item;
    result.count = slot.count;
    result.netId = slot.netId;
    result.damage = slot.damage;
    return result;
}
std::vector<CraftInventorySlotValue> fromNative(
    const std::vector<bedrock::CraftInventorySlot>& slots) {
    std::vector<CraftInventorySlotValue> result;
    result.reserve(slots.size());
    for (const auto& slot : slots)
        result.push_back(fromNative(slot));
    return result;
}
bedrock::BlockPos toNative(BlockPos pos) noexcept { return {pos.x, pos.y, pos.z}; }
bedrock::Vec3 toNative(Vec3Value v) noexcept { return {v.x, v.y, v.z}; }
} // namespace
#endif

void ItemStackRequests::onDiagnostic(Diagnostic diagnostic) {
#ifdef _WIN32
    impl_->native.onDiagnostic(std::move(diagnostic));
#else
    (void)diagnostic;
#endif
}

bool ItemStackRequests::load(const Profile& profile, std::string& error) {
#ifdef _WIN32
    const auto runtime = ProfileRuntimeAccess::runtime(profile);
    if (!runtime) {
        error = "item stack requests profile runtime snapshot unavailable";
        return false;
    }
    return impl_->native.load(*runtime, error);
#else
    (void)profile;
    error = "item stack requests unavailable on this host";
    return false;
#endif
}

bool ItemStackRequests::installPackets(const Profile& profile, std::string& error) {
#ifdef _WIN32
    return impl_->native.installPackets(std::string(profile.json()), error);
#else
    (void)profile;
    error = "item stack requests unavailable on this host";
    return false;
#endif
}

void ItemStackRequests::stop() {
#ifdef _WIN32
    impl_->native.stop();
#endif
}

void ItemStackRequests::update(void* client, void* level, bool allowWork) {
#ifdef _WIN32
    impl_->native.update(client, level, allowWork);
#else
    (void)client;
    (void)level;
    (void)allowWork;
#endif
}

bool ItemStackRequests::senderReady() const noexcept {
#ifdef _WIN32
    return impl_->native.senderReady();
#else
    return false;
#endif
}

bool ItemStackRequests::connectionReady() const noexcept {
#ifdef _WIN32
    return impl_->native.connectionReady();
#else
    return false;
#endif
}

bool ItemStackRequests::ready() const noexcept {
#ifdef _WIN32
    return impl_->native.ready();
#else
    return false;
#endif
}

bool ItemStackRequests::backgroundSupported() const noexcept {
#ifdef _WIN32
    return impl_->native.backgroundSupported();
#else
    return false;
#endif
}

bool ItemStackRequests::backgroundActive() const {
#ifdef _WIN32
    return impl_->native.backgroundActive();
#else
    return false;
#endif
}

bool ItemStackRequests::backgroundFailed() const {
#ifdef _WIN32
    return impl_->native.backgroundFailed();
#else
    return false;
#endif
}

bool ItemStackRequests::backgroundOpen(BackgroundKindValue kind) const {
#ifdef _WIN32
    return impl_->native.backgroundOpen(toNative(kind));
#else
    (void)kind;
    return false;
#endif
}

bool ItemStackRequests::backgroundArm(BackgroundKindValue kind, BlockPos position) {
#ifdef _WIN32
    return impl_->native.backgroundArm(toNative(kind), toNative(position));
#else
    (void)kind;
    (void)position;
    return false;
#endif
}

bool ItemStackRequests::backgroundUse(BackgroundKindValue kind, BlockPos block, unsigned face,
                                      Vec3Value eye, Vec3Value hit) {
#ifdef _WIN32
    return impl_->native.backgroundUse(toNative(kind), toNative(block), face, toNative(eye),
                                       toNative(hit));
#else
    (void)kind;
    (void)block;
    (void)face;
    (void)eye;
    (void)hit;
    return false;
#endif
}

bool ItemStackRequests::backgroundInventory() {
#ifdef _WIN32
    return impl_->native.backgroundInventory();
#else
    return false;
#endif
}

bool ItemStackRequests::backgroundClose() {
#ifdef _WIN32
    return impl_->native.backgroundClose();
#else
    return false;
#endif
}

void ItemStackRequests::cancelJobs() {
#ifdef _WIN32
    impl_->native.cancelJobs();
#endif
}

void ItemStackRequests::worldChanged() {
#ifdef _WIN32
    impl_->native.worldChanged();
#endif
}

bool ItemStackRequests::recipesReady() const noexcept {
#ifdef _WIN32
    return impl_->native.recipesReady();
#else
    return false;
#endif
}

bool ItemStackRequests::copyRecipe(const std::string& recipeId, CraftRecipeValue& out) const {
#ifdef _WIN32
    bedrock::CraftRecipe native;
    if (!impl_->native.copyRecipe(recipeId, native))
        return false;
    out = fromNative(native);
    return true;
#else
    (void)recipeId;
    (void)out;
    return false;
#endif
}

std::int32_t ItemStackRequests::memoryNetId(int slot) {
#ifdef _WIN32
    return impl_->native.memoryNetId(slot);
#else
    (void)slot;
    return 0;
#endif
}

std::int32_t ItemStackRequests::memoryCount(int slot) {
#ifdef _WIN32
    return impl_->native.memoryCount(slot);
#else
    (void)slot;
    return 0;
#endif
}

std::vector<CraftInventorySlotValue> ItemStackRequests::memorySnapshot(bool includeEmpty,
                                                                       int firstSlot,
                                                                       int slotCount) {
#ifdef _WIN32
    return fromNative(impl_->native.memorySnapshot(includeEmpty, firstSlot, slotCount));
#else
    (void)includeEmpty;
    (void)firstSlot;
    (void)slotCount;
    return {};
#endif
}

std::vector<CraftInventorySlotValue> ItemStackRequests::snapshot(bool includeEmpty) {
#ifdef _WIN32
    return fromNative(impl_->native.snapshot(includeEmpty));
#else
    (void)includeEmpty;
    return {};
#endif
}

void ItemStackRequests::invalidateInventorySlot(int slot) {
#ifdef _WIN32
    impl_->native.invalidateInventorySlot(slot);
#else
    (void)slot;
#endif
}

bool ItemStackRequests::startCraft(const CraftRecipeValue& recipe, std::string outputName,
                                   int times, int gridBase, int gridWidth) {
#ifdef _WIN32
    return impl_->native.startCraft(toNative(recipe), std::move(outputName), times, gridBase,
                                    gridWidth);
#else
    (void)recipe;
    (void)outputName;
    (void)times;
    (void)gridBase;
    (void)gridWidth;
    return false;
#endif
}

CraftJobStatusValue ItemStackRequests::status() const noexcept {
#ifdef _WIN32
    return fromNative(impl_->native.status());
#else
    return CraftJobStatusValue::Idle;
#endif
}

const std::string& ItemStackRequests::statusDetail() const noexcept {
#ifdef _WIN32
    return impl_->native.statusDetail();
#else
    return impl_->emptyDetail;
#endif
}

void ItemStackRequests::clearStatus() {
#ifdef _WIN32
    impl_->native.clearStatus();
#endif
}

void ItemStackRequests::resetAfterFailure() {
#ifdef _WIN32
    impl_->native.resetAfterFailure();
#endif
}

bool ItemStackRequests::startTransfer(std::uint32_t srcContainer, int srcSlot,
                                      std::int32_t srcNetId, std::uint32_t dstContainer,
                                      int dstSlot, int count, std::string srcItemId) {
#ifdef _WIN32
    return impl_->native.startTransfer(srcContainer, srcSlot, srcNetId, dstContainer, dstSlot,
                                       count, std::move(srcItemId));
#else
    (void)srcContainer;
    (void)srcSlot;
    (void)srcNetId;
    (void)dstContainer;
    (void)dstSlot;
    (void)count;
    (void)srcItemId;
    return false;
#endif
}

CraftJobStatusValue ItemStackRequests::transferStatus() const noexcept {
#ifdef _WIN32
    return fromNative(impl_->native.transferStatus());
#else
    return CraftJobStatusValue::Idle;
#endif
}

bool ItemStackRequests::startDrop(int slot, std::int32_t netId, int count,
                                  const std::string& item) {
#ifdef _WIN32
    return impl_->native.startDrop(slot, netId, count, item);
#else
    (void)slot;
    (void)netId;
    (void)count;
    (void)item;
    return false;
#endif
}

const std::string& ItemStackRequests::transferDetail() const noexcept {
#ifdef _WIN32
    return impl_->native.transferDetail();
#else
    return impl_->emptyDetail;
#endif
}

void ItemStackRequests::clearTransferStatus() {
#ifdef _WIN32
    impl_->native.clearTransferStatus();
#endif
}

bool ItemStackRequests::ingredientMatches(const CraftIngredientValue& spec,
                                          const std::string& itemName) {
#ifdef _WIN32
    return bedrock::NativeItemStackRequests::ingredientMatches(toNative(spec), itemName);
#else
    (void)spec;
    (void)itemName;
    return false;
#endif
}

} // namespace bedrock::sdk
