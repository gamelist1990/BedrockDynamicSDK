#pragma once
// NativeItemStackRequests — server-authoritative crafting for client mods.
//
// Forges native ItemStackRequest packets that replicate the real client's
// MANUAL craft flow, one request at a time, waiting for each
// ItemStackResponse before sending the next:
//
//   place  (inventory -> crafting grid, one request per ingredient cell)
//   craft  (one request: craft_recipe + results_deprecated + consume x cells
//           + take result -> cursor)
//   place  (cursor -> a free inventory slot, the WHOLE result stack)
//
// craft_recipe_auto is deliberately NOT used: a self-hosted BDS disconnects
// the client when it receives one (measured 2026-11-16, twice). The result
// descriptor is copied verbatim from the CraftingData packet's output entry
// (name supplied by the caller); inventing an "enriched" extra blob (e.g. a
// Damage NBT) made the server take ~47 s to answer pickaxe crafts, while the
// CraftingData bytes answer immediately.
//
// Two hard rules learned from live verification (CraftProbe, 2026-11-16):
//   - Never leave items on the cursor when a craft ends. A loaded cursor
//     makes every later craft fail with status 50, and the failure then
//     cascades into the ingredient places that follow.
//   - The client's own inventory model does NOT apply responses to requests
//     it did not originate. This adapter therefore tracks every slot it
//     touches from ItemStackResponses and overlays that state onto memory
//     snapshots; the overlay resets when a fresh CraftingData arrives (new
//     world join restacks everything).
//
// The packet's +0x10 field must borrow the connection pointer from a real
// outgoing packet of the same session; without it the send path drops the
// forged packet silently. The pointer is captured from the shared
// sendToServer hook (an ItemStackRequest packet is authoritative; until one
// is seen, the same value observed from two different packet types is used).
//
// Threading: PacketAPI callbacks run on networking threads and only store
// parsed data. All sends happen from update(), which the owning mod calls on
// its LevelTick thread.
#include <bedrock/profiles/profile_json.hpp>
#include <bedrock/native/packet_sender.hpp>
#include <bedrock/native/world_access.hpp>
#include <bedrock/packets/packet_api.hpp>
#include <bedrock/detail/native_profile_functions.hpp>
#include <bedrock/runtime/hook_runtime.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>
#if _ITERATOR_DEBUG_LEVEL != 0
#error Native item stack packets require the MSVC Release STL ABI.
#endif

namespace bedrock {

struct CraftIngredient {
    enum class Kind { Empty, Name, Tag };
    Kind kind{Kind::Empty};
    std::string value; // item name (Name) or tag name (Tag), namespaced
    int count{1};      // items consumed from this cell per craft
};

struct CraftRecipe {
    std::string recipeId;
    std::uint32_t networkId{};
    bool shaped{};
    int width{}, height{};                    // shaped pattern extent
    std::vector<CraftIngredient> cells;       // shaped: width*height row-major; shapeless: the ingredient list
    // Output as published by CraftingData (first output entry). The item
    // NAME is not on the wire (outputs are legacy numeric ids), so the
    // caller supplies it at craft time; everything else comes from here.
    std::int32_t outputLegacyId{};
    std::uint16_t outputCount{};
    std::int32_t outputMetadata{};
    std::uint32_t outputBlockRuntimeId{};
    std::string outputExtra; // raw bytes, copied verbatim into the descriptor
};

struct CraftInventorySlot {
    int slot{};
    std::string item;
    int count{};
    std::int32_t netId{};
};

enum class CraftJobStatus { Idle, Running, Done, Failed };

namespace item_stack_request_native {
// Exact 1.26.60.5 layouts, reverse engineered and verified against a
// self-hosted BDS (see data profile ItemStackRequest.json). The game borrows
// these objects only for the duration of the send dispatch.
struct RawVec { void* begin{}; void* end{}; void* cap{}; };
inline void putSlotInfo(std::byte* box, std::size_t off, std::uint32_t container,
                        std::uint8_t slot, std::int32_t netId) {
    std::memcpy(box + off + 0x00, &container, sizeof(container));
    box[off + 0x0C] = static_cast<std::byte>(slot);
    std::memcpy(box + off + 0x10, &netId, sizeof(netId));
    // discriminant at +0x20 stays 0 (case0: plain stack reference)
}
} // namespace item_stack_request_native

class NativeItemStackRequests {
public:
    using Diagnostic = std::function<void(const std::string&)>;
    NativeItemStackRequests() = default;
    ~NativeItemStackRequests() { stop(); }
    NativeItemStackRequests(const NativeItemStackRequests&) = delete;
    NativeItemStackRequests& operator=(const NativeItemStackRequests&) = delete;

    void onDiagnostic(Diagnostic diagnostic) { diagnostic_ = std::move(diagnostic); }

