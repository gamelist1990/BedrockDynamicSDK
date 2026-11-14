// AutoMine — Baritone-style automatic mining for the Bedrock client.
//
//   /mine <block-id>   find, walk to and mine nearby blocks, one after another
//   /mine stop         stop and release every synthetic input
//   /mine status       report the current state in chat
//
// Movement and breaking go through the game's own input routes (native key
// sends, mouse attack hold, view turning), so the server sees an ordinary
// player: walking, turning, and holding attack on the targeted block.
// AutoTool picks the best hotbar tool for each target block. When no usable
// tool is in the hotbar, the AutoCraft planner works out the crafting chain
// from the visible inventory, tells the player exactly what to make, and
// resumes on its own once a suitable tool appears in the hotbar.
#ifdef _WIN32
#include <bedrock/runtime/game_hooks.hpp>
#include <bedrock/api/game_api.hpp>
#include <bedrock/native/world_access.hpp>
#include <bedrock/native/camera.hpp>
#include <bedrock/native/safe_teleport.hpp>
#include <bedrock/native/packet_sender.hpp>
#include <bedrock/profiles/profile_json.hpp>
#include <bedrock/runtime/hook_runtime.h>
#include <knownfolders.h>
#include <shlobj.h>
#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <functional>
#include <deque>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <unordered_map>

#if !defined(_MSC_VER) || _ITERATOR_DEBUG_LEVEL != 0
#error AutoMine requires MSVC Release STL ABI.
#endif

