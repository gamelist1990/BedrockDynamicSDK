// AutoMine — Baritone-style automatic mining for the Bedrock client.
//
//   /mine <block-id>   find, walk to and mine nearby blocks, one after another
//   /mine stop         stop and release every synthetic input
//   /mine status       report the current state in chat
//
// /mine is registered in the client CommandRegistry (NativeMessageCommands,
// message-type argument labelled "block", optional), so it appears in the
// game's command suggestions/autocomplete and executes locally. That is the
// only command route; there is no packet-capture path.
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
#include <bedrock/native/message_commands.hpp>
#include <bedrock/native/outline_renderer.hpp>
#include <bedrock/profiles/profile_json.hpp>
#include <bedrock/runtime/hook_runtime.h>
#include <knownfolders.h>
#include <shlobj.h>
#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <chrono>
#include <climits>
#include <cmath>
#include <functional>
#include <deque>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <queue>
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
        if (renderToken_) { BDS_HookUnsubscribe(renderToken_); renderToken_ = nullptr; }
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
            config = {{"enabled", true}, {"radius", 24}, {"show_path", true}, {"skip_underwater", true}};
            std::ofstream file(path); file << config.dump(2) << '\n';
        }
        enabled_ = config.value("enabled", true);
        radius_ = std::clamp(config.value("radius", 24), 4, 48);
        showPath_ = config.value("show_path", true);
        skipUnderwater_ = config.value("skip_underwater", true);
    }

    // -- setup ---------------------------------------------------------------
    void initialize() {
        // Primary command route: register /mine in the client CommandRegistry
        // so it shows in command suggestions and executes locally. The
        // message-type argument carries the whole rest of the line ("dirt",
        // "stop", "status", ...); it is labelled "block" and optional so a
        // bare /mine also executes (and reports status). The handler only
        // queues the line; handling happens on the tick thread.
        if (!registration_.add("mine", "AutoMine: find and mine nearby blocks",
                [this](bedrock::NativeMessageInvocation invocation) {
                    std::lock_guard lock(nativeMutex_);
                    if (nativeLines_.size() < 16) nativeLines_.push_back(std::move(invocation.line));
                }, "block", true))
            throw std::runtime_error("/mine native command definition rejected");
        bedrock::ProfileManager profiles;
        const bedrock::ProfileRequirements required{
            bedrock::Feature::Gameplay, bedrock::Feature::LevelTick, bedrock::Feature::World,
            bedrock::Feature::Motion, bedrock::Feature::Gui, bedrock::Feature::WorldHeight,
            bedrock::Feature::ClientCommands, bedrock::Feature::WorldEvents,
            bedrock::Feature::Camera};
        const bedrock::ProfileOptions options{bedrock::UpdatePolicy::RefreshIfNeeded,
                                              bedrock::CandidatePolicy::AllowCandidates};
        std::string error;
        if (!profiles.prepare(required, options,
                [this](const bedrock::RuntimeProfile& profile, std::string& failure) {
                    if (!gui_.load(profile, failure) || !registration_.load(profile, failure) ||
                        !camera_.load(profile, failure) || !loadWorld(profile, failure))
                        return false;
                    // Path display is cosmetic: never fail startup over it.
                    std::string outlineError;
                    outlineReady_ = outline_.load(profile, outlineError);
                    if (!outlineReady_) note("path renderer unavailable: " + outlineError);
                    return true;
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
                updateRegistration();
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
        if (outlineReady_) {
            if (BDS_HookSubscribe(BDS_HOOK_RENDER_LEVEL, outline_.renderTarget(), &onRenderLevel, this, &renderToken_))
                note("path renderer: subscribed");
            else { outlineReady_ = false; note("path renderer: subscribe failed"); }
        }
        note("AutoMine v2: /mine [block] registered in CommandRegistry (autocomplete)");
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
    // feedMouse). The degrees-per-pixel gains are adapted online, per axis,
    // because they depend on the client's mouse sensitivity. Each tick moves
    // only a damped fraction of the remaining error with a hard per-tick
    // cap, so the view eases onto the target instead of snapping the full
    // error in one tick and hunting around it (the old behaviour read as
    // wandering/lost). Fractional pixels are accumulated across ticks so
    // small corrections neither quantise into jitter nor stall.
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
                if (g > 0.02f && g < 3.f) gainYaw_ = std::clamp(gainYaw_ * 0.75f + g * 0.25f, 0.03f, 2.f);
            }
            lastPx_ = 0;
        }
        if (lastPy_ != 0) {
            const float moved = pitch - lastPitch_;
            if (moved * static_cast<float>(lastPy_) > 0.f) {
                const float g = moved / static_cast<float>(lastPy_);
                if (g > 0.02f && g < 3.f) gainPitch_ = std::clamp(gainPitch_ * 0.75f + g * 0.25f, 0.03f, 2.f);
            }
            lastPy_ = 0;
        }
        const float dyaw = wrapDeg(wantYaw - yaw);
        const float dpitch = std::clamp(wantPitch - pitch, -89.f, 89.f);
        const bool within = std::abs(dyaw) <= tolerance && std::abs(dpitch) <= tolerance;
        if (std::abs(dyaw) <= 0.8f && std::abs(dpitch) <= 0.8f) { resX_ = resY_ = 0.f; return true; }
        const float stepYaw = std::clamp(dyaw * 0.45f, -7.f, 7.f);
        const float stepPitch = std::clamp(dpitch * 0.45f, -7.f, 7.f);
        const float wantPx = stepYaw / gainYaw_ + resX_;
        const float wantPy = stepPitch / gainPitch_ + resY_;
        const int px = static_cast<int>(std::lround(wantPx));
        const int py = static_cast<int>(std::lround(wantPy));
        resX_ = wantPx - static_cast<float>(px);
        resY_ = wantPy - static_cast<float>(py);
        if ((px || py) && feedMove(px, py)) {
            lastPx_ = px; lastPy_ = py; lastYaw_ = yaw; lastPitch_ = pitch;
        }
        return within;
    }
    // Aim through a smoothed point while travelling: the bearing to a block
    // swings hard as the player moves (and movement is view-relative, so a
    // hunting view weaves the whole walk). Chasing a low-passed aim point
    // keeps both the view and the path calm. Mining aims directly instead:
    // its target is static and precision matters there.
    bool faceAim(bedrock::Vec3 eye, bedrock::Vec3 target, float tolerance, float smoothing) {
        if (!aimSmoothInit_) { aimSmooth_ = target; aimSmoothInit_ = true; }
        aimSmooth_.x += (target.x - aimSmooth_.x) * smoothing;
        aimSmooth_.y += (target.y - aimSmooth_.y) * smoothing;
        aimSmooth_.z += (target.z - aimSmooth_.z) * smoothing;
        return faceToward(eye, aimSmooth_, tolerance);
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
    enum class Phase { Idle, Scan, Navigate, Mine, Collect, NeedTool, Escape };
    void resetRun() {
        releaseInputs();
        phase_ = Phase::Idle;
        targetId_.clear();
        blacklist_.clear();
        hazardZones_.clear();
        path_.clear(); pathDig_.clear(); pathIdx_ = 0;
        pathFor_ = {INT_MIN, 0, 0}; replans_ = 0;
        hasTarget_ = false;
        mined_ = 0;
        aimSmoothInit_ = false;
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
        } else if (arg == "path") {
            showPath_ = !showPath_;
            reply(showPath_ ? "経路表示を ON にしたよ" : "経路表示を OFF にしたよ");
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
        case Phase::Escape: return "水から浮上中…（掘った数: " + std::to_string(mined_) + "）";
        }
        return "";
    }

    // Poll the native CommandRegistry registration at a low rate (same
    // cadence as Fake) and log only state changes.
    void updateRegistration() {
        if (registrationTicks_++ % 20) return;
        const auto state = registration_.update(client_.load());
        if (registrationState_ && *registrationState_ == state) return;
        registrationState_ = state;
        using State = bedrock::NativeMessageCommands::State;
        if (state == State::Registered) note("/mine registered in CommandRegistry [autocomplete + local execution]");
        else if (state == State::Collision) note("/mine registration collision: " + registration_.error());
        else if (state == State::Failed) note("/mine registration failed: " + registration_.error());
        else note("/mine registration waiting");
    }

    // Entry point for lines queued by the native CommandRegistry execution.
    void dispatchLine(std::string line) {
        while (!line.empty() && (line.front() == '/' || line.front() == ' ')) line.erase(line.begin());
        while (!line.empty() && (line.back() == ' ' || line.back() == '\t' || line.back() == '\r')) line.pop_back();
        if (line.empty()) return;
        handleLine(line);
    }

    void tick() {
        if (pendingReset_) { pendingReset_ = false; gui_.reset(epoch_.load()); camera_.reset(); resetRun(); }
        {
            std::deque<std::string> native;
            { std::lock_guard lock(nativeMutex_); native.swap(nativeLines_); }
            for (auto& line : native) dispatchLine(std::move(line));
        }
        if (phase_ == Phase::Idle) { clearPath(); return; }
        if (!game_.gameplayActive()) { releaseInputs(); return; }
        bedrock::Vec3 feet;
        if (!feetPos(feet)) { releaseInputs(); return; }
        // Drowning safety net: in any working phase, if the head stays
        // submerged for a few ticks, abandon the target and swim up.
        // Target selection already avoids water, but the standpoint next
        // to a dry-column target can still be below the waterline (e.g.
        // digging into a shore face), and waiting there in NeedTool is
        // what actually drowned the player during verification.
        if (phase_ == Phase::Escape) { tickEscape(feet); updatePath(feet); return; }
        if (phase_ == Phase::Navigate || phase_ == Phase::Mine ||
            phase_ == Phase::Collect || phase_ == Phase::NeedTool) {
            if (headInWater(feet)) {
                if (++submergedTicks_ >= 3) { startEscape(); tickEscape(feet); updatePath(feet); return; }
            } else submergedTicks_ = 0;
        }
        switch (phase_) {
        case Phase::Scan: tickScan(feet); break;
        case Phase::Navigate: tickNavigate(feet); break;
        case Phase::Mine: tickMine(feet); break;
        case Phase::Collect: tickCollect(feet); break;
        case Phase::NeedTool: tickNeedTool(); break;
        case Phase::Idle: case Phase::Escape: break;
        }
        updatePath(feet);
    }

    bool headInWater(const bedrock::Vec3& feet) {
        void* src = region();
        if (!src) return false;
        bedrock::BlockPos bp;
        bp.x = static_cast<int>(std::floor(feet.x));
        bp.y = static_cast<int>(std::floor(feet.y + 1.62f));
        bp.z = static_cast<int>(std::floor(feet.z));
        return contains(world_.getBlock(src, bp).identifier, "water");
    }
    void startEscape() {
        if (hasTarget_) {
            blacklist_.insert(target_);
            hazardZones_.push_back({target_.x, target_.y, target_.z, 6});
            hasTarget_ = false;
        }
        setAttack(false);
        pressKey(kKeyW, false); pressKey(kKeyA, false);
        pressKey(kKeyS, false); pressKey(kKeyD, false);
        phase_ = Phase::Escape;
        escapeTicks_ = 0; dryTicks_ = 0; submergedTicks_ = 0;
        note("escape: head underwater; abandoning target and surfacing");
        reply("水に潜っちゃったから浮上するね。そのブロックは諦めるよ");
    }
    void tickEscape(const bedrock::Vec3& feet) {
        ++escapeTicks_;
        pressKey(kKeySpace, true); // hold jump: swim up / bob at the surface
        if (!headInWater(feet)) {
            if (++dryTicks_ >= 10 || escapeTicks_ > 240) {
                pressKey(kKeySpace, false);
                phase_ = Phase::Scan; scanCursor_ = 0; bestDist2_ = 1e18;
            }
        } else dryTicks_ = 0;
        if (escapeTicks_ > 240 && phase_ == Phase::Escape) {
            pressKey(kKeySpace, false);
            phase_ = Phase::Scan; scanCursor_ = 0; bestDist2_ = 1e18;
        }
    }

    // -- path display ----------------------------------------------------------
    // The selected route, drawn in the world: a terrain-following line from
    // the player's feet to the target block, plus an outline box on the
    // target itself. Points are rebuilt on the tick thread in world space;
    // the render-thread callback converts them against the live render
    // origin and submits them through the SDK outline renderer.
    void clearPath() {
        std::lock_guard lock(pathMutex_);
        pathLine_.clear(); pathBox_ = false;
    }
    void updatePath(const bedrock::Vec3& feet) {
        std::vector<bedrock::Vec3> points;
        bool box = false; BlockPos3 boxPos{};
        if (showPath_ && hasTarget_ && (phase_ == Phase::Navigate || phase_ == Phase::Mine)) {
            const bedrock::Vec3 goal{target_.x + .5f, target_.y + .5f, target_.z + .5f};
            if (!path_.empty() && phase_ == Phase::Navigate) {
                // Draw the actual A* route: remaining waypoints, then goal.
                points.push_back({feet.x, feet.y + 0.25f, feet.z});
                for (std::size_t i = pathIdx_; i < path_.size(); ++i)
                    points.push_back({path_[i].x + .5f, path_[i].y + 0.3f, path_[i].z + .5f});
                points.push_back(goal);
                box = true; boxPos = target_;
                std::lock_guard lock(pathMutex_);
                pathLine_ = std::move(points); pathBox_ = box; pathBoxPos_ = boxPos;
                return;
            }
            const double dx = goal.x - feet.x, dz = goal.z - feet.z;
            const double horiz = std::hypot(dx, dz);
            const int steps = std::clamp(static_cast<int>(horiz / 0.7), 1, 48);
            const int baseY = static_cast<int>(std::floor(feet.y));
            for (int i = 0; i <= steps; ++i) {
                const double t = steps ? static_cast<double>(i) / steps : 1.0;
                const float px = static_cast<float>(feet.x + dx * t);
                const float pz = static_cast<float>(feet.z + dz * t);
                float py = feet.y + 0.25f;
                const int cx = static_cast<int>(std::floor(px));
                const int cz = static_cast<int>(std::floor(pz));
                for (int y = baseY + 2; y >= baseY - 3; --y) {
                    const auto id = blockAt({cx, y, cz});
                    if (!id.empty() && !isAir(id)) { py = static_cast<float>(y) + 1.25f; break; }
                }
                points.push_back({px, py, pz});
            }
            points.push_back(goal);
            box = true; boxPos = target_;
        }
        std::lock_guard lock(pathMutex_);
        pathLine_ = std::move(points); pathBox_ = box; pathBoxPos_ = boxPos;
    }
    static void BDS_HOOK_CALL onRenderLevel(void* user, void* raw, BDS_HookNext next, void* frame) {
        auto& self = *static_cast<AutoMineMod*>(user);
        // Draw AFTER the native world pass: it is what supplies this frame's
        // view/projection, render origin and depth state. Drawing before it
        // (the first attempt) projected the lines through the previous pass
        // and they came out as huge black wireframes.
        next(frame, raw);
        try { self.drawPath(raw); } catch (...) {}
    }
    void drawPath(void* raw) {
        if (!outlineReady_ || !showPath_) return;
        const auto& args = *static_cast<BDS_RenderLevelArgs*>(raw);
        if (!args.renderer || !args.screen) return;
        std::vector<bedrock::Vec3> line; bool box{}; BlockPos3 boxPos{};
        { std::lock_guard lock(pathMutex_); line = pathLine_; box = pathBox_; boxPos = pathBoxPos_; }
        if (line.size() < 2 && !box) return;
        const auto origin = outline_.renderOrigin(args.renderer);
        if (!origin) return;
        auto rel = [&](const bedrock::Vec3& p) {
            return bedrock::Vec3{p.x - origin->x, p.y - origin->y, p.z - origin->z};
        };
        bedrock::WorldOutlineGeometry geometry;
        geometry.origin = *origin;
        if (line.size() >= 2) {
            bedrock::WorldGeometryBatch batch;
            batch.color = {0.25f, 1.f, 0.45f, 1.f};
            for (std::size_t i = 1; i < line.size(); ++i) {
                batch.vertices.push_back(rel(line[i - 1]));
                batch.vertices.push_back(rel(line[i]));
            }
            geometry.lines.push_back(std::move(batch));
        }
        if (box) {
            bedrock::WorldGeometryBatch batch;
            batch.color = {1.f, 0.75f, 0.2f, 1.f};
            const float x0 = static_cast<float>(boxPos.x), y0 = static_cast<float>(boxPos.y),
                        z0 = static_cast<float>(boxPos.z);
            const float x1 = x0 + 1.f, y1 = y0 + 1.f, z1 = z0 + 1.f;
            const bedrock::Vec3 c[8]{{x0, y0, z0}, {x1, y0, z0}, {x1, y0, z1}, {x0, y0, z1},
                                     {x0, y1, z0}, {x1, y1, z0}, {x1, y1, z1}, {x0, y1, z1}};
            constexpr int edges[12][2]{{0, 1}, {1, 2}, {2, 3}, {3, 0}, {4, 5}, {5, 6},
                                       {6, 7}, {7, 4}, {0, 4}, {1, 5}, {2, 6}, {3, 7}};
            for (const auto& e : edges) {
                batch.vertices.push_back(rel(c[e[0]]));
                batch.vertices.push_back(rel(c[e[1]]));
            }
            geometry.lines.push_back(std::move(batch));
        }
        outline_.draw(args.renderer, args.screen, geometry);
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
            // Underwater guard: the player cannot swim, so a target that
            // can only be reached through water ends in a drowning loop.
            // Skip it when any water sits in the column above (up to 16
            // cells: seabed dirt, and dirt buried under the seabed alike).
            // This also skips dry caves under lakes, which is the safe
            // side to err on; dry-land mining is unaffected.
            if (skipUnderwater_) {
                bool underWater = false;
                for (int wy = pos.y + 1; wy <= pos.y + 16; ++wy) {
                    bedrock::BlockPos above; above.x = pos.x; above.y = wy; above.z = pos.z;
                    auto aboveCell = world_.getBlock(src, above);
                    if (contains(aboveCell.identifier, "water")) { underWater = true; break; }
                }
                if (underWater) continue;
            }
            // Hazard zones: spots where an escape already fired (the dry
            // column passed selection, but the only standpoint was in the
            // water). Skip the whole patch so the bot does not chew along
            // a shoreline one escape at a time.
            bool inHazard = false;
            for (const auto& z : hazardZones_) {
                if (std::abs(pos.x - z.x) <= z.r && std::abs(pos.y - z.y) <= z.r &&
                    std::abs(pos.z - z.z) <= z.r) { inHazard = true; break; }
            }
            if (inHazard) continue;
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
            pathFor_ = {INT_MIN, 0, 0}; replans_ = 0; path_.clear(); pathDig_.clear(); pathIdx_ = 0;
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

    // -- pathfinding (A*) ----------------------------------------------------
    // A navigation cell is the block position of the player's feet. Stand =
    // can stand/walk there now; Dig = the feet or head cell must be mined
    // out first (solid ground present); anything else is Blocked. Moves:
    // walk (1.0), step up one block (1.6, needs headroom above the current
    // cell), drop 1-3 blocks (1.0 + 0.3/level, Stand landing, clear shaft),
    // and entering a Dig cell (25.0+: a long detour still beats tunneling,
    // per the design goal of following real paths). Fluids are never
    // entered, so routes
    // stay on land and go around water instead of through it.
    enum class CellKind : std::uint8_t { Stand, Dig, Blocked };
    struct CellKey {
        int x{}, y{}, z{};
        bool operator==(const CellKey&) const noexcept = default;
    };
    struct CellKeyHash {
        std::size_t operator()(const CellKey& k) const noexcept {
            return std::hash<long long>{}((static_cast<long long>(k.x) * 73856093LL) ^
                                          (static_cast<long long>(k.y) * 19349663LL) ^
                                          (static_cast<long long>(k.z) * 83492791LL));
        }
    };
    bool isPassableCell(const std::string& id) {
        if (id.empty()) return false;
        if (isAir(id)) return true;
        const auto l = localPart(id);
        return l == "grass" || l == "short_grass" || l == "tall_grass" || l == "fern" ||
               l == "large_fern" || l == "snow_layer" || l == "dead_bush" ||
               contains(l, "flower") || contains(l, "sapling") || contains(l, "carpet") ||
               l == "torch" || l == "wall_torch";
    }
    bool isFluidCell(const std::string& id) {
        const auto l = localPart(id);
        return contains(l, "water") || contains(l, "lava");
    }
    bool isSolidCell(const std::string& id) {
        return !id.empty() && !isAir(id) && !isFluidCell(id) && !isPassableCell(id);
    }
    bool isDiggableCell(const std::string& id) {
        return isSolidCell(id) && localPart(id) != "bedrock";
    }
    CellKind classifyCell(CellKey c, std::unordered_map<CellKey, CellKind, CellKeyHash>& cache) {
        if (auto it = cache.find(c); it != cache.end()) return it->second;
        CellKind kind = CellKind::Blocked;
        const auto ground = blockAt({c.x, c.y - 1, c.z});
        const auto feetB = blockAt({c.x, c.y, c.z});
        const auto headB = blockAt({c.x, c.y + 1, c.z});
        if (!ground.empty() && !feetB.empty() && !headB.empty() &&
            !isFluidCell(ground) && !isFluidCell(feetB) && !isFluidCell(headB) &&
            isSolidCell(ground)) {
            const bool fP = isPassableCell(feetB), hP = isPassableCell(headB);
            if (fP && hP) kind = CellKind::Stand;
            else if ((fP || isDiggableCell(feetB)) && (hP || isDiggableCell(headB)))
                kind = CellKind::Dig;
        }
        cache.emplace(c, kind);
        return kind;
    }

    bool planPath(bedrock::Vec3 feet) {
        path_.clear(); pathDig_.clear(); pathIdx_ = 0;
        pathFor_ = target_;
        std::unordered_map<CellKey, CellKind, CellKeyHash> cache;
        const CellKey start{static_cast<int>(std::floor(feet.x)),
                            static_cast<int>(std::floor(feet.y)),
                            static_cast<int>(std::floor(feet.z))};
        // Goal cells: Stand/Dig cells around the target whose eye point is
        // within mining reach (4.2) of the target centre.
        std::vector<CellKey> goals;
        for (int dx = -1; dx <= 1; ++dx)
            for (int dy = -1; dy <= 2; ++dy)
                for (int dz = -1; dz <= 1; ++dz) {
                    const CellKey g{target_.x + dx, target_.y + dy, target_.z + dz};
                    if (classifyCell(g, cache) == CellKind::Blocked) continue;
                    const double ex = dx, ey = g.y + 1.62 - (target_.y + .5), ez = dz;
                    if (ex * ex + ey * ey + ez * ez <= 4.2 * 4.2) goals.push_back(g);
                }
        if (goals.empty()) { note("path: no standpoint near target"); return false; }
        auto heuristic = [&](const CellKey& c) {
            double best = 1e18;
            for (const auto& g : goals) {
                const double d = std::sqrt(static_cast<double>(
                    (c.x - g.x) * (c.x - g.x) + (c.y - g.y) * (c.y - g.y) + (c.z - g.z) * (c.z - g.z)));
                if (d < best) best = d;
            }
            return best;
        };
        struct OpenNode {
            double f; CellKey c;
            bool operator<(const OpenNode& o) const noexcept { return f > o.f; } // min-heap
        };
        std::priority_queue<OpenNode> open;
        std::unordered_map<CellKey, double, CellKeyHash> gScore;
        std::unordered_map<CellKey, CellKey, CellKeyHash> cameFrom;
        std::unordered_map<CellKey, bool, CellKeyHash> digFlag;
        gScore[start] = 0;
        open.push({heuristic(start), start});
        constexpr int kDirs[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
        constexpr int kMaxExpansions = 9000;
        int expansions = 0;
        CellKey found{}; bool haveFound = false;
        while (!open.empty() && expansions < kMaxExpansions) {
            const OpenNode cur = open.top(); open.pop();
            const double cg = gScore.count(cur.c) ? gScore[cur.c] : 1e18;
            if (cur.f > cg + heuristic(cur.c) + 1e-9) continue; // stale entry
            ++expansions;
            if (std::find(goals.begin(), goals.end(), cur.c) != goals.end()) {
                found = cur.c; haveFound = true; break;
            }
            auto relax = [&](const CellKey& next, double cost, CellKind kind) {
                const double ng = cg + cost;
                if (!gScore.count(next) || ng < gScore[next] - 1e-9) {
                    gScore[next] = ng;
                    cameFrom[next] = cur.c;
                    digFlag[next] = (kind == CellKind::Dig);
                    open.push({ng + heuristic(next), next});
                }
            };
            for (const auto& d : kDirs) {
                const int nx = cur.c.x + d[0], nz = cur.c.z + d[1];
                {   // walk, same level
                    const CellKey n{nx, cur.c.y, nz};
                    const auto k = classifyCell(n, cache);
                    if (k != CellKind::Blocked) relax(n, k == CellKind::Dig ? 25.0 : 1.0, k);
                }
                {   // step up one block (headroom above the current cell)
                    const CellKey n{nx, cur.c.y + 1, nz};
                    const auto k = classifyCell(n, cache);
                    if (k != CellKind::Blocked &&
                        isPassableCell(blockAt({cur.c.x, cur.c.y + 2, cur.c.z})))
                        relax(n, k == CellKind::Dig ? 25.6 : 1.6, k);
                }
                // drop 1-3 blocks onto a Stand cell, fall shaft passable
                if (isPassableCell(blockAt({nx, cur.c.y + 1, nz}))) {
                    for (int drop = 1; drop <= 3; ++drop) {
                        const CellKey n{nx, cur.c.y - drop, nz};
                        if (classifyCell(n, cache) != CellKind::Stand) break;
                        bool shaft = true;
                        for (int k2 = 0; k2 < drop; ++k2)
                            if (!isPassableCell(blockAt({nx, cur.c.y - k2, nz}))) { shaft = false; break; }
                        if (!shaft) break;
                        relax(n, 1.0 + 0.3 * drop, CellKind::Stand);
                    }
                }
            }
        }
        if (!haveFound) {
            note("path: no route (" + std::to_string(expansions) + " expansions)");
            return false;
        }
        std::vector<CellKey> cells;
        for (CellKey c = found;; c = cameFrom[c]) {
            cells.push_back(c);
            if (c == start) break;
        }
        std::reverse(cells.begin(), cells.end());
        // Keep every Dig cell and every direction/height change; compress
        // straight same-level walking runs to their endpoints.
        for (std::size_t i = 1; i < cells.size(); ++i) {
            const bool dig = digFlag.count(cells[i]) && digFlag[cells[i]];
            const bool last = (i + 1 == cells.size());
            bool keep = last || dig;
            if (!keep) {
                const auto& a = cells[i - 1]; const auto& b = cells[i]; const auto& n = cells[i + 1];
                keep = !(a.y == b.y && b.y == n.y && (b.x - a.x) == (n.x - b.x) &&
                         (b.z - a.z) == (n.z - b.z));
            }
            if (keep) {
                path_.push_back({cells[i].x, cells[i].y, cells[i].z});
                pathDig_.push_back(dig);
            }
        }
        note("path: " + std::to_string(path_.size()) + " waypoints (" +
             std::to_string(expansions) + " expansions)");
        return true;
    }

    void enterMineCell(BlockPos3 cell, const std::string& id) {
        savedTarget_ = target_; savedId_ = targetId_;
        target_ = cell; targetId_ = localPart(id);
        hasTarget_ = true; phase_ = Phase::Mine; mineTicks_ = 0;
        path_.clear(); pathDig_.clear(); pathIdx_ = 0;
        pathFor_ = {INT_MIN, 0, 0};
        note("mine-through: " + targetId_);
    }

    void tickFollowPath(bedrock::Vec3 feet) {
        ++navTicks_;
        if (navTicks_ > 20 * 45) { // same overall budget as legacy navigation
            note("navigate timeout (path); blacklist target");
            blacklist_.insert(target_); hasTarget_ = false;
            pressKey(kKeyW, false); pressKey(kKeySpace, false);
            path_.clear(); pathDig_.clear();
            phase_ = Phase::Scan; scanCursor_ = 0; bestDist2_ = 1e18;
            return;
        }
        if (pathIdx_ >= path_.size()) {
            pressKey(kKeyW, false); pressKey(kKeySpace, false);
            if (++replans_ > 2) {
                note("path exhausted; blacklist target");
                blacklist_.insert(target_); hasTarget_ = false;
                phase_ = Phase::Scan; scanCursor_ = 0; bestDist2_ = 1e18;
            } else pathFor_ = {INT_MIN, 0, 0}; // force a re-plan next tick
            return;
        }
        const BlockPos3 wp = path_[pathIdx_];
        if (pathDig_[pathIdx_]) {
            // Mine out this waypoint's blocking cell, then re-plan (the
            // Mine phase restores the real target via the saved goal).
            pressKey(kKeyW, false); pressKey(kKeySpace, false);
            const auto feetId = blockAt({wp.x, wp.y, wp.z});
            const auto headId = blockAt({wp.x, wp.y + 1, wp.z});
            if (isDiggableCell(feetId)) { enterMineCell({wp.x, wp.y, wp.z}, feetId); return; }
            if (isDiggableCell(headId)) { enterMineCell({wp.x, wp.y + 1, wp.z}, headId); return; }
            ++pathIdx_; // already cleared somehow: move on
            return;
        }
        const bedrock::Vec3 eye{feet.x, feet.y + 1.62f, feet.z};
        const bedrock::Vec3 wpC{wp.x + .5f, wp.y + .5f, wp.z + .5f};
        faceAim(eye, {wpC.x, eye.y, wpC.z}, 10.f, 0.25f);
        pressKey(kKeyW, true);
        const int feetCellY = static_cast<int>(std::floor(feet.y));
        if (wp.y > feetCellY) pressKey(kKeySpace, true);
        else pressKey(kKeySpace, false);
        const double horiz = std::hypot(wpC.x - feet.x, wpC.z - feet.z);
        bool advance = false;
        if (wp.y == feetCellY) advance = horiz < 0.7;
        else if (wp.y > feetCellY) advance = (feetCellY == wp.y && horiz < 0.9);
        else advance = (feet.y <= wp.y + 0.6 && horiz < 1.0);
        if (advance) {
            ++pathIdx_;
            stuckTicks_ = 0;
            return;
        }
        // Stuck on this waypoint: re-plan a few times, then give up on it.
        if (navTicks_ % 10 == 0) {
            const double moved = std::hypot(feet.x - lastX_, feet.z - lastZ_);
            if (moved < 0.12) {
                if (++stuckTicks_ >= 3) {
                    stuckTicks_ = 0;
                    pressKey(kKeyW, false); pressKey(kKeySpace, false);
                    if (++replans_ > 2) {
                        note("path follow stuck; blacklist target");
                        blacklist_.insert(target_); hasTarget_ = false;
                        path_.clear(); pathDig_.clear();
                        phase_ = Phase::Scan; scanCursor_ = 0; bestDist2_ = 1e18;
                    } else pathFor_ = {INT_MIN, 0, 0};
                    return;
                }
            } else stuckTicks_ = 0;
            lastX_ = feet.x; lastZ_ = feet.z;
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
        // Planned route (A*): plan once per target and follow the waypoints
        // when a route exists. No route (or a failed plan) falls back to
        // the legacy straight-line navigation below.
        if (!(pathFor_ == target_)) planPath(feet);
        if (!path_.empty()) { tickFollowPath(feet); return; }
        faceAim(eye, center, 10.f, 0.25f);
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
        faceAim(eye, center, 12.f, 0.3f);
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
    bedrock::NativeMessageCommands registration_;
    std::optional<bedrock::NativeMessageCommands::State> registrationState_;
    unsigned registrationTicks_{};
    std::mutex nativeMutex_;
    std::deque<std::string> nativeLines_;
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
    float gainYaw_{0.15f}, gainPitch_{0.15f};
    float resX_{}, resY_{};
    int lastPx_{}, lastPy_{};
    float lastYaw_{}, lastPitch_{};
    bedrock::Vec3 aimSmooth_{};
    bool aimSmoothInit_{};
    bedrock::NativeOutlineRenderer outline_;
    bool outlineReady_{};
    BDS_HookToken renderToken_{};
    std::mutex pathMutex_;
    std::vector<bedrock::Vec3> pathLine_;
    bool pathBox_{};
    BlockPos3 pathBoxPos_{};
    bool showPath_{true};
    bool skipUnderwater_{true};
    int submergedTicks_{0};
    int escapeTicks_{0};
    int dryTicks_{0};
    struct HazardZone { int x, y, z, r; };
    std::vector<HazardZone> hazardZones_;
    std::vector<BlockPos3> path_;
    std::vector<bool> pathDig_;
    std::size_t pathIdx_{0};
    BlockPos3 pathFor_{INT_MIN, 0, 0};
    int replans_{0};
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