    bool load(const RuntimeProfile& profile, std::string& error) {
        if (loaded_) { error = "item stack requests already loaded"; return false; }
        try {
            if (!profile.resolver().exactBuild())
                throw std::runtime_error("exact executable fingerprint required");
            const auto root = bedrock::parseNativeProfile(profile.json());
            const auto& layout = root.at("layouts").at("ItemStackRequest");
            if (native_profile::number(layout.at("schema")) != 1 ||
                native_profile::number(layout.at("layout_revision")) != 1 ||
                native_profile::number(layout.at("pointer_size")) != 8 ||
                root.at("target").at("sha256").get<std::string>() !=
                    layout.at("module_sha256").get<std::string>())
                throw std::runtime_error("unsupported ItemStackRequest layout or identity");
            packetVtable_ = native_profile::number(layout.at("packet_vtable_rva"));
            requestDataVtable_ = native_profile::number(layout.at("request_data_vtable_rva"));
            base_ = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
            senderLayout_.load(root);
            world_.load(profile.json());
            loadItems(root);
            if (!items_.loaded) throw std::runtime_error("native_items layout unavailable");
            loaded_ = true;
            error.clear();
            return true;
        } catch (const std::exception& e) { error = e.what(); return false; }
    }

    // Registers the CraftingData / ItemStackResponse receive handlers and
    // installs the PacketAPI receive routes. Call once, after load().
    bool installPackets(const std::string& profileJson, std::string& error) {
        packets_.onReceive(PacketId::CraftingData,
                           [this](const PacketReadEvent& event) {
                               auto bytes = event.ownedPayload();
                               if (bytes)
                                   onCraftingData(
                                       reinterpret_cast<const std::byte*>(bytes->data()),
                                       bytes->size());
                           });
        packets_.onReceive(PacketId::ItemStackResponse,
                           [this](const PacketReadEvent& event) {
                               auto owned = event.ownedPayload();
                               if (owned)
                                   onItemStackResponse(
                                       reinterpret_cast<const std::byte*>(owned->data()),
                                       owned->size());
                           });
        if (!packets_.installReceive(profileJson, error)) return false;
        note("ItemStackRequest adapter: packet routes installed");
        return true;
    }

    void stop() {
        stopped_ = true;
        if (sendToken_) { BDS_HookUnsubscribe(std::exchange(sendToken_, nullptr)); }
    }

    // Sender observation + job pump. Call on the LevelTick thread.
    void update(void* client, void* level) {
        if (stopped_ || !loaded_) return;
        client_ = client;
        level_ = level;
        observeSender(client);
        pump();
    }

    bool senderReady() const noexcept { return sender_.load() != nullptr; }
    bool connectionReady() const noexcept { return capturedPlus10_.load() != nullptr; }
    bool ready() const noexcept { return loaded_ && senderReady() && connectionReady(); }
    bool recipesReady() const noexcept {
        std::lock_guard lock(recipesMutex_);
        return !recipes_.empty();
    }
    // Copy a recipe out of the table (the table itself is replaced wholesale
    // when a fresh CraftingData arrives, so pointers into it must not be
    // retained by callers).
    bool copyRecipe(const std::string& recipeId, CraftRecipe& out) const {
        std::lock_guard lock(recipesMutex_);
        auto it = recipes_.find(recipeId);
        if (it == recipes_.end()) return false;
        out = it->second;
        return true;
    }

    // The stack net id the (possibly frozen) client model shows for a
    // slot, 0 when the model has no stack there.
    std::int32_t memoryNetId(int slot) {
        void* player = world_.localPlayer(client_);
        if (!player) return 0;
        void* stack = itemStack(player, slot);
        if (!stack) return 0;
        std::uint8_t count{};
        if (!world_.readField(stack, items_.countOffset, count) || !count) return 0;
        std::int32_t netId{};
        readBytes(static_cast<const std::byte*>(stack) + items_.netIdOffset,
                  &netId, sizeof(netId));
        return netId;
    }

    // Memory snapshot of inventory slots 0..35 with the response-tracked
    // overlay applied (the overlay wins: the memory view is stale for every
    // slot a forged request has touched).
    std::vector<CraftInventorySlot> snapshot() {
        std::map<int, CraftInventorySlot> merged;
        if (void* player = world_.localPlayer(client_)) {
            for (int slot = 0; slot < 36; ++slot) {
                void* stack = itemStack(player, slot);
                if (!stack) continue;
                std::uint8_t count{};
                if (!world_.readField(stack, items_.countOffset, count) || !count) continue;
                std::int32_t netId{};
                readBytes(static_cast<const std::byte*>(stack) + items_.netIdOffset,
                          &netId, sizeof(netId));
                merged[slot] = CraftInventorySlot{slot, stackItemId(stack),
                                                  static_cast<int>(count), netId};
            }
        }
        std::lock_guard lock(shadowMutex_);
        for (auto it = shadow_.begin(); it != shadow_.end();) {
            const int slot = it->first;
            auto mem = merged.find(slot);
            const std::int32_t memNet =
                mem == merged.end() ? 0 : mem->second.netId;
            // The merged map holds exactly what the model shows (empty
            // slots are absent), except slots a still-valid entry below
            // has not overwritten yet — this loop is the only writer, so
            // reading `merged` here is the raw memory view.
            if (memNet != it->second.memNetId) {
                it = shadow_.erase(it); // gameplay replaced the stack
                continue;
            }
            merged[slot] = it->second.item;
            ++it;
        }
        std::vector<CraftInventorySlot> out;
        for (const auto& [slot, item] : merged)
            if (item.count > 0) out.push_back(item);
        return out;
    }