namespace {
using namespace std::chrono_literals;

// Win32 virtual keys, matching the values the native HID route uses.
constexpr unsigned kKeyW = 0x57, kKeyA = 0x41, kKeyS = 0x53, kKeyD = 0x44;
constexpr unsigned kKeySpace = 0x20;

std::string lower(std::string value) {
    for (auto& c : value) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return value;
}
// "minecraft:iron_ore" -> "iron_ore"; leaves other namespaces untouched textually.
std::string localPart(std::string id) {
    id = lower(std::move(id));
    const auto colon = id.find(':');
    return colon == std::string::npos ? id : id.substr(colon + 1);
}
bool isAir(const std::string& id) {
    const auto local = localPart(id);
    return local.empty() || local == "air" || local == "cave_air" || local == "void_air";
}
bool contains(const std::string& text, std::string_view needle) {
    return text.find(needle) != std::string::npos;
}

// ---------------------------------------------------------------------------
// Tool knowledge
// ---------------------------------------------------------------------------
enum class ToolClass { None, Pickaxe, Axe, Shovel, Hoe, Shears };

ToolClass classForBlock(const std::string& rawId) {
    const auto id = localPart(rawId);
    if (id.empty() || isAir(id)) return ToolClass::None;
    // Pickaxe first: "sandstone" must not fall into the shovel "sand" rule.
    if (contains(id, "_ore") || id == "obsidian" || id == "ancient_debris" ||
        contains(id, "stone") || contains(id, "deepslate") || contains(id, "netherrack") ||
        contains(id, "basalt") || contains(id, "blackstone") || contains(id, "brick") ||
        contains(id, "quartz") || contains(id, "prismarine") || contains(id, "terracotta") ||
        contains(id, "concrete") || id == "ice" || id == "packed_ice" || id == "blue_ice" ||
        contains(id, "amethyst") || contains(id, "copper") || contains(id, "iron") ||
        contains(id, "gold_block") || contains(id, "diamond_block") || contains(id, "emerald") ||
        contains(id, "lapis") || contains(id, "redstone_block") || contains(id, "furnace") ||
        contains(id, "anvil") || contains(id, "enchanting_table") || contains(id, "brewing_stand") ||
        contains(id, "hopper") || contains(id, "rails") || id == "rail" || contains(id, "_rail") ||
        contains(id, "lantern") || contains(id, "chain") || id == "glass" || contains(id, "_glass") ||
        contains(id, "end_stone") || contains(id, "purpur") || contains(id, "magma") ||
        contains(id, "bone_block") || contains(id, "dripstone") || contains(id, "calcite") ||
        contains(id, "tuff") || contains(id, "mud_bricks"))
        return ToolClass::Pickaxe;
    if (id == "sand" || id == "red_sand" || id == "gravel" || contains(id, "dirt") ||
        id == "grass_block" || id == "podzol" || id == "mycelium" || contains(id, "clay") ||
        contains(id, "snow") || id == "soul_sand" || id == "soul_soil" || id == "mud" ||
        id == "farmland" || contains(id, "concrete_powder"))
        return ToolClass::Shovel;
    if (contains(id, "_log") || contains(id, "_wood") || contains(id, "planks") ||
        contains(id, "_stem") || contains(id, "_hyphae") || contains(id, "chest") ||
        id == "crafting_table" || id == "barrel" || id == "bookshelf" || id == "lectern" ||
        id == "note_block" || id == "jukebox" || id == "composter" || id == "loom" ||
        id == "fletching_table" || id == "cartography_table" || id == "smithing_table" ||
        contains(id, "pumpkin") || id == "melon" || contains(id, "bamboo") ||
        contains(id, "_sign") || contains(id, "_door") || contains(id, "_fence") ||
        contains(id, "ladder") || contains(id, "scaffolding"))
        return ToolClass::Axe;
    if (contains(id, "leaves") || id == "hay_block" || contains(id, "sculk") ||
        id == "target" || contains(id, "moss"))
        return ToolClass::Hoe;
    if (id == "wool" || contains(id, "_wool") || id == "cobweb" || id == "web")
        return ToolClass::Shears;
    return ToolClass::None;
}

// Minimum tool tier (material rank) needed for the block to drop anything.
int requiredTier(const std::string& rawId) {
    const auto id = localPart(rawId);
    if (id == "obsidian" || id == "ancient_debris" || id == "netherite_block" ||
        id == "crying_obsidian" || id == "respawn_anchor")
        return 5; // diamond+
    if (contains(id, "diamond_ore") || contains(id, "emerald_ore") || contains(id, "gold_ore") ||
        contains(id, "redstone_ore") || id == "gold_block" || id == "diamond_block" ||
        id == "emerald_block" || id == "redstone_block")
        return 4; // iron+
    if (contains(id, "iron_ore") || contains(id, "lapis_ore") || id == "iron_block" ||
        id == "lapis_block" || contains(id, "copper") || contains(id, "_rail") || id == "rail")
        return 2; // stone+
    if (classForBlock(id) == ToolClass::Pickaxe) return 1; // wooden pickaxe still required
    return 0;
}

ToolClass classForItem(const std::string& rawId, int& tier) {
    const auto id = localPart(rawId);
    tier = 0;
    if (id == "shears") return ToolClass::Shears;
    ToolClass kind = ToolClass::None;
    if (contains(id, "_pickaxe")) kind = ToolClass::Pickaxe;
    else if (contains(id, "_shovel")) kind = ToolClass::Shovel;
    else if (contains(id, "_hoe")) kind = ToolClass::Hoe;
    else if (contains(id, "_axe")) kind = ToolClass::Axe;
    if (kind == ToolClass::None) return ToolClass::None;
    if (contains(id, "netherite_")) tier = 6;
    else if (contains(id, "diamond_")) tier = 5;
    else if (contains(id, "iron_")) tier = 4;
    else if (contains(id, "golden_")) tier = 3;
    else if (contains(id, "stone_")) tier = 2;
    else if (contains(id, "wooden_")) tier = 1;
    return kind;
}
bool handMineable(const std::string& blockId) {
    // Blocks that drop (or are acceptably broken) with a bare hand / any item.
    return classForBlock(blockId) != ToolClass::Pickaxe && requiredTier(blockId) == 0;
}

// ---------------------------------------------------------------------------
// AutoCraft planner: given the visible inventory, describe the shortest
// chain that produces a usable pickaxe. Pure planning; execution of real
// ItemStackRequest crafting is the follow-up step.
// ---------------------------------------------------------------------------
struct CraftPlan {
    bool possible{};
    std::string summary;
};
CraftPlan planPickaxe(const std::unordered_map<std::string, int>& items) {
    auto have = [&](std::string_view key) {
        int total = 0;
        for (const auto& [id, count] : items) if (contains(id, key)) total += count;
        return total;
    };
    const int logs = have("_log") + have("_wood");
    const int planks = have("planks");
    const int sticks = have("stick");
    const int tables = have("crafting_table");
    const int cobble = have("cobblestone");
    // Wooden pickaxe: 3 planks + 2 sticks on a table. Sticks: 2 planks.
    // Table: 4 planks. Total planks for the full chain without a table: 9.
    const int plankTotal = planks + logs * 4;
    CraftPlan plan;
    if (cobble >= 3 && (sticks >= 2 || plankTotal >= 2) && (tables >= 1 || plankTotal >= 4)) {
        plan.possible = true;
        plan.summary = "丸石3＋棒2＋作業台で「石のツルハシ」が作れるよ";
        return plan;
    }
    if (plankTotal >= (tables >= 1 ? 5 : 9)) {
        plan.possible = true;
        plan.summary = "原木" + std::to_string(logs) + "・板材" + std::to_string(planks) +
            "から 板材→（作業台）→棒→「木のツルハシ」が作れるよ";
        return plan;
    }
    plan.summary = "ツルハシの材料が足りない（原木あと" +
        std::to_string(std::max(0, 3 - logs - (planks >= 9 ? 3 : 0))) +
        "個くらい必要）。原木を集めてホットバーに置いてね";
    return plan;
}

// ---------------------------------------------------------------------------
// /mine command capture on the packet-send route (the SDK command builder
// supports integer arguments only, so the block name is parsed here from the
// raw command line, exactly like NativeCommandService's fallback path).
// ---------------------------------------------------------------------------
class MineCommandHook {
public:
    bool load(const bedrock::RuntimeProfile& profile, std::string& error) {
        if (!profile.supports({bedrock::Feature::CommandPackets, bedrock::Feature::PacketSender}, error))
            return false;
        try {
            const auto root = bedrock::parseNativeProfile(profile.json());
            base_ = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
            textBegin_ = base_ + number(root.at("target").at("text_rva"));
            textEnd_ = textBegin_ + number(root.at("target").at("text_size"));
            senderResolver_.load(root);
            const auto& command = root.at("native_packets").at("command_request");
            commandVtable_ = base_ + number(command.at("vtable_rva"));
            commandOffset_ = static_cast<std::uint32_t>(number(command.at("command_offset")));
            if (commandOffset_ > 4096) throw std::runtime_error("invalid command transport layout");
            loaded_ = true;
            return true;
        } catch (const std::exception& e) { error = e.what(); return false; }
    }
    void stop() {
        stopped_.store(true);
        if (packetToken_) { BDS_HookUnsubscribe(packetToken_); packetToken_ = nullptr; }
    }
    void update(void* client) {
        if (stopped_.load() || !loaded_) return;
        void* sender{}; void* target{};
        if (!senderResolver_.resolve(client, sender, target) || !sender || !target) return;
        const auto address = reinterpret_cast<std::uintptr_t>(target);
        if (address < textBegin_ || address >= textEnd_) return;
        if (!packetToken_) {
            if (!BDS_HookOwnsTarget(BDS_HOOK_PACKET_SEND_TO_SERVER, target)) {
                DWORD64 image{};
                const auto* function = RtlLookupFunctionEntry(address, &image, nullptr);
                if (!function || image + function->BeginAddress != address) return;
            }
            if (!BDS_HookSubscribe(BDS_HOOK_PACKET_SEND_TO_SERVER, target, &onPacket, this, &packetToken_))
                return;
            installedTarget_ = target;
        }
        sender_.store(sender);
    }
    bool poll(std::string& line) {
        std::lock_guard lock(mutex_);
        if (lines_.empty()) return false;
        line = std::move(lines_.front()); lines_.pop_front();
        return true;
    }
private:
    static std::uint64_t number(const nlohmann::json& value) {
        return value.is_string() ? std::stoull(value.get<std::string>(), nullptr, 0)
                                 : value.get<std::uint64_t>();
    }
    struct Line { char bytes[1024]; std::size_t size{}; };
    static bool copyLine(const std::byte* value, std::uint32_t offset, Line& out) {
        if (!value) return false;
        __try {
            const auto* p = value + offset;
            std::size_t size{}, capacity{};
            std::memcpy(&size, p + 16, 8); std::memcpy(&capacity, p + 24, 8);
            if (size > sizeof(out.bytes) - 1 || capacity < size || capacity > 0x1000000) return false;
            const char* data = reinterpret_cast<const char*>(p);
            if (capacity > 15) std::memcpy(&data, p, 8);
            if (!data) return false;
            std::memcpy(out.bytes, data, size); out.size = size; return true;
        } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
    static bool readPtr(const void* object, std::size_t offset, std::uintptr_t& out) noexcept {
        __try { std::memcpy(&out, static_cast<const std::byte*>(object) + offset, sizeof(out)); return true; }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
    static void BDS_HOOK_CALL onPacket(void* context, void* raw, BDS_HookNext next, void* frame) noexcept {
        auto& self = *static_cast<MineCommandHook*>(context);
        const auto& args = *static_cast<BDS_PacketSendArgs*>(raw);
        if (self.stopped_.load() || !args.sender || args.sender != self.sender_.load()) { next(frame, raw); return; }
        std::uintptr_t vtable{}; Line line;
        if (!readPtr(args.packet, 0, vtable) || vtable != self.commandVtable_ ||
            !copyLine(static_cast<const std::byte*>(args.packet), self.commandOffset_, line)) {
            next(frame, raw); return;
        }
        std::string text(line.bytes, line.size);
        while (!text.empty() && (text.front() == '/' || text.front() == ' ')) text.erase(text.begin());
        const auto word = text.substr(0, text.find(' '));
        if (lower(word) != "mine") { next(frame, raw); return; }
        // Ours: swallow the packet so the server never sees it, queue the line.
        try {
            std::lock_guard lock(self.mutex_);
            if (self.lines_.size() < 16) self.lines_.push_back(std::move(text));
        } catch (...) {}
    }
    bedrock::NativePacketSender senderResolver_;
    std::atomic<void*> sender_{};
    std::atomic<bool> stopped_{};
    std::uintptr_t base_{}, textBegin_{}, textEnd_{}, commandVtable_{};
    std::uint32_t commandOffset_{};
    BDS_HookToken packetToken_{};
    void* installedTarget_{};
    bool loaded_{};
    std::mutex mutex_;
    std::deque<std::string> lines_;
};

// ---------------------------------------------------------------------------
// The mod itself
// ---------------------------------------------------------------------------
struct BlockPos3 {
    int x{}, y{}, z{};
    bool operator<(const BlockPos3& o) const noexcept {
        if (x != o.x) return x < o.x; if (y != o.y) return y < o.y; return z < o.z;
    }
    bool operator==(const BlockPos3&) const noexcept = default;
};

class AutoMineMod {
public:
    std::atomic<unsigned> status{};
    void signalStop() { stopped_ = true; }
    void stop() {
        signalStop();
        hook_.stop();
        if (mouseToken_) { BDS_HookUnsubscribe(mouseToken_); mouseToken_ = nullptr; }
        gui_.stop();
        releaseInputs();
    }
    void run() {
        status = 1;
        try {
            const auto dir = directory();
            std::filesystem::create_directories(dir);
            loadConfig(dir);
            initialize();
            status = 2;
            std::ofstream out(dir / L"AutoMine.log", std::ios::trunc);
            while (!stopped_.load()) { flush(out); std::this_thread::sleep_for(100ms); }
            flush(out);
            status = 3;
        } catch (const std::exception& error) {
            status = 4;
            try {
                const auto dir = directory();
                std::filesystem::create_directories(dir);
                std::ofstream out(dir / L"AutoMine.log", std::ios::app);
                flush(out);
                out << "startup failed: " << error.what() << '\n';
            } catch (...) {}
        }
        stop();
    }
private:
    // -- logging / chat ------------------------------------------------------
    void note(std::string text) {
        std::lock_guard lock(logMutex_);
        if (logs_.size() < 256) logs_.push_back(std::move(text));
    }
    void flush(std::ostream& out) {
        std::deque<std::string> batch;
        { std::lock_guard lock(logMutex_); batch.swap(logs_); }
        for (const auto& line : batch) out << line << '\n';
        if (!batch.empty()) out.flush();
    }
    void reply(const std::string& text) {
        note(text);
        if (!gui_.message("[AutoMine] " + text)) note("game message unavailable; see AutoMine.log");
    }
    static std::filesystem::path directory() {
        PWSTR path{};
        if (FAILED(SHGetKnownFolderPath(FOLDERID_Documents, KF_FLAG_CREATE, nullptr, &path)))
            throw std::runtime_error("Documents unavailable");
        const auto documents = std::filesystem::path(path);
        CoTaskMemFree(path);
        return documents / L"PEXData" / L"AutoMine";
    }
    void loadConfig(const std::filesystem::path& dir) {
        const auto path = dir / L"config.json";
        nlohmann::json config;
        if (std::filesystem::exists(path)) { std::ifstream file(path); file >> config; }
        else {
            config = {{"enabled", true}, {"radius", 24}};
            std::ofstream file(path); file << config.dump(2) << '\n';
        }
        enabled_ = config.value("enabled", true);
        radius_ = std::clamp(config.value("radius", 24), 4, 48);
    }

    // -- setup ---------------------------------------------------------------
    void initialize() {
        bedrock::ProfileManager profiles;
        const bedrock::ProfileRequirements required{
            bedrock::Feature::Gameplay, bedrock::Feature::LevelTick, bedrock::Feature::World,
            bedrock::Feature::Motion, bedrock::Feature::Gui, bedrock::Feature::WorldHeight,
            bedrock::Feature::PacketSender, bedrock::Feature::CommandPackets,
            bedrock::Feature::ClientCommands, bedrock::Feature::WorldEvents,
            bedrock::Feature::Camera};
        const bedrock::ProfileOptions options{bedrock::UpdatePolicy::RefreshIfNeeded,
                                              bedrock::CandidatePolicy::AllowCandidates};
        std::string error;
        if (!profiles.prepare(required, options,
                [this](const bedrock::RuntimeProfile& profile, std::string& failure) {
                    return gui_.load(profile, failure) && hook_.load(profile, failure) &&
                           camera_.load(profile, failure) && loadWorld(profile, failure);
                }, error))
            throw std::runtime_error(error);

        game_.Event.Enum.Gameplay.then([this](const bedrock::GameplayEvent& event) {
            if (event.clientInstance) client_.store(event.clientInstance);
        });
        game_.Event.Enum.WorldChange.then([this](const bedrock::WorldChangeEvent& event) {
            if (event.clientInstance) client_.store(event.clientInstance);
            ++epoch_;
            pendingReset_ = true; // applied on the tick thread
        });
        game_.Event.Enum.LevelTick.then([this](const bedrock::LevelTickEvent& event) {
            if (stopped_.load()) return;
            try {
                auto scope = gui_.tick(client_.load(), event.level, epoch_.load());
                if (!scope) return;
                auto camScope = camera_.tick(client_.load(), bedrock::WorldEpoch{epoch_.load()});
                snap_ = camera_.readPlayerProxy();
                hook_.update(client_.load());
                tick();
            } catch (const std::exception& e) {
                releaseInputs();
                note(std::string("tick failed: ") + e.what());
            }
        });
        const auto selected = profiles.snapshot();
        hooks_ = std::make_unique<bedrock::GameHooks>(game_);
        if (!hooks_->installGameplay(selected->resolver(), error) ||
            !hooks_->installLevelTick(selected->resolver(), error) ||
            !hooks_->installWorldEvents(selected->resolver(), selected->json(), error) ||
            !hooks_->installNativeKeyboard(selected->resolver(), selected->json(), error) ||
            !hooks_->installInput(selected->resolver(), error))
            throw std::runtime_error(error);
        installMouseCapture(selected->resolver());
        note("AutoMine v1: /mine <block-id> | /mine stop | /mine status");
        note(BDS_HookRuntimeDiagnostic());
    }
    bool loadWorld(const bedrock::RuntimeProfile& profile, std::string& failure) {
        try { world_.load(profile.json()); }
        catch (const std::exception& e) { failure = e.what(); return false; }
        loadItems(profile);
        return true;
    }
    // The SDK's hotbar reader only resolves block-backed stacks (it walks
    // the stack's Block*), so tools — pure item stacks — read as empty and
    // AutoTool could never see a pickaxe. Read the stacks directly: supplies
    // -> inventory vtable getItem -> ItemStack's Item* -> namespaced id.
    struct ItemLayout {
        std::size_t suppliesOffset{}, inventoryOffset{}, getItemSlot{};
        std::size_t itemRefOffset{}, countOffset{}, idOffset{}, strSizeOffset{}, strCapOffset{};
        bool loaded{};
    } items_;
    void loadItems(const bedrock::RuntimeProfile& profile) {
        try {
            const auto root = bedrock::parseNativeProfile(profile.json());
            const auto& ni = root.at("native_items");
            const auto& lp = ni.at("local_player");
            const auto& inv = ni.at("inventory");
            const auto& st = ni.at("item_stack");
            auto num = [](const nlohmann::json& j) -> std::size_t {
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
            items_.loaded = true;
            note("items layout loaded");
        } catch (const std::exception& e) {
            note(std::string("items layout unavailable: ") + e.what());
        }
    }
    void* itemStack(void* p, int slot) {
        void* supplies{}; void* inventory{}; void* vt{}; void* fn{};
        if (!world_.readField(p, items_.suppliesOffset, supplies) || !supplies) return nullptr;
        if (!world_.readField(supplies, items_.inventoryOffset, inventory) || !inventory) return nullptr;
        if (!world_.readField(inventory, 0, vt) || !vt) return nullptr;
        if (!world_.readField(static_cast<const void*>(vt), items_.getItemSlot * 8, fn) || !fn)
            return nullptr;
        void* stack{};
        __try { stack = reinterpret_cast<void*(__fastcall*)(void*, int)>(fn)(inventory, slot); }
        __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
        return stack;
    }
    std::string stackItemId(void* stack) {
        // itemReferenceOffset holds an ItemRef: a pointer to the Item*
        // (double indirection, as in the SDK's own equipment reader).
        void* itemRef{};
        if (!world_.readField(stack, items_.itemRefOffset, itemRef) || !itemRef) return {};
        void* item{};
        if (!world_.readField(itemRef, 0, item) || !item) return {};
        const auto base = items_.idOffset;
        std::size_t size{}, cap{};
        if (!world_.readField(item, base + items_.strSizeOffset, size) ||
            !world_.readField(item, base + items_.strCapOffset, cap) ||
            size == 0 || size > 256 || cap < size || cap > 0x100000)
            return {};
        const void* data = static_cast<const std::byte*>(item) + base;
        if (cap > 15) {
            const void* ptr{};
            if (!world_.readField(item, base, ptr) || !ptr) return {};
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
    struct InvSlot { std::string id; int count{}; };
    std::array<InvSlot, 9> hotbarItems() {
        std::array<InvSlot, 9> out;
        void* p = player();
        if (!p) return out;
        if (!items_.loaded) { // fallback: SDK reader (block-backed stacks only)
            bedrock::NativeHotbar bar;
            if (world_.hotbar(p, bar))
                for (int i = 0; i < 9; ++i)
                    out[static_cast<std::size_t>(i)] = {bar.copied[static_cast<std::size_t>(i)].identifier,
                                                        static_cast<int>(bar.copied[static_cast<std::size_t>(i)].count)};
            return out;
        }
        for (int i = 0; i < 9; ++i) {
            void* stack = itemStack(p, i);
            if (!stack) continue;
            std::uint8_t count{};
            if (!world_.readField(stack, items_.countOffset, count) || !count) continue;
            out[static_cast<std::size_t>(i)] = {stackItemId(stack), static_cast<int>(count)};
        }
        return out;
    }
    // -- input helpers (owner thread only) ------------------------------------
    bedrock::Keyboard keyboard() { return game_.localPlayer().getKeyboard(); }
    bedrock::Mouse mouse() { return game_.localPlayer().getMouse(); }
    // Mouse control through the game's own MouseDevice::feed, replaying the
    // shape of a real captured event (device pointer, pointer position) with
    // our deltas/button state and WITHOUT the synthetic flag the GameApi
    // replay adds. Every other route failed live: SDK moveMouse/turn are
    // unimplemented, rotation component writes are overwritten each tick
    // from the input-side state, and OS-level SendInput never reaches this
    // client's view or destroy logic.
    void installMouseCapture(const bedrock::Resolver& resolver) {
        auto res = resolver.resolveRvaForSharedHook("MouseDevice::feed");
        if (!res) { note("mouse capture: MouseDevice::feed unresolved"); return; }
        auto* target = reinterpret_cast<void*>(res->address);
        if (!BDS_HookSubscribe(BDS_HOOK_MOUSE_FEED, target, &onMouseCapture, this, &mouseToken_)) {
            note("mouse capture: subscribe failed");
            return;
        }
        note("mouse capture: subscribed");
    }
    static void BDS_HOOK_CALL onMouseCapture(void* user, void* raw, BDS_HookNext next, void* frame) noexcept {
        auto& self = *static_cast<AutoMineMod*>(user);
        try {
            std::lock_guard lock(self.mouseMutex_);
            self.lastMouse_ = *static_cast<BDS_MouseArgs*>(raw);
            self.haveMouse_ = true;
        } catch (...) {}
        next(frame, raw);
    }
    bool feedMouse(int action, int state, int dx, int dy) {
        BDS_MouseArgs args;
        {
            std::lock_guard lock(mouseMutex_);
            if (!haveMouse_) return false;
            args = lastMouse_;
        }
        args.action = static_cast<std::int8_t>(action);
        args.state = static_cast<std::int8_t>(state);
        args.dx = static_cast<std::int16_t>(std::clamp(dx, -32768, 32767));
        args.dy = static_cast<std::int16_t>(std::clamp(dy, -32768, 32767));
        args.x = static_cast<std::int16_t>(std::clamp(static_cast<int>(args.x) + dx, -32768, 32767));
        args.y = static_cast<std::int16_t>(std::clamp(static_cast<int>(args.y) + dy, -32768, 32767));
        args.force_motionless_pointer &= BDS_MOUSE_FLAG_FORCE_MOTIONLESS;
        BDS_HookDispatch(BDS_HOOK_MOUSE_FEED, &args);
        {
            std::lock_guard lock(mouseMutex_);
            lastMouse_.x = args.x;
            lastMouse_.y = args.y;
        }
        return true;
    }
    bool feedMove(int dx, int dy) { return feedMouse(0, 0, dx, dy); }
    bool feedButton(bool down) { return feedMouse(1, down ? 1 : 0, 0, 0); }
    void pressKey(unsigned key, bool down) {
        auto kb = keyboard();
        if (down) { if (!heldKeys_.count(key) && kb.press(key)) heldKeys_.insert(key); }
        else if (heldKeys_.erase(key)) kb.release(key);
    }
    void setAttack(bool down) {
        if (attackHeld_ == down) return;
        if (feedButton(down) || mouse().setDown(bedrock::MouseButton::Left, down))
            attackHeld_ = down;
    }
    void releaseInputs() {
        for (auto key : {kKeyW, kKeyA, kKeyS, kKeyD, kKeySpace}) {
            if (heldKeys_.erase(key)) { try { keyboard().release(key); } catch (...) {} }
        }
        if (attackHeld_) { try { feedButton(false); mouse().setDown(bedrock::MouseButton::Left, false); } catch (...) {} attackHeld_ = false; }
    }

    // -- world helpers ---------------------------------------------------------
    void* player() { return world_.localPlayer(client_.load()); }
    void* region() { return world_.region(client_.load()); }
    // True feet position from the player's AABB (minimum corner). The pose /
    // snapshot position reads proved anchor-unstable for the local player
    // (sometimes feet, sometimes eye height), which poisoned every Y-based
    // decision; the bounding box minimum is unambiguous.
    bool feetPos(bedrock::Vec3& out) {
        void* p = player();
        if (!p) return false;
        const auto& actors = world_.actors();
        void* shape = world_.component(p, actors.aabbShapeComponentHash);
        if (!shape && actors.aabbShapeComponentHashAlternate)
            shape = world_.component(p, actors.aabbShapeComponentHashAlternate);
        if (shape) {
            bedrock::Vec3 mn{}, mx{};
            if (world_.readField(shape, actors.aabbMinimumOffset, mn) &&
                world_.readField(shape, actors.aabbMaximumOffset, mx) &&
                mx.y > mn.y && mx.y - mn.y < 4.f) {
                out = {(mn.x + mx.x) * .5f, mn.y, (mn.z + mx.z) * .5f};
                return true;
            }
        }
        std::uint64_t rt{};
        if (!world_.pose(p, out, rt)) return false;
        out.y -= 1.62f; // fallback: pose read is eye-anchored in most states
        return true;
    }
    bool eyePos(bedrock::Vec3& out) {
        if (!feetPos(out)) return false;
        out.y += 1.62f;
        return true;
    }
    std::string blockAt(BlockPos3 pos) {
        void* src = region();
        if (!src) return {};
        bedrock::BlockPos bp; bp.x = pos.x; bp.y = pos.y; bp.z = pos.z;
        auto cell = world_.getBlock(src, bp);
        return cell.identifier;
    }
    // Voxel raycast (Amanatides & Woo) from the eye toward a target cell:
    // returns true when the target cell is the first solid cell on the ray
    // (line of sight is clear); otherwise fills `hit` with the occluding cell.
    bool lineOfSight(bedrock::Vec3 eye, BlockPos3 targetCell, BlockPos3& hit) {
        const bedrock::Vec3 goal{targetCell.x + .5f, targetCell.y + .5f, targetCell.z + .5f};
        double dx = goal.x - eye.x, dy = goal.y - eye.y, dz = goal.z - eye.z;
        const double len = std::sqrt(dx * dx + dy * dy + dz * dz);
        if (len < 1e-6) return true;
        dx /= len; dy /= len; dz /= len;
        int cx = static_cast<int>(std::floor(eye.x));
        int cy = static_cast<int>(std::floor(eye.y));
        int cz = static_cast<int>(std::floor(eye.z));
        const int stepX = dx > 0 ? 1 : -1, stepY = dy > 0 ? 1 : -1, stepZ = dz > 0 ? 1 : -1;
        auto boundary = [](double pos, double dir) {
            if (dir == 0) return 1e30;
            const double cell = dir > 0 ? std::floor(pos) + 1.0 : std::floor(pos);
            return std::abs((cell - pos) / dir);
        };
        double tX = boundary(eye.x, dx), tY = boundary(eye.y, dy), tZ = boundary(eye.z, dz);
        const double dtX = dx == 0 ? 1e30 : std::abs(1.0 / dx);
        const double dtY = dy == 0 ? 1e30 : std::abs(1.0 / dy);
        const double dtZ = dz == 0 ? 1e30 : std::abs(1.0 / dz);
        double t = 0;
        for (int i = 0; i < 64 && t <= len; ++i) {
            if (tX < tY && tX < tZ) { cx += stepX; t = tX; tX += dtX; }
            else if (tY < tZ) { cy += stepY; t = tY; tY += dtY; }
            else { cz += stepZ; t = tZ; tZ += dtZ; }
            if (cx == targetCell.x && cy == targetCell.y && cz == targetCell.z) return true;
            const auto id = blockAt({cx, cy, cz});
            // Fluids do not block the game's targeting ray.
            if (!id.empty() && !isAir(id) && !contains(localPart(id), "water") &&
                !contains(localPart(id), "lava")) { hit = {cx, cy, cz}; return false; }
        }
        return true; // reached the goal distance without entering the cell
    }
    bool rotation(float& yaw, float& pitch) {
        // The world-access component route reads back frozen spawn values for
        // the local player on this build; the camera context (Freecam's route)
        // reads the live view rotation.
        if (!snap_) return false;
        yaw = snap_->rotation.yaw;
        pitch = snap_->rotation.pitch;
        return true;
    }
    static float wrapDeg(float deg) {
        while (deg > 180.f) deg -= 360.f;
        while (deg < -180.f) deg += 360.f;
        return deg;
    }
    // Aim the view at a world point: closed loop on the live rotation read,
    // feeding mouse motion through the game's own MouseDevice::feed (see
    // feedMouse). The degrees-per-pixel gain is adapted online because it
    // depends on the client's mouse sensitivity.
    bool faceToward(bedrock::Vec3 eye, bedrock::Vec3 target, float tolerance = 3.f) {
        const double dx = target.x - eye.x, dy = target.y - eye.y, dz = target.z - eye.z;
        const double horiz = std::hypot(dx, dz);
        const float wantYaw = static_cast<float>(std::atan2(-dx, dz) * 180.0 / 3.141592653589793);
        const float wantPitch = static_cast<float>(std::atan2(-dy, horiz) * 180.0 / 3.141592653589793);
        float yaw{}, pitch{};
        if (!rotation(yaw, pitch)) return false;
        if (lastPx_ != 0) {
            const float moved = wrapDeg(yaw - lastYaw_);
            if (moved * static_cast<float>(lastPx_) > 0.f) {
                const float g = moved / static_cast<float>(lastPx_);
                if (g > 0.02f && g < 3.f) aimGain_ = aimGain_ * 0.6f + g * 0.4f;
            }
            lastPx_ = 0;
        }
        if (lastPy_ != 0) {
            const float moved = pitch - lastPitch_;
            if (moved * static_cast<float>(lastPy_) > 0.f) {
                const float g = moved / static_cast<float>(lastPy_);
                if (g > 0.02f && g < 3.f) aimGain_ = aimGain_ * 0.6f + g * 0.4f;
            }
            lastPy_ = 0;
        }
        const float dyaw = wrapDeg(wantYaw - yaw);
        const float dpitch = std::clamp(wantPitch - pitch, -89.f, 89.f);
        if (std::abs(dyaw) <= tolerance && std::abs(dpitch) <= tolerance) return true;
        const int maxPx = std::max(4, static_cast<int>(std::lround(22.f / aimGain_)));
        const int px = std::clamp(static_cast<int>(std::lround(dyaw / aimGain_)), -maxPx, maxPx);
        const int py = std::clamp(static_cast<int>(std::lround(dpitch / aimGain_)), -maxPx, maxPx);
        if ((px || py) && feedMove(px, py)) {
            lastPx_ = px; lastPy_ = py; lastYaw_ = yaw; lastPitch_ = pitch;
        }
        return false;
    }

    // -- tools -----------------------------------------------------------------
    struct ToolChoice { int slot{-1}; int tier{}; ToolClass kind{ToolClass::None}; bool byHand{}; };
    ToolChoice chooseTool(const std::string& blockId) {
        ToolChoice best;
        const auto need = classForBlock(blockId);
        const int minTier = requiredTier(blockId);
        void* p = player();
        if (!p) return best;
        if (need == ToolClass::None || minTier == 0) { best.byHand = true; return best; }
        const auto slots = hotbarItems();
        for (int slot = 0; slot < 9; ++slot) {
            const auto& item = slots[static_cast<std::size_t>(slot)];
            if (item.count == 0 || item.id.empty()) continue;
            int tier{};
            const auto kind = classForItem(item.id, tier);
            if (kind != need || tier < minTier) continue;
            if (slot == best.slot || tier > best.tier) { best.slot = slot; best.tier = tier; best.kind = kind; }
        }
        return best;
    }
    bool ensureTool(const std::string& blockId) {
        const auto choice = chooseTool(blockId);
        if (choice.byHand) return true;
        if (choice.slot < 0) return false;
        void* p = player();
        if (p && world_.selected(p) != choice.slot) world_.select(p, static_cast<unsigned>(choice.slot));
        return true;
    }
    std::unordered_map<std::string, int> visibleItems() {
        std::unordered_map<std::string, int> items;
        for (const auto& slot : hotbarItems())
            if (slot.count && !slot.id.empty()) items[localPart(slot.id)] += slot.count;
        return items;
    }

    // -- state machine -----------------------------------------------------------
    enum class Phase { Idle, Scan, Navigate, Mine, Collect, NeedTool };
    void resetRun() {
        releaseInputs();
        phase_ = Phase::Idle;
        targetId_.clear();
        blacklist_.clear();
        hasTarget_ = false;
        mined_ = 0;
    }
    void startMining(std::string arg) {
        resetRun();
        targetId_ = localPart(arg);
        if (targetId_.empty() || isAir(targetId_)) { reply("ブロック名を指定してね（例: /mine iron_ore）"); return; }
        phase_ = Phase::Scan;
        scanCursor_ = 0;
        bestDist2_ = 1e18;
        reply(targetId_ + " を探して掘るよ（半径" + std::to_string(radius_) + "）");
        note("mine start: " + targetId_);
    }
    void stopMining(const std::string& why) {
        releaseInputs();
        if (phase_ != Phase::Idle)
            reply("止めたよ（掘った数: " + std::to_string(mined_) + "）" + (why.empty() ? "" : " " + why));
        phase_ = Phase::Idle;
        hasTarget_ = false;
    }
    void handleLine(const std::string& line) {
        // line starts with "mine"; argument is the remainder.
        std::string rest = line.size() > 4 ? line.substr(4) : "";
        while (!rest.empty() && rest.front() == ' ') rest.erase(rest.begin());
        const auto arg = lower(rest.substr(0, rest.find(' ')));
        if (arg.empty() || arg == "status") {
            reply(statusText());
        } else if (arg == "stop") {
            stopMining("");
        } else if (enabled_) {
            startMining(arg);
        } else {
            reply("設定で無効になっているよ");
        }
    }
    std::string statusText() const {
        switch (phase_) {
        case Phase::Idle: return "待機中。/mine <ブロック名> で開始";
        case Phase::Scan: return targetId_ + " を探索中…（掘った数: " + std::to_string(mined_) + "）";
        case Phase::Navigate: return targetId_ + " へ移動中（掘った数: " + std::to_string(mined_) + "）";
        case Phase::Mine: return targetId_ + " を採掘中（掘った数: " + std::to_string(mined_) + "）";
        case Phase::Collect: return "ドロップ回収中（掘った数: " + std::to_string(mined_) + "）";
        case Phase::NeedTool: return "ツール待ち（掘った数: " + std::to_string(mined_) + "）";
        }
        return "";
    }

    void tick() {
        if (pendingReset_) { pendingReset_ = false; gui_.reset(epoch_.load()); camera_.reset(); resetRun(); }
        std::string line;
        while (hook_.poll(line)) handleLine(line);
        if (phase_ == Phase::Idle) return;
        if (!game_.gameplayActive()) { releaseInputs(); return; }
        bedrock::Vec3 feet;
        if (!feetPos(feet)) { releaseInputs(); return; }
        switch (phase_) {
        case Phase::Scan: tickScan(feet); break;
        case Phase::Navigate: tickNavigate(feet); break;
        case Phase::Mine: tickMine(feet); break;
        case Phase::Collect: tickCollect(feet); break;
        case Phase::NeedTool: tickNeedTool(); break;
        case Phase::Idle: break;
        }
    }

    void tickScan(bedrock::Vec3 feet) {
        releaseInputs();
        if (scanWait_ > 0) { --scanWait_; return; }
        const int cx = static_cast<int>(std::floor(feet.x));
        const int cy = static_cast<int>(std::floor(feet.y));
        const int cz = static_cast<int>(std::floor(feet.z));
        const int ry = 10;
        const int spanX = radius_ * 2 + 1, spanY = ry * 2 + 1, spanZ = radius_ * 2 + 1;
        const int total = spanX * spanY * spanZ;
        void* src = region();
        if (!src) return;
        int budget = 9000;
        while (scanCursor_ < total && budget-- > 0) {
            const int i = scanCursor_++;
            const int dx = (i % spanX) - radius_;
            const int dy = ((i / spanX) % spanY) - ry;
            const int dz = (i / (spanX * spanY)) - radius_;
            BlockPos3 pos{cx + dx, cy + dy, cz + dz};
            if (blacklist_.count(pos)) continue;
            bedrock::BlockPos bp; bp.x = pos.x; bp.y = pos.y; bp.z = pos.z;
            auto cell = world_.getBlock(src, bp);
            if (cell.identifier.empty() || isAir(cell.identifier)) continue;
            if (localPart(cell.identifier) != targetId_) continue;
            const double ddx = pos.x + .5 - feet.x, ddy = pos.y + .5 - feet.y, ddz = pos.z + .5 - feet.z;
            const double d2 = ddx * ddx + ddy * ddy + ddz * ddz;
            if (d2 < bestDist2_) {
                bestDist2_ = d2; target_ = pos; hasTarget_ = true;
                if (d2 < 36.0) { scanCursor_ = total; break; } // close enough: go now
            }
        }
        if (scanCursor_ < total) return;
        if (hasTarget_) {
            emptyScans_ = 0;
            note("target: " + targetId_ + " at " + std::to_string(target_.x) + "," +
                 std::to_string(target_.y) + "," + std::to_string(target_.z));
            phase_ = Phase::Navigate;
            stuckTicks_ = 0; lastX_ = feet.x; lastZ_ = feet.z; navTicks_ = 0;
        } else {
            // Nothing in range: wait ~2s, then rescan (the player may move,
            // or a fresh area may load).
            note("scan: no " + targetId_ + " in range");
            scanCursor_ = 0; bestDist2_ = 1e18;
            if (++emptyScans_ == 3)
                reply("近くに " + targetId_ + " が見つからないよ。場所を変えてみて");
            scanWait_ = 40;
        }
    }

    void tickNavigate(bedrock::Vec3 feet) {
        if (!hasTarget_) { phase_ = Phase::Scan; scanCursor_ = 0; bestDist2_ = 1e18; return; }
        // Target may have changed (another player mined it, physics, ...).
        const auto targetNow = blockAt(target_);
        if (targetNow.empty() || localPart(targetNow) != targetId_) {
            note("target vanished; rescan");
            hasTarget_ = false; phase_ = Phase::Scan; scanCursor_ = 0; bestDist2_ = 1e18;
            releaseInputs(); return;
        }
        const bedrock::Vec3 eye{feet.x, feet.y + 1.62f, feet.z};
        const bedrock::Vec3 center{target_.x + .5f, target_.y + .5f, target_.z + .5f};
        const double dx = center.x - eye.x, dy = center.y - eye.y, dz = center.z - eye.z;
        const double dist = std::sqrt(dx * dx + dy * dy + dz * dz);
        // Tool first: if we will need a tool we cannot provide, stop early.
        if (!ensureTool(targetId_)) { enterNeedTool(); return; }
        if (dist <= 4.0) {
            pressKey(kKeyW, false); pressKey(kKeySpace, false);
            phase_ = Phase::Mine; mineTicks_ = 0;
            return;
        }
        faceToward(eye, center, 10.f);
        // Cliff guard: do not step into a >2 block drop straight ahead.
        const double hx = center.x - feet.x, hz = center.z - feet.z;
        const double hl = std::hypot(hx, hz);
        if (hl > 0.001) {
            const int fx = static_cast<int>(std::floor(feet.x + hx / hl * 0.8));
            const int fz = static_cast<int>(std::floor(feet.z + hz / hl * 0.8));
            const int fy = static_cast<int>(std::floor(feet.y));
            const auto below1 = blockAt({fx, fy - 1, fz});
            const auto below2 = blockAt({fx, fy - 2, fz});
            if (!below1.empty() && isAir(below1) && !below2.empty() && isAir(below2)) {
                pressKey(kKeyW, false);
                note("cliff guard: target unreachable on foot");
                blacklist_.insert(target_); hasTarget_ = false;
                phase_ = Phase::Scan; scanCursor_ = 0; bestDist2_ = 1e18;
                return;
            }
        }
        pressKey(kKeyW, true);
        // Stuck handling: jump, then mine through the blocker.
        ++navTicks_;
        if (navTicks_ % 10 == 0) {
            const double moved = std::hypot(feet.x - lastX_, feet.z - lastZ_);
            if (moved < 0.12) {
                if (++stuckTicks_ == 1) {
                    pressKey(kKeySpace, true);
                } else if (stuckTicks_ >= 3) {
                    pressKey(kKeySpace, false);
                    if (!tryMineThrough(feet)) {
                        blacklist_.insert(target_); hasTarget_ = false;
                        pressKey(kKeyW, false);
                        phase_ = Phase::Scan; scanCursor_ = 0; bestDist2_ = 1e18;
                        return;
                    }
                }
            } else {
                stuckTicks_ = 0;
                pressKey(kKeySpace, false);
            }
            lastX_ = feet.x; lastZ_ = feet.z;
        }
        if (navTicks_ > 20 * 45) { // ~45s per target is plenty
            note("navigate timeout; blacklist target");
            blacklist_.insert(target_); hasTarget_ = false;
            pressKey(kKeyW, false); pressKey(kKeySpace, false);
            phase_ = Phase::Scan; scanCursor_ = 0; bestDist2_ = 1e18;
        }
    }

    bool tryMineThrough(bedrock::Vec3 feet) {
        float yaw{}, pitch{};
        if (!rotation(yaw, pitch)) return false;
        const double rad = yaw * 3.141592653589793 / 180.0;
        const int fx = static_cast<int>(std::floor(feet.x - std::sin(rad) * 1.0));
        const int fz = static_cast<int>(std::floor(feet.z + std::cos(rad) * 1.0));
        const int fy = static_cast<int>(std::floor(feet.y));
        for (int y : {fy, fy + 1}) {
            const auto id = blockAt({fx, y, fz});
            if (!id.empty() && !isAir(id) && localPart(id) != "bedrock") {
                subTarget_ = {fx, y, fz}; subId_ = localPart(id);
                savedTarget_ = target_; savedId_ = targetId_;
                target_ = subTarget_; targetId_ = subId_;
                hasTarget_ = true; phase_ = Phase::Mine; mineTicks_ = 0;
                note("mine-through: " + subId_);
                return true;
            }
        }
        return false;
    }

    void tickMine(bedrock::Vec3 feet) {
        pressKey(kKeyW, false); pressKey(kKeySpace, false);
        const auto current = blockAt(target_);
        if (current.empty()) { setAttack(false); return; } // chunk hiccup: wait
        if (isAir(current) || localPart(current) != targetId_) {
            setAttack(false);
            ++mined_;
            note("mined " + targetId_ + " total=" + std::to_string(mined_));
            if (!savedId_.empty()) { // mine-through block cleared: restore goal
                targetId_ = savedId_; savedId_.clear(); hasTarget_ = false;
                phase_ = Phase::Scan; scanCursor_ = 0; bestDist2_ = 1e18;
                return;
            }
            phase_ = Phase::Collect; collectTicks_ = 0;
            return;
        }
        if (!ensureTool(current)) { setAttack(false); enterNeedTool(); return; }
        const bedrock::Vec3 eye{feet.x, feet.y + 1.62f, feet.z};
        const bedrock::Vec3 center{target_.x + .5f, target_.y + .5f, target_.z + .5f};
        const double dx = center.x - eye.x, dy = center.y - eye.y, dz = center.z - eye.z;
        if (std::sqrt(dx * dx + dy * dy + dz * dz) > 4.4) { setAttack(false); phase_ = Phase::Navigate; return; }
        // Occlusion: if another solid block blocks the ray to the target, mine
        // that block first (dig-through), then resume the original target.
        {
            BlockPos3 occluder;
            if (!lineOfSight(eye, target_, occluder)) {
                const auto occId = blockAt(occluder);
                if (!occId.empty() && localPart(occId) != "bedrock") {
                    setAttack(false);
                    if (savedId_.empty()) { savedTarget_ = target_; savedId_ = targetId_; }
                    target_ = occluder; targetId_ = localPart(occId);
                    mineTicks_ = 0;
                    note("occluded; mining through " + targetId_);
                    return;
                }
            }
        }
        // Aim first, then hold attack without touching the input routes again
        // until the block breaks. While the attack is held, re-aim only past
        // a loose 10-degree tolerance.
        const bool facing = faceToward(eye, center, attackHeld_ ? 10.f : 5.f);
        if (facing && !attackHeld_) {
            setAttack(true);
            if (attackHeld_) note("attack start");
            else if (mineTicks_ % 40 == 0) note("attack send failed");
        }
        ++mineTicks_;
        if (mineTicks_ > 20 * 12) {
            note("mine timeout; blacklist");
            setAttack(false);
            blacklist_.insert(target_); hasTarget_ = false;
            if (!savedId_.empty()) { targetId_ = savedId_; savedId_.clear(); }
            phase_ = Phase::Scan; scanCursor_ = 0; bestDist2_ = 1e18;
        }
    }

    void tickCollect(bedrock::Vec3 feet) {
        // Step onto the mined cell briefly to pick the drop up, when safe.
        const auto below = blockAt({target_.x, target_.y - 1, target_.z});
        const auto cell = blockAt(target_);
        if (collectTicks_ == 0 && (below.empty() || isAir(below) || !isAir(cell))) {
            phase_ = Phase::Scan; scanCursor_ = 0; bestDist2_ = 1e18; hasTarget_ = false; return;
        }
        const bedrock::Vec3 eye{feet.x, feet.y + 1.62f, feet.z};
        const bedrock::Vec3 center{target_.x + .5f, feet.y + .5f, target_.z + .5f};
        faceToward(eye, center, 12.f);
        pressKey(kKeyW, true);
        if (++collectTicks_ >= 7) {
            pressKey(kKeyW, false);
            hasTarget_ = false;
            phase_ = Phase::Scan; scanCursor_ = 0; bestDist2_ = 1e18;
        }
    }

    void enterNeedTool() {
        releaseInputs();
        phase_ = Phase::NeedTool; needTicks_ = 0;
        if (classForBlock(targetId_) == ToolClass::Pickaxe) {
            const auto plan = planPickaxe(visibleItems());
            reply("ツルハシがホットバーに無いよ。" + plan.summary);
            note("needtool: " + plan.summary + " possible=" + std::to_string(plan.possible));
        } else {
            reply("このブロック用のツールがホットバーに無いよ");
        }
    }
    void tickNeedTool() {
        if (++needTicks_ % 20) return;
        if (ensureTool(targetId_)) {
            reply("ツールを確認。再開するね");
            phase_ = Phase::Scan; scanCursor_ = 0; bestDist2_ = 1e18; hasTarget_ = false;
        }
    }

    // -- members -----------------------------------------------------------------
    bedrock::GameApi game_;
    bedrock::NativeWorldAccess world_;
    bedrock::NativeCamera camera_;
    std::optional<bedrock::CameraPlayerSnapshot> snap_;
    bedrock::NativeSafeTeleport gui_; // message route only; never requested to teleport
    MineCommandHook hook_;
    std::unique_ptr<bedrock::GameHooks> hooks_;
    std::atomic<void*> client_{};
    std::atomic<std::uint64_t> epoch_{1};
    std::atomic<bool> stopped_{};
    bool enabled_{true};
    int radius_{24};

    std::mutex logMutex_;
    std::deque<std::string> logs_;
    std::set<unsigned> heldKeys_;
    bool attackHeld_{};
    bool pendingReset_{};

    BDS_HookToken mouseToken_{};
    std::mutex mouseMutex_;
    BDS_MouseArgs lastMouse_{};
    bool haveMouse_{};
    float aimGain_{0.15f};
    int lastPx_{}, lastPy_{};
    float lastYaw_{}, lastPitch_{};
    Phase phase_{Phase::Idle};
    std::string targetId_;
    BlockPos3 target_{};
    bool hasTarget_{};
    std::set<BlockPos3> blacklist_;
    int mined_{};
    int scanCursor_{};
    double bestDist2_{1e18};
    int emptyScans_{};
    int scanWait_{};
    int navTicks_{}, stuckTicks_{};
    float lastX_{}, lastZ_{};
    int mineTicks_{}, collectTicks_{}, needTicks_{};
    BlockPos3 subTarget_{};
    std::string subId_;
    BlockPos3 savedTarget_{};
    std::string savedId_;
};
AutoMineMod mod;

DWORD WINAPI start(void*) {
    HMODULE pinned{};
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                       reinterpret_cast<LPCWSTR>(&start), &pinned);
    mod.run();
    return 0;
}
} // namespace

extern "C" __declspec(dllexport) unsigned AutoMine_GetStatus() { return mod.status.load(); }
extern "C" __declspec(dllexport) void AutoMine_Stop() { mod.stop(); }
BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(instance);
        if (const auto thread = CreateThread(nullptr, 0, start, nullptr, 0, nullptr)) CloseHandle(thread);
    }
    return TRUE;
}
#endif
