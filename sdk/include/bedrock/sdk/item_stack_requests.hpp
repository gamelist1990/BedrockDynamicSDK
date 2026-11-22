#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "block_types.hpp"
#include "profiles.hpp"
#include "world_access.hpp"

namespace bedrock::sdk {

// One cell of a recipe pattern. Mirrors the native CraftIngredient.
struct CraftIngredientValue {
    enum class Kind { Empty, Name, Tag };
    Kind kind{Kind::Empty};
    std::string value; // item name (Name) or tag name (Tag), namespaced
    int count{1};      // items consumed from this cell per craft
};

// A recipe copied out of the CraftingData table. Mirrors the native
// CraftRecipe field for field; the output item NAME is not on the wire,
// so the caller supplies it at craft time.
struct CraftRecipeValue {
    std::string recipeId;
    std::uint32_t networkId{};
    bool shaped{};
    int width{}, height{};
    std::vector<CraftIngredientValue> cells;
    std::int32_t outputLegacyId{};
    std::uint16_t outputCount{};
    std::int32_t outputMetadata{};
    std::uint32_t outputBlockRuntimeId{};
    std::string outputExtra;
};

// One inventory slot as the crafting engine sees it (memory model
// reconciled with the server shadow overlay in snapshot()).
struct CraftInventorySlotValue {
    int slot{};
    std::string item;
    int count{};
    std::int32_t netId{};
    int damage{-1}; // native aux; interpreted as wear only for tools
};

enum class CraftJobStatusValue { Idle, Running, Done, Failed };

// Containers the engine can open without a visible screen.
enum class BackgroundKindValue { Inventory, Table, Furnace };

// --------------------------------------------------------------------------
// ItemStackRequests — public facade over the native ItemStackRequest
// crafting engine. It builds real ItemStackRequest packets (craft,
// transfer, drop) and reconciles the server's ItemStackResponse against a
// shadow inventory, and it can open inventory/crafting-table/furnace
// containers in the background. Jobs are pumped from update() on the
// LevelTick thread; callers poll status()/transferStatus().
// --------------------------------------------------------------------------
class ItemStackRequests final {
public:
    using Diagnostic = std::function<void(const std::string&)>;

    ItemStackRequests();
    ~ItemStackRequests();
    ItemStackRequests(const ItemStackRequests&) = delete;
    ItemStackRequests& operator=(const ItemStackRequests&) = delete;
    ItemStackRequests(ItemStackRequests&&) noexcept;
    ItemStackRequests& operator=(ItemStackRequests&&) noexcept;

    void onDiagnostic(Diagnostic diagnostic);

    // Bind the exact-build ItemStackRequest layout from the profile.
    bool load(const Profile& profile, std::string& error);
    // Register the CraftingData / ItemStackResponse receive routes and
    // the send-side wire hook. Call once, after load().
    bool installPackets(const Profile& profile, std::string& error);
    void stop();

    // Sender observation + job pump. Call on the LevelTick thread.
    void update(void* client, void* level, bool allowWork = true);
    bool senderReady() const noexcept;
    bool connectionReady() const noexcept;
    bool ready() const noexcept;

    // Background containers.
    bool backgroundSupported() const noexcept;
    bool backgroundActive() const;
    bool backgroundFailed() const;
    bool backgroundOpen(BackgroundKindValue kind) const;
    bool backgroundArm(BackgroundKindValue kind, BlockPos position);
    bool backgroundUse(BackgroundKindValue kind, BlockPos block, unsigned face,
                       Vec3Value eye, Vec3Value hit);
    bool backgroundInventory();
    bool backgroundClose();

    void cancelJobs();
    void worldChanged();

    // Recipes (the table is replaced wholesale on new CraftingData, so
    // recipes are copied out, never referenced).
    bool recipesReady() const noexcept;
    bool copyRecipe(const std::string& recipeId, CraftRecipeValue& out) const;

    // Inventory views.
    std::int32_t memoryNetId(int slot);
    std::int32_t memoryCount(int slot);
    std::vector<CraftInventorySlotValue> memorySnapshot(bool includeEmpty = false,
                                                        int firstSlot = 0, int slotCount = 36);
    std::vector<CraftInventorySlotValue> snapshot(bool includeEmpty = false);
    void invalidateInventorySlot(int slot);

    // Craft job: `times` consecutive single crafts, results deposited
    // into the first free inventory slot. gridBase/gridWidth: personal
    // grid 28/2, crafting table grid 32/3 (table UI must be open).
    bool startCraft(const CraftRecipeValue& recipe, std::string outputName, int times,
                    int gridBase, int gridWidth);
    CraftJobStatusValue status() const noexcept;
    const std::string& statusDetail() const noexcept;
    void clearStatus();
    // Post-failure reset: drops the shadow overlay so a retry does not
    // place into the grid the failed job left occupied.
    void resetAfterFailure();

    // One-shot place transfer between arbitrary containers.
    bool startTransfer(std::uint32_t srcContainer, int srcSlot, std::int32_t srcNetId,
                       std::uint32_t dstContainer, int dstSlot, int count,
                       std::string srcItemId);
    CraftJobStatusValue transferStatus() const noexcept;
    bool startDrop(int slot, std::int32_t netId, int count, const std::string& item);
    const std::string& transferDetail() const noexcept;
    void clearTransferStatus();

    static bool ingredientMatches(const CraftIngredientValue& spec, const std::string& itemName);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace bedrock::sdk