    // Queue a craft job: `times` consecutive single crafts of `recipe`,
    // depositing each result into the first free inventory slot.
    // gridBase/gridWidth: personal grid 28/2, crafting table grid 32/3.
    // The table grid only exists while the crafting table UI is open; the
    // caller is responsible for opening it (and keeping it open) first.
    bool startCraft(const CraftRecipe& recipe, std::string outputName, int times,
                    int gridBase, int gridWidth) {
        if (!ready() || status_ == CraftJobStatus::Running) return false;
        if (times < 1 || outputName.empty()) return false;
        Job job;
        job.recipe = recipe;
        job.outputName = std::move(outputName);
        job.timesLeft = times;
        job.base = gridBase;
        job.width = gridWidth;
        // Resolve the pattern into concrete grid cells.
        if (recipe.shaped) {
            const int gridHeight = gridBase == 28 ? 2 : 3;
            if (recipe.width > gridWidth || recipe.height > gridHeight) return false;
            for (int r = 0; r < recipe.height; ++r)
                for (int c = 0; c < recipe.width; ++c) {
                    const auto& spec = recipe.cells[static_cast<std::size_t>(r * recipe.width + c)];
                    if (spec.kind == CraftIngredient::Kind::Empty) continue;
                    job.cells.push_back(JobCell{gridBase + r * gridWidth + c, spec});
                }
        } else {
            if (static_cast<int>(recipe.cells.size()) > gridWidth * (gridBase == 28 ? 2 : 3))
                return false;
            int i = 0;
            for (const auto& spec : recipe.cells) {
                if (spec.kind == CraftIngredient::Kind::Empty) continue;
                job.cells.push_back(JobCell{gridBase + i, spec});
                ++i;
            }
        }
        if (job.cells.empty()) return false;
        job_ = std::move(job);
        status_ = CraftJobStatus::Running;
        detail_.clear();
        beginIteration();
        return status_ == CraftJobStatus::Running;
    }

    CraftJobStatus status() const noexcept { return status_; }
    const std::string& statusDetail() const noexcept { return detail_; }
    void clearStatus() {
        if (status_ == CraftJobStatus::Done || status_ == CraftJobStatus::Failed) {
            status_ = CraftJobStatus::Idle;
            detail_.clear();
        }
    }

    static bool ingredientMatches(const CraftIngredient& spec, const std::string& itemName) {
        using Kind = CraftIngredient::Kind;
        if (spec.kind == Kind::Name) return itemName == spec.value;
        if (spec.kind != Kind::Tag) return false;
        const auto shortName = [](const std::string& s) {
            const auto p = s.rfind(':');
            return p == std::string::npos ? s : s.substr(p + 1);
        };
        const std::string tag = shortName(spec.value);
        const std::string item = shortName(itemName);
        if (tag == "planks")
            return item == "planks" ||
                   (item.size() > 7 && item.compare(item.size() - 7, 7, "_planks") == 0);
        if (tag == "stone_tool_materials")
            return item == "cobblestone" || item == "stone" ||
                   item == "cobbled_deepslate" || item == "blackstone";
        if (item == tag) return true;
        return item.size() > tag.size() + 1 &&
               item.compare(item.size() - tag.size(), tag.size(), tag) == 0 &&
               item[item.size() - tag.size() - 1] == '_';
    }

private:
    // ---- profile-bound inventory reading (mirrors the verified probe) ----
    struct ItemLayout {
        std::size_t suppliesOffset{}, inventoryOffset{}, getItemSlot{};
        std::size_t itemRefOffset{}, countOffset{}, idOffset{}, strSizeOffset{}, strCapOffset{};
        std::size_t netIdOffset{0x80};
        bool loaded{};
    } items_;
    void loadItems(const nlohmann::json& root) {
        const auto& ni = root.at("native_items");
        const auto& lp = ni.at("local_player");
        const auto& inv = ni.at("inventory");
        const auto& st = ni.at("item_stack");
        const auto num = [](const nlohmann::json& j) -> std::size_t {
            return j.is_string() ? std::stoull(j.get<std::string>(), nullptr, 0)
                                 : j.get<std::size_t>();
        };
        items_.suppliesOffset = num(lp.at("supplies_offset"));
        items_.inventoryOffset = num(lp.at("inventory_offset"));
        items_.getItemSlot = num(inv.at("get_item_vtable_slot"));
        items_.itemRefOffset = num(st.at("item_reference_offset"));
        items_.countOffset = num(st.at("count_offset"));
        items_.idOffset = num(st.at("namespaced_id_string_offset"));
        items_.strSizeOffset = num(st.at("string_size_offset"));
        items_.strCapOffset = num(st.at("string_capacity_offset"));
        if (st.contains("stack_net_id_offset")) items_.netIdOffset = num(st.at("stack_net_id_offset"));
        items_.loaded = true;
    }
    static bool readBytes(const void* address, void* out, std::size_t size) noexcept {
        SIZE_T copied{};
        return ReadProcessMemory(GetCurrentProcess(), address, out, size, &copied) &&
               copied == size;
    }
    void* itemStack(void* player, int slot) {
        void* supplies{}; void* inventory{}; void* vt{}; void* fn{};
        if (!world_.readField(player, items_.suppliesOffset, supplies) || !supplies) return nullptr;
        if (!world_.readField(supplies, items_.inventoryOffset, inventory) || !inventory)
            return nullptr;
        if (!world_.readField(inventory, 0, vt) || !vt) return nullptr;
        if (!world_.readField(static_cast<const void*>(vt), items_.getItemSlot * 8, fn) || !fn)
            return nullptr;
        void* stack{};
        __try { stack = reinterpret_cast<void*(__fastcall*)(void*, int)>(fn)(inventory, slot); }
        __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
        return stack;
    }
    std::string stackItemId(void* stack) {
        void* itemRef{};
        if (!world_.readField(stack, items_.itemRefOffset, itemRef) || !itemRef) return {};
        void* item{};
        if (!world_.readField(itemRef, 0, item) || !item) return {};
        const auto baseOff = items_.idOffset;
        std::size_t size{}, cap{};
        if (!world_.readField(item, baseOff + items_.strSizeOffset, size) ||
            !world_.readField(item, baseOff + items_.strCapOffset, cap) ||
            size == 0 || size > 256 || cap < size || cap > 0x100000)
            return {};
        const void* data = static_cast<const std::byte*>(item) + baseOff;
        if (cap > 15) {
            const void* ptr{};
            if (!world_.readField(item, baseOff, ptr) || !ptr) return {};
            data = ptr;
        }
        std::string out;
        out.reserve(size);
        for (std::size_t i = 0; i < size; ++i) {
            char c{};
            if (!world_.readField(data, i, c)) return {};
            out.push_back(c);
        }
        return out;
    }

    // ---- sender observation + connection pointer capture ------------------
    void observeSender(void* client) {
        void* sender{}; void* target{};
        if (!client || !level_ || !senderLayout_.resolve(client, sender, target)) {
            sender_ = nullptr;
            return;
        }
        if (sendToken_ && target != sendTarget_) { sender_ = nullptr; return; }
        if (!sendToken_) {
            if (!BDS_HookSubscribe(BDS_HOOK_PACKET_SEND_TO_SERVER, target, &onSend, this,
                                   &sendToken_))
                return;
            sendTarget_ = target;
        }
        sender_ = sender;
    }
    static void BDS_HOOK_CALL onSend(void* user, void* raw, BDS_HookNext next,
                                     void* frame) noexcept {
        next(frame, raw);
        auto& self = *static_cast<NativeItemStackRequests*>(user);
        if (self.stopped_) return;
        const auto& args = *static_cast<BDS_PacketSendArgs*>(raw);
        if (!args.packet || args.sender != self.sender_.load()) return;
        void* vtable{};
        if (!readBytes(args.packet, &vtable, sizeof(vtable)) || !vtable) return;
        void* plus10{};
        if (!readBytes(static_cast<const std::byte*>(args.packet) + 0x10, &plus10,
                       sizeof(plus10)) || !plus10)
            return;
        const auto vt = reinterpret_cast<std::uintptr_t>(vtable);
        if (vt == self.base_ + self.packetVtable_) {
            self.capturedPlus10_.store(plus10); // authoritative: a real 147
            // A request whose id we did not issue is the real client (or
            // the player through the UI) changing the inventory: our
            // response-tracked overlay may go stale, and the client model
            // resyncs through the screen such an interaction implies, so
            // drop the overlay and trust memory again.
            std::int32_t firstId{};
            const void* reqBegin{};
            if (readBytes(static_cast<const std::byte*>(args.packet) + 0x30, &reqBegin, 8) &&
                reqBegin &&
                readBytes(static_cast<const std::byte*>(reqBegin) + 0x08, &firstId, 4) &&
                !self.isOurRequestId(firstId)) {
                std::lock_guard lock(self.shadowMutex_);
                self.shadow_.clear();
            }
            return;
        }
        // Consensus fallback: the +0x10 connection pointer is a Packet base
        // field, so unrelated packet types carry the same value. Trust a
        // value once two different packet types agree on it.
        std::lock_guard lock(self.consensusMutex_);
        if (self.capturedPlus10_.load()) return;
        auto it = self.consensus_.find(plus10);
        if (it == self.consensus_.end()) {
            self.consensus_.emplace(plus10, std::vector<std::uintptr_t>{vt});
        } else if (std::find(it->second.begin(), it->second.end(), vt) == it->second.end()) {
            it->second.push_back(vt);
            if (it->second.size() >= 2) self.capturedPlus10_.store(plus10);
        }
    }

    // ---- packet construction ----------------------------------------------
    std::int32_t nextRequestId() { return requestId_.fetch_sub(2) - 2; }
    // Issued ids are the odd negatives from the initial -99 downwards.
    // nextRequestId() returns counter-2 and leaves the counter AT the
    // issued id, so the issued set is {counter, counter+2, ..., -101}.
    bool isOurRequestId(std::int32_t id) const {
        return id <= -101 && id >= requestId_.load() && (id & 1) != 0;
    }
    std::byte* makeRequest(std::int32_t requestId, std::byte* actions, std::size_t actionCount) {
        using namespace item_stack_request_native;
        auto* request = new std::byte[0x48]{};
        void* vtable = reinterpret_cast<void*>(base_ + requestDataVtable_);
        std::memcpy(request + 0x00, &vtable, sizeof(vtable));
        std::memcpy(request + 0x08, &requestId, sizeof(requestId));
        const RawVec actionsVec{actions, actions + actionCount * 0x60,
                                actions + actionCount * 0x60};
        std::memcpy(request + 0x10, &actionsVec, sizeof(actionsVec));
        const std::int32_t minusOne = -1;
        std::memcpy(request + 0x40, &minusOne, sizeof(minusOne));
        return request;
    }
    void dispatchRequests(std::byte* requests, std::size_t count) {
        using namespace item_stack_request_native;
        alignas(8) std::array<std::byte, 0x50> packet{};
        void* packetVtable = reinterpret_cast<void*>(base_ + packetVtable_);
        std::memcpy(packet.data() + 0x00, &packetVtable, sizeof(packetVtable));
        const std::uint64_t pr = 0x100000002ull;
        std::memcpy(packet.data() + 0x08, &pr, sizeof(pr));
        if (void* plus10 = capturedPlus10_.load())
            std::memcpy(packet.data() + 0x10, &plus10, sizeof(plus10));
        const RawVec vec{requests, requests + count * 0x48, requests + count * 0x48};
        std::memcpy(packet.data() + 0x30, &vec, sizeof(vec));
        const std::uint32_t version = 5;
        std::memcpy(packet.data() + 0x48, &version, sizeof(version));
        BDS_PacketSendArgs args{sender_.load(), packet.data()};
        BDS_HookDispatch(BDS_HOOK_PACKET_SEND_TO_SERVER, &args);
    }
    void putSlotInfoResult(std::byte* box, std::size_t off, std::int32_t requestId) {
        // case1 (result take): +0x10 holds the RequestData vtable address
        // value itself, +0x18 the owning request id, discriminant 1.
        const std::uint32_t container = 60;
        std::memcpy(box + off + 0x00, &container, sizeof(container));
        box[off + 0x0C] = static_cast<std::byte>(50);
        void* vtableValue = reinterpret_cast<void*>(base_ + requestDataVtable_);
        std::memcpy(box + off + 0x10, &vtableValue, sizeof(vtableValue));
        std::memcpy(box + off + 0x18, &requestId, sizeof(requestId));
        box[off + 0x20] = static_cast<std::byte>(1);
    }

    // ---- job / response state ----------------------------------------------
    struct JobCell {
        int gridSlot{};
        CraftIngredient ingredient;
    };
    struct Job {
        CraftRecipe recipe;
        std::string outputName;
        int timesLeft{};
        int base{};
        int width{};
        std::vector<JobCell> cells;
        std::map<int, CraftInventorySlot> inv;
        std::size_t cell{};
        int srcSlot{};
        std::vector<std::pair<int, std::int32_t>> grid; // grid slot -> stack id after place
        std::int32_t resultStack{};
        int resultCount{};
        int depSlot{};
        std::int32_t pending{};
        int phase{}; // 0 = placing ingredients, 1 = craft sent, 2 = deposit sent
        std::chrono::steady_clock::time_point sentAt{};
    };
    struct RespSlot {
        std::uint32_t container{};
        std::uint8_t slot{};
        std::uint8_t count{};
        std::int32_t stackId{};
    };
    struct RespInfo {
        std::uint8_t status{};
        std::vector<RespSlot> slots;
    };
    bool takeResponse(std::int32_t id, RespInfo& out) {
        std::lock_guard lock(respMutex_);
        auto it = responses_.find(id);
        if (it == responses_.end()) return false;
        out = std::move(it->second);
        responses_.erase(it);
        return true;
    }

    // ---- wire reading --------------------------------------------------------
    struct Wire {
        const std::byte* p{};
        std::size_t left{};
        explicit Wire(const std::byte* data, std::size_t size) : p(data), left(size) {}
        std::uint8_t u8() {
            if (left < 1) throw std::runtime_error("wire overrun");
            --left;
            return static_cast<std::uint8_t>(*p++);
        }
        std::uint32_t varuint() {
            std::uint32_t result = 0;
            int shift = 0;
            for (;;) {
                const std::uint8_t b = u8();
                result |= static_cast<std::uint32_t>(b & 0x7F) << shift;
                if (!(b & 0x80)) return result;
                shift += 7;
                if (shift > 28) throw std::runtime_error("varint too long");
            }
        }
        std::int32_t zigzag() {
            const std::uint32_t v = varuint();
            return static_cast<std::int32_t>((v >> 1) ^ (0u - (v & 1u)));
        }
        std::uint16_t lu16() {
            const std::uint32_t lo = u8();
            return static_cast<std::uint16_t>(lo | (static_cast<std::uint32_t>(u8()) << 8));
        }
        std::int16_t li16() { return static_cast<std::int16_t>(lu16()); }
        std::string bytes(std::size_t n) {
            if (left < n) throw std::runtime_error("wire overrun");
            std::string out(reinterpret_cast<const char*>(p), n);
            p += n;
            left -= n;
            return out;
        }
        std::string str() { return bytes(varuint()); }
        void skip(std::size_t n) { (void)bytes(n); }
    };

    // ItemStackResponse: {count, [status, requestId zigzag, (success only:
    // flag=1, containers...)]}. Error responses carry no containers at all
    // (measured 6-byte payloads), so a nonzero status is stored at once.
    void onItemStackResponse(const std::byte* data, std::size_t size) {
        try {
            Wire w(data, size);
            const std::uint32_t count = w.varuint();
            for (std::uint32_t i = 0; i < count; ++i) {
                RespInfo info;
                info.status = w.u8();
                const std::int32_t reqId = w.zigzag();
                if (info.status != 0) {
                    std::lock_guard lock(respMutex_);
                    responses_[reqId] = std::move(info);
                    return;
                }
                (void)w.u8(); // success flag (1)
                const std::uint32_t nContainers = w.varuint();
                for (std::uint32_t c = 0; c < nContainers; ++c) {
                    const std::uint32_t container = w.u8();
                    if (w.u8()) (void)w.varuint(); // dynamic container id
                    const std::uint32_t nSlots = w.varuint();
                    for (std::uint32_t sIdx = 0; sIdx < nSlots; ++sIdx) {
                        RespSlot slot;
                        slot.container = container;
                        slot.slot = w.u8();
                        (void)w.u8(); // hotbar slot
                        slot.count = w.u8();
                        (void)w.u8(); // flag
                        slot.stackId = w.zigzag();
                        (void)w.str(); // custom name
                        (void)w.str(); // filtered custom name
                        if (slot.stackId != 0) (void)w.zigzag(); // durability
                        info.slots.push_back(slot);
                    }
                }
                std::lock_guard lock(respMutex_);
                responses_[reqId] = std::move(info);
            }
        } catch (const std::exception& e) {
            note(std::string("ItemStackResponse parse failed: ") + e.what());
        }
    }

    // ---- CraftingData -> recipe table ---------------------------------------
    static CraftIngredient wireIngredient(Wire& w) {
        CraftIngredient out;
        const std::uint32_t mapper = w.varuint();
        if (mapper == 0) {
            (void)w.zigzag(); // invalid descriptor metadata
            out.kind = CraftIngredient::Kind::Empty;
        } else {
            const std::string dt = w.str();
            if (dt == "name") {
                out.value = w.str();
                (void)w.zigzag();
                out.kind = CraftIngredient::Kind::Name;
            } else if (dt == "item_tag") {
                out.value = w.str();
                (void)w.zigzag();
                out.kind = CraftIngredient::Kind::Tag;
            } else if (dt == "molang") {
                (void)w.str();
                (void)w.li16();
                out.kind = CraftIngredient::Kind::Empty; // never matches
            } else { // "empty" and anything unknown
                (void)w.zigzag();
                out.kind = CraftIngredient::Kind::Empty;
            }
        }
        out.count = w.zigzag();
        return out;
    }
    struct WireOutput {
        std::int32_t legacyId{};
        std::uint16_t count{};
        std::int32_t metadata{};
        std::uint32_t blockRuntimeId{};
        std::string extra;
    };
    static WireOutput wireOutput(Wire& w) {
        WireOutput out;
        out.legacyId = w.zigzag();
        out.count = w.lu16();
        out.metadata = w.zigzag();
        out.blockRuntimeId = static_cast<std::uint32_t>(w.zigzag());
        out.extra = w.str();
        return out;
    }
    static void wireUnlocking(Wire& w) {
        if (!w.u8()) return;
        (void)w.zigzag(); // context
        if (w.u8()) {
            const std::uint32_t n = w.varuint();
            for (std::uint32_t i = 0; i < n; ++i) (void)wireIngredient(w);
        }
    }
    static void finishRecipe(CraftRecipe& r, const std::vector<WireOutput>& outputs,
                             std::uint32_t networkId) {
        r.networkId = networkId;
        if (!outputs.empty()) {
            r.outputLegacyId = outputs[0].legacyId;
            r.outputCount = outputs[0].count;
            r.outputMetadata = outputs[0].metadata;
            r.outputBlockRuntimeId = outputs[0].blockRuntimeId;
            r.outputExtra = outputs[0].extra;
        }
    }
    void onCraftingData(const std::byte* data, std::size_t size) {
        try {
            Wire w(data, size);
            std::map<std::string, CraftRecipe> table;
            const std::uint32_t nShaped = w.varuint();
            for (std::uint32_t i = 0; i < nShaped; ++i) {
                CraftRecipe r;
                r.shaped = true;
                r.recipeId = w.str();
                r.width = w.zigzag();
                r.height = w.zigzag();
                const std::uint32_t nCells = w.varuint();
                for (std::uint32_t c = 0; c < nCells; ++c)
                    r.cells.push_back(wireIngredient(w));
                const std::uint32_t nOut = w.varuint();
                std::vector<WireOutput> outputs;
                for (std::uint32_t o = 0; o < nOut; ++o) outputs.push_back(wireOutput(w));
                w.skip(16); // uuid
                (void)w.str(); // crafting block tag
                (void)w.zigzag(); // priority
                (void)w.u8(); // assume symmetry
                wireUnlocking(w);
                finishRecipe(r, outputs, w.varuint());
                table[r.recipeId] = std::move(r);
            }
            const std::uint32_t nShapeless = w.varuint();
            for (std::uint32_t i = 0; i < nShapeless; ++i) {
                CraftRecipe r;
                r.shaped = false;
                r.recipeId = w.str();
                const std::uint32_t nIng = w.varuint();
                for (std::uint32_t c = 0; c < nIng; ++c)
                    r.cells.push_back(wireIngredient(w));
                const std::uint32_t nOut = w.varuint();
                std::vector<WireOutput> outputs;
                for (std::uint32_t o = 0; o < nOut; ++o) outputs.push_back(wireOutput(w));
                w.skip(16); // uuid
                (void)w.str(); // crafting block tag
                (void)w.zigzag(); // priority
                wireUnlocking(w);
                finishRecipe(r, outputs, w.varuint());
                table[r.recipeId] = std::move(r);
            }
            std::size_t built = 0;
            {
                std::lock_guard lock(recipesMutex_);
                recipes_ = std::move(table);
                built = recipes_.size();
            }
            // A fresh CraftingData means a (re)join: the inventory model is
            // back in sync with the server, so the response overlay resets.
            {
                std::lock_guard lock(shadowMutex_);
                shadow_.clear();
            }
            note("recipe table built (" + std::to_string(built) + " recipes)");
        } catch (const std::exception& e) {
            note(std::string("CraftingData parse failed: ") + e.what());
        }
    }

    void failJob(const std::string& why) {
        detail_ = why;
        status_ = CraftJobStatus::Failed;
        job_.pending = 0;
        note("craft failed: " + why);
    }
    void beginIteration() {
        job_.inv.clear();
        for (const auto& slot : snapshot()) job_.inv[slot.slot] = slot;
        job_.cell = 0;
        job_.grid.clear();
        job_.resultStack = 0;
        job_.resultCount = 0;
        sendPlace();
    }
    void sendPlace() {
        using namespace item_stack_request_native;
        const JobCell& cellDef = job_.cells[job_.cell];
        int src = -1;
        for (const auto& [slot, item] : job_.inv)
            if (item.count > 0 && ingredientMatches(cellDef.ingredient, item.item)) {
                src = slot;
                break;
            }
        if (src < 0) {
            failJob("ingredient not in inventory: " + cellDef.ingredient.value);
            return;
        }
        job_.srcSlot = src;
        const std::int32_t srcNet = job_.inv[src].netId;
        const int placeCount = std::min(cellDef.ingredient.count, job_.inv[src].count);
        auto* actions = new std::byte[0x60]{};
        actions[0x00] = static_cast<std::byte>(placeCount);
        putSlotInfo(actions, 0x08, 12, static_cast<std::uint8_t>(src), srcNet);
        putSlotInfo(actions, 0x30, 13, static_cast<std::uint8_t>(cellDef.gridSlot), 0);
        actions[0x58] = static_cast<std::byte>(1); // place
        const std::int32_t reqId = nextRequestId();
        auto* request = makeRequest(reqId, actions, 1);
        dispatchRequests(request, 1);
        delete[] actions;
        delete[] request;
        job_.inv[src].count -= placeCount; // provisional; the response overwrites it
        job_.pending = reqId;
        job_.sentAt = std::chrono::steady_clock::now();
        job_.phase = 0;
    }
    void sendCraft() {
        using namespace item_stack_request_native;
        const CraftRecipe& r = job_.recipe;
        const std::size_t nActions = 2 + job_.grid.size() + 1;
        auto* actions = new std::byte[0x60 * nActions]{};
        const std::int32_t reqId = nextRequestId();
        std::byte* a0 = actions; // craft_recipe
        std::memcpy(a0 + 0x00, &r.networkId, sizeof(r.networkId));
        a0[0x04] = static_cast<std::byte>(1);
        a0[0x58] = static_cast<std::byte>(10);
        std::byte* a1 = actions + 0x60; // results_deprecated
        auto* desc = new std::byte[0x58]{};
        new (desc + 0x00) std::string(job_.outputName);
        std::memcpy(desc + 0x20, &r.outputMetadata, sizeof(r.outputMetadata));
        desc[0x28] = static_cast<std::byte>(1); // variant: name
        std::memcpy(desc + 0x30, &r.outputCount, sizeof(r.outputCount));
        std::memcpy(desc + 0x34, &r.outputBlockRuntimeId, sizeof(r.outputBlockRuntimeId));
        new (desc + 0x38) std::string(r.outputExtra);
        const RawVec descVec{desc, desc + 0x58, desc + 0x58};
        std::memcpy(a1 + 0x00, &descVec, sizeof(descVec));
        a1[0x18] = static_cast<std::byte>(1); // times_crafted
        a1[0x58] = static_cast<std::byte>(18);
        std::size_t ai = 2;
        for (const auto& [gridSlot, stackId] : job_.grid) { // consume x cells
            std::byte* a = actions + ai++ * 0x60;
            a[0x00] = static_cast<std::byte>(1);
            putSlotInfo(a, 0x08, 13, static_cast<std::uint8_t>(gridSlot), stackId);
            a[0x58] = static_cast<std::byte>(5);
        }
        std::byte* at = actions + ai * 0x60; // take the WHOLE result -> cursor
        at[0x00] = static_cast<std::byte>(r.outputCount & 0xFF);
        putSlotInfoResult(at, 0x08, reqId);
        putSlotInfo(at, 0x30, 59, 0, 0);
        at[0x58] = static_cast<std::byte>(0);
        auto* request = makeRequest(reqId, actions, nActions);
        dispatchRequests(request, 1);
        reinterpret_cast<std::string*>(desc + 0x00)->~basic_string();
        reinterpret_cast<std::string*>(desc + 0x38)->~basic_string();
        delete[] desc;
        delete[] actions;
        delete[] request;
        job_.pending = reqId;
        job_.sentAt = std::chrono::steady_clock::now();
        job_.phase = 1;
    }
    void sendDeposit() {
        using namespace item_stack_request_native;
        // First free slot in the tracked inventory (hotbar slots come first
        // in 0..35 order). Depositing onto a partial stack of the same item
        // would also work, but an empty slot keeps the shadow bookkeeping
        // exact.
        job_.depSlot = -1;
        for (int s = 0; s < 36; ++s) {
            auto it = job_.inv.find(s);
            if (it == job_.inv.end() || it->second.count == 0) { job_.depSlot = s; break; }
        }
        if (job_.depSlot < 0) { failJob("no free inventory slot for the result"); return; }
        auto* actions = new std::byte[0x60]{};
        // Move the WHOLE result stack off the cursor (see header comment).
        actions[0x00] = static_cast<std::byte>(job_.resultCount & 0xFF);
        putSlotInfo(actions, 0x08, 59, 0, job_.resultStack);
        putSlotInfo(actions, 0x30, 12, static_cast<std::uint8_t>(job_.depSlot), 0);
        actions[0x58] = static_cast<std::byte>(1); // place
        const std::int32_t reqId = nextRequestId();
        auto* request = makeRequest(reqId, actions, 1);
        dispatchRequests(request, 1);
        delete[] actions;
        delete[] request;
        job_.pending = reqId;
        job_.sentAt = std::chrono::steady_clock::now();
        job_.phase = 2;
    }
    void advanceJob(const RespInfo& resp) {
        if (resp.status != 0) {
            failJob("response status=" + std::to_string(resp.status) +
                    " phase=" + std::to_string(job_.phase));
            return;
        }
        if (job_.phase == 0) {
            const int gridSlot = job_.cells[job_.cell].gridSlot;
            bool foundGrid = false;
            for (const auto& s : resp.slots) {
                if (s.container == 13 && static_cast<int>(s.slot) == gridSlot) {
                    job_.grid.emplace_back(gridSlot, s.stackId);
                    foundGrid = true;
                }
                if (s.container == 12 && static_cast<int>(s.slot) == job_.srcSlot) {
                    auto& item = job_.inv[job_.srcSlot];
                    item.count = static_cast<int>(s.count);
                    item.netId = s.stackId;
                    std::lock_guard lock(shadowMutex_);
                    shadow_[job_.srcSlot] = ShadowEntry{item, memoryNetId(job_.srcSlot)};
                }
            }
            if (!foundGrid) { failJob("place response missing grid entry"); return; }
            job_.cell++;
            if (job_.cell < job_.cells.size()) sendPlace();
            else sendCraft();
        } else if (job_.phase == 1) {
            bool found = false;
            for (const auto& s : resp.slots)
                if (s.container == 59) {
                    job_.resultStack = s.stackId;
                    job_.resultCount = s.count;
                    found = true;
                }
            if (!found) { failJob("craft response missing cursor entry"); return; }
            sendDeposit();
        } else {
            CraftInventorySlot deposited{job_.depSlot, job_.outputName,
                                         static_cast<int>(job_.resultCount), job_.resultStack};
            for (const auto& s : resp.slots)
                if (s.container == 12 && static_cast<int>(s.slot) == job_.depSlot) {
                    deposited.count = static_cast<int>(s.count);
                    deposited.netId = s.stackId;
                }
            {
                std::lock_guard lock(shadowMutex_);
                shadow_[job_.depSlot] = ShadowEntry{deposited, memoryNetId(job_.depSlot)};
            }
            if (--job_.timesLeft > 0) { beginIteration(); return; }
            status_ = CraftJobStatus::Done;
            detail_ = job_.outputName;
            job_.pending = 0;
            note("craft done: " + job_.outputName);
        }
    }
    void pump() {
        if (status_ != CraftJobStatus::Running || !job_.pending) return;
        RespInfo resp;
        if (takeResponse(job_.pending, resp)) {
            job_.pending = 0;
            advanceJob(resp);
        } else if (std::chrono::steady_clock::now() - job_.sentAt > std::chrono::seconds(90)) {
            failJob("timeout waiting for ItemStackResponse");
        }
    }

    void note(const std::string& text) { if (diagnostic_) diagnostic_(text); }

    PacketAPI packets_;
    NativePacketSender senderLayout_;
    NativeWorldAccess world_;
    Diagnostic diagnostic_;
    bool loaded_{};
    bool stopped_{};
    void* client_{};
    void* level_{};
    void* sendTarget_{};
    BDS_HookToken sendToken_{};
    std::atomic<void*> sender_{};
    std::atomic<void*> capturedPlus10_{};
    std::uintptr_t base_{};
    std::uintptr_t packetVtable_{};
    std::uintptr_t requestDataVtable_{};
    std::atomic<std::int32_t> requestId_{-99};
    std::mutex consensusMutex_;
    std::map<void*, std::vector<std::uintptr_t>> consensus_;
    std::mutex respMutex_;
    std::map<std::int32_t, RespInfo> responses_;
    mutable std::mutex recipesMutex_;
    std::map<std::string, CraftRecipe> recipes_;
    std::mutex shadowMutex_;
    // A shadow entry is only trusted while the client model's stack id
    // for that slot still equals the baseline captured when the entry
    // was written. The model is frozen for forged-craft results (so the
    // baseline holds), but any gameplay change to the slot — placing
    // the crafted block, a pickup landing there, the tool breaking —
    // replaces the stack id and retires the entry.
    struct ShadowEntry {
        CraftInventorySlot item;
        std::int32_t memNetId{};
    };
    std::map<int, ShadowEntry> shadow_;
    Job job_;
    CraftJobStatus status_{CraftJobStatus::Idle};
    std::string detail_;
};

} // namespace bedrock
