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
#include <bedrock/native/game_access.hpp>
#include <bedrock/native/runtime_actors.hpp>
#include <bedrock/native/actor_registry.hpp>
#include <bedrock/native/actors.hpp>
#include <bedrock/native/item_stack_requests.hpp>
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
constexpr unsigned kKeySprint = 0xA2; // VK_LCONTROL: Bedrock's sprint key
constexpr unsigned kKeySneak = 0x10; // VK_SHIFT: dismount / sneak

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
// What a block drops when mined with a sufficient tool (for goal counts).
std::string dropOf(const std::string& rawId) {
    const auto id = localPart(rawId);
    if (contains(id, "diamond_ore")) return "diamond";
    if (contains(id, "emerald_ore")) return "emerald";
    if (contains(id, "iron_ore")) return "raw_iron";
    if (contains(id, "gold_ore")) return "raw_gold";
    if (contains(id, "copper_ore")) return "raw_copper";
    if (contains(id, "coal_ore")) return "coal";
    if (contains(id, "redstone_ore")) return "redstone";
    if (contains(id, "lapis_ore")) return "lapis_lazuli";
    if (contains(id, "quartz_ore")) return "quartz";
    return id;
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

// ---------------------------------------------------------------------------
// Crash flight recorder + reporter, and SEH guard rails.
//
// A tiny allocation-free record of what the mod was doing (a section
// breadcrumb + the last notes) lives in static storage. A vectored
// exception handler writes it — plus the faulting module/offset and a
// stack scan — to crash.log when the process dies on a fatal exception,
// so a crash on a machine we cannot debug still says where it happened.
// Separately, SEH guards wrap the dangerous native-call sections (tick,
// craft update, path draw, mouse dispatch) so an access violation inside
// one aborts the operation instead of taking the game down.
// ---------------------------------------------------------------------------
namespace crashrec {
    constexpr int kRingSize = 48;
    constexpr int kLineSize = 168;
    inline char g_ring[kRingSize][kLineSize]{};
    inline volatile LONG g_ringNext = 0;
    inline char g_section[64] = "init";
    inline volatile LONG g_ticks = 0;
    inline volatile LONG g_sehFaults = 0;
    inline volatile LONG g_reported = 0;
    inline HANDLE g_file = INVALID_HANDLE_VALUE;

    inline void copyLine(char* dst, const char* src, int cap) {
        int i = 0; for (; i < cap - 1 && src[i]; ++i) dst[i] = src[i]; dst[i] = 0;
    }
    inline void ringPush(const char* text) {
        const LONG i = InterlockedIncrement(&g_ringNext) - 1;
        copyLine(g_ring[i % kRingSize], text, kLineSize);
    }
    inline void setSection(const char* name) { copyLine(g_section, name, sizeof(g_section)); }
    struct Section {
        char prev[64];
        explicit Section(const char* name) { copyLine(prev, g_section, sizeof(prev)); setSection(name); }
        ~Section() { copyLine(g_section, prev, sizeof(g_section)); }
    };
    inline void append(char*& p, const char* end, const char* s) { while (*s && p < end) *p++ = *s++; }
    inline void appendHex(char*& p, const char* end, unsigned long long v, int width = 0) {
        char tmp[32]; int n = 0;
        if (!v) tmp[n++] = '0';
        while (v) { const int d = static_cast<int>(v & 0xF); tmp[n++] = static_cast<char>(d < 10 ? '0' + d : 'a' + d - 10); v >>= 4; }
        while (n < width) tmp[n++] = '0';
        while (n > 0 && p < end) *p++ = tmp[--n];
    }
    inline void appendDec(char*& p, const char* end, long long v) {
        if (v < 0) { if (p < end) *p++ = '-'; v = -v; }
        char tmp[24]; int n = 0;
        if (!v) tmp[n++] = '0';
        while (v) { tmp[n++] = static_cast<char>('0' + (v % 10)); v /= 10; }
        while (n > 0 && p < end) *p++ = tmp[--n];
    }
    inline void moduleOf(char*& p, const char* end, const void* addr) {
        HMODULE h{};
        if (addr && GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                       GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                       static_cast<LPCWSTR>(addr), &h) && h) {
            wchar_t path[260]{}; const DWORD n = GetModuleFileNameW(h, path, 260);
            const wchar_t* base = path;
            for (DWORD i = 0; i < n; ++i) if (path[i] == L'\\' || path[i] == L'/') base = path + i + 1;
            for (const wchar_t* s = base; *s && p < end; ++s) *p++ = static_cast<char>(*s);
            append(p, end, "+0x");
            appendHex(p, end, static_cast<unsigned long long>(
                reinterpret_cast<const char*>(addr) - reinterpret_cast<const char*>(h)));
        } else append(p, end, "?");
    }
    inline LONG WINAPI veh(EXCEPTION_POINTERS* ep) {
        const DWORD code = ep && ep->ExceptionRecord ? ep->ExceptionRecord->ExceptionCode : 0;
        const bool fatal = code == EXCEPTION_ACCESS_VIOLATION ||
            code == 0xC0000374ul || // STATUS_HEAP_CORRUPTION
            code == EXCEPTION_STACK_OVERFLOW || code == EXCEPTION_ILLEGAL_INSTRUCTION ||
            code == EXCEPTION_INT_DIVIDE_BY_ZERO || code == 0xC0000006ul;
        if (!fatal || InterlockedExchange(&g_reported, 1)) return EXCEPTION_CONTINUE_SEARCH;
        if (g_file == INVALID_HANDLE_VALUE || !ep->ContextRecord) return EXCEPTION_CONTINUE_SEARCH;
        char buf[16384]; char* p = buf; const char* const end = buf + sizeof(buf) - 64;
        append(p, end, "==== AutoMine crash report ====\ncode=0x");
        appendHex(p, end, code, 8);
        append(p, end, " section="); append(p, end, g_section);
        append(p, end, " ticks="); appendDec(p, end, g_ticks);
        append(p, end, " sehFaults="); appendDec(p, end, g_sehFaults);
        append(p, end, "\n");
        auto* ctx = ep->ContextRecord;
        append(p, end, "rip=0x"); appendHex(p, end, ctx->Rip, 16); append(p, end, " ");
        moduleOf(p, end, reinterpret_cast<const void*>(ctx->Rip)); append(p, end, "\n");
        append(p, end, "faultAddr=0x");
        appendHex(p, end, static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(ep->ExceptionRecord->ExceptionAddress)), 16);
        append(p, end, " "); moduleOf(p, end, ep->ExceptionRecord->ExceptionAddress); append(p, end, "\n");
        if (code == EXCEPTION_ACCESS_VIOLATION && ep->ExceptionRecord->NumberParameters >= 2) {
            append(p, end, "av: ");
            append(p, end, ep->ExceptionRecord->ExceptionInformation[0] ? "write" : "read");
            append(p, end, " at 0x"); appendHex(p, end, ep->ExceptionRecord->ExceptionInformation[1], 16);
            append(p, end, "\n");
        }
        append(p, end, "rsp=0x"); appendHex(p, end, ctx->Rsp, 16);
        append(p, end, " rbp=0x"); appendHex(p, end, ctx->Rbp, 16); append(p, end, "\nstack:\n");
        MEMORY_BASIC_INFORMATION mbi{};
        if (VirtualQuery(reinterpret_cast<const void*>(ctx->Rsp), &mbi, sizeof(mbi)) &&
            (mbi.State & MEM_COMMIT)) {
            const auto* q = reinterpret_cast<const unsigned long long*>(ctx->Rsp);
            const auto* limit = reinterpret_cast<const unsigned long long*>(
                static_cast<const char*>(mbi.BaseAddress) + mbi.RegionSize);
            if (limit > q + 96) limit = q + 96;
            for (; q < limit; ++q) {
                const unsigned long long v = *q;
                HMODULE h{};
                if (v > 0x10000 && GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                        GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                        reinterpret_cast<LPCWSTR>(v), &h) && h) {
                    append(p, end, "  0x"); appendHex(p, end, v, 16); append(p, end, " ");
                    moduleOf(p, end, reinterpret_cast<const void*>(v)); append(p, end, "\n");
                }
            }
        }
        append(p, end, "recent notes (oldest first):\n");
        const LONG total = g_ringNext;
        const LONG start = total > kRingSize ? total - kRingSize : 0;
        for (LONG i = start; i < total; ++i) {
            append(p, end, "  "); append(p, end, g_ring[i % kRingSize]); append(p, end, "\n");
        }
        append(p, end, "==== end ====\n");
        DWORD written{};
        WriteFile(g_file, buf, static_cast<DWORD>(p - buf), &written, nullptr);
        FlushFileBuffers(g_file);
        return EXCEPTION_CONTINUE_SEARCH;
    }
    inline void install(const std::filesystem::path& dir) {
        const auto path = dir / L"crash.log";
        g_file = CreateFileW(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ, nullptr,
                             OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        AddVectoredExceptionHandler(1, &veh);
    }
} // namespace crashrec

class AutoMineMod {
public:
    std::atomic<unsigned> status{};
    void signalStop() { stopped_ = true; }
    void stop() {
        signalStop();
        if (renderToken_) { BDS_HookUnsubscribe(renderToken_); renderToken_ = nullptr; }
        if (mouseToken_) { BDS_HookUnsubscribe(mouseToken_); mouseToken_ = nullptr; }
        craft_.stop();
        gui_.stop();
        releaseInputs();
    }
    void run() {
        status = 1;
        try {
            const auto dir = directory();
            std::filesystem::create_directories(dir);
            crashrec::install(dir);
            loadConfig(dir);
            initialize();
            status = 2;
            std::ofstream out(dir / L"AutoMine.log", std::ios::trunc);
            while (!stopped_.load()) {
                flush(out);
                if (crashTestPending_.exchange(false)) {
                    // Deliberate unguarded fault OUTSIDE the tick guards, to
                    // verify the crash reporter end to end (/mine crashtest).
                    crashrec::setSection("crashtest");
                    crashrec::ringPush("crashtest: deliberate fault");
                    volatile int* bad = nullptr; *bad = 42;
                }
                std::this_thread::sleep_for(100ms);
            }
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
        crashrec::ringPush(text.c_str());
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
        craft_.onDiagnostic([this](const std::string& text) { note("craft: " + text); });
        bedrock::ProfileManager profiles;
        const bedrock::ProfileRequirements required{
            bedrock::Feature::Gameplay, bedrock::Feature::LevelTick, bedrock::Feature::World,
            bedrock::Feature::Motion, bedrock::Feature::Gui, bedrock::Feature::WorldHeight,
            bedrock::Feature::ClientCommands, bedrock::Feature::WorldEvents,
            bedrock::Feature::Actors, bedrock::Feature::Camera};
        const bedrock::ProfileOptions options{bedrock::UpdatePolicy::RefreshIfNeeded,
                                              bedrock::CandidatePolicy::AllowCandidates};
        std::string error;
        if (!profiles.prepare(required, options,
                [this](const bedrock::RuntimeProfile& profile, std::string& failure) {
                    if (!gui_.load(profile, failure) || !registration_.load(profile, failure) ||
                        !camera_.load(profile, failure) || !loadWorld(profile, failure))
                        return false;
                    // AutoCraft (real ItemStackRequest crafting) is
                    // optional: mining keeps working without it.
                    std::string craftError;
                    craftReady_ = craft_.load(profile, craftError);
                    if (!craftReady_) note("craft adapter unavailable: " + craftError);
                    else craftProfileJson_ = profile.json();
                    // Path display is cosmetic: never fail startup over it.
                    std::string outlineError;
                    outlineReady_ = outline_.load(profile, outlineError);
                    if (!outlineReady_) note("path renderer unavailable: " + outlineError);
                    // Actor access (combat) is optional too.
                    std::string actorError;
                    actorsReady_ = loadActors(profile, actorError);
                    if (!actorsReady_) note("actor access unavailable: " + actorError);
                    loadScreen(profile); // verified screen state for item moves
                    std::string accessError;
                    if (!access_.load(profile, accessError))
                        note("game access unavailable: " + accessError);
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
                level_.store(event.level);
                auto scope = gui_.tick(client_.load(), event.level, epoch_.load());
                if (!scope) return;
                auto camScope = camera_.tick(client_.load(), bedrock::WorldEpoch{epoch_.load()});
                snap_ = camera_.readPlayerProxy();
                updateRegistration();
                if (craftReady_) craftUpdateGuarded(event.level);
                tickGuarded();
            } catch (const std::exception& e) {
                releaseInputs();
                note(std::string("tick failed: ") + e.what());
            }
        });
        if (craftReady_) {
            std::string packetError;
            if (!craft_.installPackets(craftProfileJson_, packetError)) {
                craftReady_ = false;
                note("craft packets unavailable: " + packetError);
            }
        }
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
    std::array<InvSlot, 9> hotbarMemoryItems() {
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
    std::array<InvSlot, 9> hotbarItems() {
        auto out = hotbarMemoryItems();
        // The snapshot is the authority for the hotbar. It merges the
        // same memory read with the response-tracked shadow of every
        // forged request, and retires a shadow entry the moment the
        // memory view diverges from it — so wherever the snapshot has
        // an entry it is at least as fresh as the memory read, and for
        // slots a forged craft/transfer touched it is the ONLY correct
        // view. Memory-first masking was measured broken on
        // 2026-11-18: the crafted wooden pickaxe sat at hotbar 4 in
        // the snapshot while the memory read still showed the slot's
        // stale pre-craft stack, chooseTool never saw the pickaxe,
        // and NeedTool waited forever with the tool in hand. (The
        // earlier fear that the snapshot misplaces takes was traced
        // to the fy+1 placement adopt gap instead.) Slots the
        // snapshot does not cover keep the memory read.
        if (craftReady_) {
            for (const auto& slot : craft_.snapshot()) {
                if (slot.slot < 0 || slot.slot >= 9) continue;
                out[static_cast<std::size_t>(slot.slot)] = {slot.item, slot.count};
            }
        }
        return out;
    }
    // Every hotbar slot either inventory view claims holds the item.
    // The place loops cycle through these: a single wrong claim no
    // longer strands placement on an empty slot.
    std::vector<int> hotbarSlotCandidates(const std::string& local) {
        std::vector<int> out;
        auto addFrom = [&](const std::array<InvSlot, 9>& slots) {
            for (int i = 0; i < 9; ++i) {
                const auto& it = slots[static_cast<std::size_t>(i)];
                if (it.count && localPart(it.id) == local &&
                    std::find(out.begin(), out.end(), i) == out.end())
                    out.push_back(i);
            }
        };
        addFrom(hotbarItems());
        addFrom(hotbarMemoryItems());
        return out;
    }
    // The slot to hold while placing: rotate the claimed slots,
    // 40 ticks each, until the block lands.
    int placeSlotTick(const std::string& local, std::size_t& idx, int& ticks, int fallback) {
        const auto cands = hotbarSlotCandidates(local);
        if (cands.empty()) return fallback;
        if (idx >= cands.size()) idx = 0;
        const int slot = cands[idx];
        if (++ticks > 40) { ticks = 0; idx = (idx + 1) % cands.size(); }
        return slot;
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
    static uintptr_t dispatchMouseSeh(void* args) {
        __try { return BDS_HookDispatch(BDS_HOOK_MOUSE_FEED, args); }
        __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
    }
    bool feedMouse(int action, int state, int dx, int dy) {
        crashrec::Section section("feedMouse");
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
        dispatchMouseSeh(&args);
        {
            std::lock_guard lock(mouseMutex_);
            lastMouse_.x = args.x;
            lastMouse_.y = args.y;
        }
        return true;
    }
    bool feedMove(int dx, int dy) { return feedMouse(0, 0, dx, dy); }
    bool feedButton(bool down) { return feedMouse(1, down ? 1 : 0, 0, 0); }
    // Right button ("use item": bow draw, block placement). The encoding
    // (action=2, state=1/0) was confirmed against real captured events
    // from the live client; the GameApi route runs in parallel, exactly
    // like setAttack does for the left button.
    bool feedUseButton(bool down) { return feedMouse(2, down ? 1 : 0, 0, 0); }
    void setUse(bool down) {
        if (useHeld_ == down) return;
        if (feedUseButton(down) || mouse().setDown(bedrock::MouseButton::Right, down))
            useHeld_ = down;
    }
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
        for (auto key : {kKeyW, kKeyA, kKeyS, kKeyD, kKeySpace, kKeySneak, kKeySprint}) {
            if (heldKeys_.erase(key)) { try { keyboard().release(key); } catch (...) {} }
        }
        if (attackHeld_) { try { feedButton(false); mouse().setDown(bedrock::MouseButton::Left, false); } catch (...) {} attackHeld_ = false; }
        if (useHeld_) { try { feedUseButton(false); mouse().setDown(bedrock::MouseButton::Right, false); } catch (...) {} useHeld_ = false; }
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
            // Fluids and no-collision ground cover (grass, leaf
            // litter, ...) do not block the targeting ray: treating
            // leaf_litter as opaque made the mine phase dig at a
            // decoration it cannot break (its hitbox sits at the
            // cell's floor, under the aim point) and loop forever
            // on one log (measured 2026-11-17 on fresh3).
            if (!id.empty() && !isAir(id) && !contains(localPart(id), "water") &&
                !contains(localPart(id), "lava") && !isPassableCell(id)) {
                hit = {cx, cy, cz}; return false;
            }
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
    // Soft durability tracking: the item layout exposes no damage
    // value, so wear is estimated by counting blocks broken with
    // each hotbar slot's tool (reset when the slot's item changes).
    // Estimates drive conservative decisions only (prefer the
    // fresher of two equal tools, craft a spare before a long
    // descent), so drift fails safe.
    static int maxDurabilityOf(const std::string& local) {
        if (!contains(local, "_pickaxe") && !contains(local, "_axe") &&
            !contains(local, "_sword") && !contains(local, "_shovel") &&
            !contains(local, "_hoe"))
            return 0;
        if (contains(local, "netherite_")) return 2031;
        if (contains(local, "diamond_")) return 1561;
        if (contains(local, "iron_")) return 250;
        if (contains(local, "stone_")) return 131;
        if (contains(local, "golden_")) return 32;
        if (contains(local, "wooden_")) return 59;
        return 0;
    }
    std::array<std::string, 9> toolIdAtSlot_;
    std::array<int, 9> toolUsedAtSlot_{};
    void chargeToolUse() {
        void* p = player();
        if (!p) return;
        const int slot = world_.selected(p);
        if (slot < 0 || slot > 8) return;
        const auto slots = hotbarItems();
        const auto& it = slots[static_cast<std::size_t>(slot)];
        if (it.id.empty()) { toolIdAtSlot_[static_cast<std::size_t>(slot)].clear(); return; }
        if (maxDurabilityOf(localPart(it.id)) == 0) return;
        if (toolIdAtSlot_[static_cast<std::size_t>(slot)] != it.id) {
            toolIdAtSlot_[static_cast<std::size_t>(slot)] = it.id;
            toolUsedAtSlot_[static_cast<std::size_t>(slot)] = 0;
        }
        ++toolUsedAtSlot_[static_cast<std::size_t>(slot)];
    }
    int toolRemainingAt(int slot) { // -1: not a tracked tool
        const auto slots = hotbarItems();
        if (slot < 0 || slot > 8) return -1;
        const auto& it = slots[static_cast<std::size_t>(slot)];
        const int max = it.id.empty() ? 0 : maxDurabilityOf(localPart(it.id));
        if (!max) return -1;
        if (toolIdAtSlot_[static_cast<std::size_t>(slot)] != it.id) return max; // untouched here
        return std::max(0, max - toolUsedAtSlot_[static_cast<std::size_t>(slot)]);
    }
    // Best (largest) remaining-durability estimate among hotbar
    // pickaxes of at least minTier; -1 when none is held.
    int bestPickaxeRemaining(int minTier) {
        int best = -1;
        const auto slots = hotbarItems();
        for (int slot = 0; slot < 9; ++slot) {
            const auto& it = slots[static_cast<std::size_t>(slot)];
            if (!it.count || it.id.empty()) continue;
            int t{};
            if (classForItem(localPart(it.id), t) != ToolClass::Pickaxe || t < minTier) continue;
            best = std::max(best, toolRemainingAt(slot));
        }
        return best;
    }
    // A pickaxe of minTier+ sitting in the main inventory counts as
    // a fresh spare (it is not the one being worn down).
    bool hasSparePickaxe(int minTier) {
        if (!craftReady_) return false;
        for (const auto& s : craft_.snapshot()) {
            if (!s.count || s.slot < 9) continue;
            int t{};
            if (classForItem(localPart(s.item), t) == ToolClass::Pickaxe && t >= minTier)
                return true;
        }
        return false;
    }
    struct ToolChoice { int slot{-1}; int tier{}; ToolClass kind{ToolClass::None}; bool byHand{}; };
    ToolChoice chooseTool(const std::string& blockId) {
        ToolChoice best;
        int bestRemaining = -1;
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
            const int rem = toolRemainingAt(slot);
            if (slot == best.slot || tier > best.tier ||
                (tier == best.tier && rem > bestRemaining)) {
                best.slot = slot; best.tier = tier; best.kind = kind;
                bestRemaining = rem;
            }
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

    // -- combat --------------------------------------------------------------
    // Mob type ids read from the actor type component on the live client
    // (1.26.60.5, verified by controlled summons on the test server —
    // see README). 199534/199540 are the zombie-family neighbours of the
    // zombie id (199456): every live sighting fit drowned (in water) and
    // a zombie variant (a 0.49x0.98 baby form exists), so both count as
    // hostile.
    static bool isHostileType(std::uint32_t t) {
        switch (t) {
        case 199456:  // zombie
        case 1116962: // skeleton
        case 264995:  // spider
        case 2849:    // creeper
        case 199534:  // drowned (zombie family)
        case 199540:  // zombie variant (zombie family)
            return true;
        default: return false;
        }
    }
    static bool isCreeperType(std::uint32_t t) { return t == 2849; }
    struct MobInfo {
        std::uint32_t eid{}; std::uint32_t type{};
        bedrock::Vec3 center{}; bedrock::Vec3 bmin{}; bedrock::Vec3 bmax{};
        bool hasBounds{}; double dist{1e18};
    };
    // Nearest hostile actor within maxDist of `feet` (3D, to the mob
    // centre). onlyEid != 0 restricts the search to that one actor.
    std::optional<MobInfo> scanHostile(const bedrock::Vec3& feet, double maxDist,
                                       std::uint32_t onlyEid = 0) {
        if (!actorsReady_) return std::nullopt;
        const auto snap = runtimeActors_.snapshot(level_.load());
        if (!snap) return std::nullopt;
        void* self = world_.localPlayer(client_.load());
        std::optional<MobInfo> best;
        for (void* a : *snap) {
            if (a == self) continue;
            const auto type = actorTypeOf(a);
            if (!type || !isHostileType(*type)) continue;
            std::uint32_t eid = 0;
            actorRegistry_.readEntityId(a, eid);
            if (onlyEid && eid != onlyEid) continue;
            bedrock::Vec3 pos;
            if (!actorRegistry_.readPosition(a, pos)) continue;
            MobInfo m; m.eid = eid; m.type = *type;
            bedrock::ActorBounds b{};
            if (actorRegistry_.readBounds(a, b)) {
                m.hasBounds = true; m.bmin = b.minimum; m.bmax = b.maximum;
                m.center = {(b.minimum.x + b.maximum.x) * .5f,
                            (b.minimum.y + b.maximum.y) * .5f,
                            (b.minimum.z + b.maximum.z) * .5f};
            } else {
                m.center = {pos.x, pos.y + .9f, pos.z};
            }
            m.dist = std::sqrt((m.center.x - feet.x) * (m.center.x - feet.x) +
                               (m.center.y - feet.y) * (m.center.y - feet.y) +
                               (m.center.z - feet.z) * (m.center.z - feet.z));
            if (!onlyEid && combatIgnore_.count(eid)) continue; // gave up on it recently
            if (m.dist <= maxDist && (!best || m.dist < best->dist)) best = m;
        }
        return best;
    }
    // Passive food animals (type ids measured live 2026-11-18 by
    // controlled summons + /mine mobs: bounds and positions matched
    // per species; the four ids are consecutive).
    static bool isFoodAnimalType(std::uint32_t t) {
        switch (t) {
        case 4874: // chicken (0.30x0.40)
        case 4875: // cow (0.90x1.30)
        case 4876: // pig (0.90x0.90)
        case 4877: // sheep (0.90x1.30)
            return true;
        default: return false;
        }
    }
    static const char* rawMeatOf(std::uint32_t t) {
        switch (t) {
        case 4874: return "chicken";
        case 4875: return "beef";
        case 4876: return "porkchop";
        case 4877: return "mutton";
        default: return "";
        }
    }
    static std::string cookedMeatOf(const std::string& raw) {
        if (raw == "beef") return "cooked_beef";
        if (raw == "porkchop") return "cooked_porkchop";
        if (raw == "chicken") return "cooked_chicken";
        if (raw == "mutton") return "cooked_mutton";
        return "";
    }
    // Nearest food animal within maxDist (onlyEid restricts to one).
    std::optional<MobInfo> scanPassive(const bedrock::Vec3& feet, double maxDist,
                                       std::uint32_t onlyEid = 0) {
        if (!actorsReady_) return std::nullopt;
        const auto snap = runtimeActors_.snapshot(level_.load());
        if (!snap) return std::nullopt;
        void* self = world_.localPlayer(client_.load());
        std::optional<MobInfo> best;
        for (void* a : *snap) {
            if (a == self) continue;
            const auto type = actorTypeOf(a);
            if (!type || !isFoodAnimalType(*type)) continue;
            std::uint32_t eid = 0;
            actorRegistry_.readEntityId(a, eid);
            if (onlyEid && eid != onlyEid) continue;
            bedrock::Vec3 pos;
            if (!actorRegistry_.readPosition(a, pos)) continue;
            MobInfo m; m.eid = eid; m.type = *type;
            bedrock::ActorBounds b{};
            if (actorRegistry_.readBounds(a, b)) {
                m.hasBounds = true; m.bmin = b.minimum; m.bmax = b.maximum;
                m.center = {(b.minimum.x + b.maximum.x) * .5f,
                            (b.minimum.y + b.maximum.y) * .5f,
                            (b.minimum.z + b.maximum.z) * .5f};
            } else {
                m.center = {pos.x, pos.y + .9f, pos.z};
            }
            m.dist = std::sqrt((m.center.x - feet.x) * (m.center.x - feet.x) +
                               (m.center.y - feet.y) * (m.center.y - feet.y) +
                               (m.center.z - feet.z) * (m.center.z - feet.z));
            if (m.dist <= maxDist && (!best || m.dist < best->dist)) best = m;
        }
        return best;
    }
    // Distance from a point to the mob's AABB (0 when inside): the reach
    // metric — a hit counts when this is within the game's ~3 block reach.
    static double distToMob(const bedrock::Vec3& p, const MobInfo& m) {
        if (!m.hasBounds) {
            return std::sqrt((m.center.x - p.x) * (m.center.x - p.x) +
                             (m.center.y - p.y) * (m.center.y - p.y) +
                             (m.center.z - p.z) * (m.center.z - p.z));
        }
        const double dx = p.x < m.bmin.x ? m.bmin.x - p.x : (p.x > m.bmax.x ? p.x - m.bmax.x : 0);
        const double dy = p.y < m.bmin.y ? m.bmin.y - p.y : (p.y > m.bmax.y ? p.y - m.bmax.y : 0);
        const double dz = p.z < m.bmin.z ? m.bmin.z - p.z : (p.z > m.bmax.z ? p.z - m.bmax.z : 0);
        return std::sqrt(dx * dx + dy * dy + dz * dz);
    }
    struct Weapon { int slot{-1}; int kind{0}; int tier{-1}; }; // kind: 1 sword, 2 axe, 3 bow
    static int weaponTierOf(const std::string& lp) {
        if (lp.rfind("netherite_", 0) == 0) return 5;
        if (lp.rfind("diamond_", 0) == 0) return 4;
        if (lp.rfind("iron_", 0) == 0) return 3;
        if (lp.rfind("stone_", 0) == 0) return 2;
        if (lp.rfind("golden_", 0) == 0) return 1;
        if (lp.rfind("wooden_", 0) == 0) return 1;
        return 0;
    }
    Weapon bestMelee() {
        Weapon best;
        const auto slots = hotbarItems();
        for (int i = 0; i < 9; ++i) {
            const auto& it = slots[static_cast<std::size_t>(i)];
            if (!it.count || it.id.empty()) continue;
            const auto lp = localPart(it.id);
            int kind = 0;
            if (lp.size() > 6 && lp.compare(lp.size() - 6, 6, "_sword") == 0) kind = 1;
            // "pickaxe" cannot false-match: it ends in "kaxe", not "_axe".
            else if (lp.size() > 4 && lp.compare(lp.size() - 4, 4, "_axe") == 0) kind = 2;
            if (!kind) continue;
            const int tier = weaponTierOf(lp);
            // Swords win ties (faster swing); then higher tier.
            if (best.slot < 0 || tier > best.tier || (tier == best.tier && kind < best.kind)) {
                best.slot = i; best.kind = kind; best.tier = tier;
            }
        }
        return best;
    }
    int bowSlot() {
        const auto slots = hotbarItems();
        for (int i = 0; i < 9; ++i) {
            const auto& it = slots[static_cast<std::size_t>(i)];
            if (it.count && localPart(it.id) == "bow") return i;
        }
        return -1;
    }
    bool hasArrows() {
        for (const auto& it : hotbarItems())
            if (it.count && localPart(it.id) == "arrow") return true;
        return false;
    }
    void selectSlot(int slot) {
        if (slot < 0) return;
        void* p = player();
        if (p && world_.selected(p) != slot) world_.select(p, static_cast<unsigned>(slot));
    }
    enum class CombatMode { Melee, Bow, Flee };
    void enterCombat(const MobInfo& mob) {
        releaseInputs();
        setAttack(false);
        resumePhase_ = phase_;
        phase_ = Phase::Combat;
        combatEid_ = mob.eid; combatType_ = mob.type;
        combatTicks_ = 0; attackCd_ = 0; attackPulse_ = 0; retreatTicks_ = 0;
        bowDraw_ = 0; bowCd_ = 0; landedHits_ = 0; lastEngageTick_ = 0; clearPath();
        const int bow = bowSlot();
        const Weapon melee = bestMelee();
        if (isCreeperType(mob.type) && bow >= 0 && hasArrows()) {
            combatMode_ = CombatMode::Bow; selectSlot(bow);
        } else if (melee.slot >= 0 || !isCreeperType(mob.type)) {
            combatMode_ = CombatMode::Melee;
            if (melee.slot >= 0) selectSlot(melee.slot);
        } else {
            combatMode_ = CombatMode::Flee; // creeper, no bow, no weapon
        }
        note("combat: start type=" + std::to_string(mob.type) +
             " mode=" + std::to_string(static_cast<int>(combatMode_)));
        reply("敵が来た！応戦するよ");
    }
    void exitCombat(bool won) {
        releaseInputs();
        setAttack(false);
        setUse(false);
        if (!won && combatEid_) // unreachable or timed out: don't retrigger at once
            combatIgnore_[combatEid_] = tickCount_ + 20 * 60;
        if (won && landedHits_ > 0) reply("やっつけたよ");
        note("combat: end won=" + std::to_string(won ? 1 : 0));
        combatEid_ = 0;
        if ((resumePhase_ == Phase::Navigate || resumePhase_ == Phase::Mine ||
             resumePhase_ == Phase::Collect || resumePhase_ == Phase::NeedTool) &&
            hasTarget_ && !targetId_.empty() &&
            localPart(blockAt(target_)) == targetId_) {
            phase_ = resumePhase_;
            pathFor_ = {INT_MIN, 0, 0}; replans_ = 0;
        } else if (resumePhase_ == Phase::Idle) {
            phase_ = Phase::Idle; // idle self-defense: go back to waiting
        } else {
            phase_ = Phase::Scan; scanCursor_ = 0; bestDist2_ = 1e18;
        }
        resumePhase_ = Phase::Scan;
    }
    void tickCombat(const bedrock::Vec3& feet) {
        ++combatTicks_;
        if (combatTicks_ > 20 * 90) { exitCombat(false); return; } // 90 s cap
        // No engagement for 25 s (never got in reach / never had a clear
        // shot): the target is unreachable from here — give up on it for
        // a minute instead of re-entering combat forever.
        if (combatTicks_ - lastEngageTick_ > 20 * 25) { exitCombat(false); return; }
        auto mob = scanHostile(feet, 40.0, combatEid_);
        if (!mob) {
            // Target died or left: retarget to any other nearby hostile,
            // otherwise the fight is over.
            if (auto other = scanHostile(feet, 14.0)) {
                combatEid_ = other->eid; combatType_ = other->type; mob = other;
                note("combat: retarget type=" + std::to_string(other->type));
            } else { exitCombat(true); return; }
        }
        const bedrock::Vec3 eye{feet.x, feet.y + 1.62f, feet.z};
        BlockPos3 hitCell{};
        const BlockPos3 mobCell{static_cast<int>(std::floor(mob->center.x)),
                                static_cast<int>(std::floor(mob->center.y)),
                                static_cast<int>(std::floor(mob->center.z))};
        const bool los = lineOfSight(eye, mobCell, hitCell);
        const double reach = distToMob(eye, *mob);
        const bool wet = inWaterNow(feet);
        if (attackCd_ > 0) --attackCd_;
        if (attackPulse_ > 0 && --attackPulse_ == 0) {
            setAttack(false);
            if (retreatAfterHit_) { retreatAfterHit_ = false; retreatTicks_ = 10; }
        }
        if (combatMode_ == CombatMode::Flee) {
            // Run directly away from the mob until it is far enough.
            lastEngageTick_ = combatTicks_; // fleeing is the engagement
            bedrock::Vec3 away{feet.x * 2 - mob->center.x, mob->center.y, feet.z * 2 - mob->center.z};
            faceAim(eye, away, 6.f, .4f);
            pressKey(kKeyW, true);
            if (wet) pressKey(kKeySpace, true);
            if (mob->dist > 16.0) exitCombat(true);
            return;
        }
        if (combatMode_ == CombatMode::Bow) {
            const double d = std::sqrt((mob->center.x - eye.x) * (mob->center.x - eye.x) +
                                       (mob->center.y - eye.y) * (mob->center.y - eye.y) +
                                       (mob->center.z - eye.z) * (mob->center.z - eye.z));
            if (bowDraw_ > 0) {
                // Drawing: hold still, track the mob, release at full draw.
                pressKey(kKeyW, false); pressKey(kKeyS, false);
                if (wet) pressKey(kKeySpace, true);
                bedrock::Vec3 aim = mob->center; aim.y += static_cast<float>(d * 0.02);
                faceToward(eye, aim, 2.5f);
                if (!los) { setUse(false); bowDraw_ = 0; bowCd_ = 6; }
                else if (--bowDraw_ == 0) { setUse(false); bowCd_ = 9; ++landedHits_; lastEngageTick_ = combatTicks_; }
                return;
            }
            if (bowCd_ > 0) --bowCd_;
            faceAim(eye, mob->center, 4.f, .35f);
            if (!los || d > 14.0) { // close in until the shot is clear
                pressKey(kKeyS, false); pressKey(kKeyW, true);
                if (wet) pressKey(kKeySpace, true);
            } else if (d < 7.0) {   // too close: back off while facing it
                pressKey(kKeyW, false); pressKey(kKeyS, true);
            } else {
                pressKey(kKeyW, false); pressKey(kKeyS, false);
                if (bowCd_ == 0) { setUse(true); bowDraw_ = 22; }
            }
            return;
        }
        // Melee.
        if (retreatTicks_ > 0) {
            --retreatTicks_;
            faceAim(eye, mob->center, 5.f, .4f);
            pressKey(kKeyW, false); pressKey(kKeyS, true);
            if (wet) pressKey(kKeySpace, true);
            return;
        }
        pressKey(kKeyS, false);
        if (reach > 2.9 || !los) {
            faceAim(eye, mob->center, 5.f, .35f);
            pressKey(kKeyW, true);
            if (wet || mob->center.y > feet.y + .6f) pressKey(kKeySpace, true);
            else pressKey(kKeySpace, false);
            return;
        }
        pressKey(kKeyW, false);
        pressKey(kKeySpace, wet); // stay at the surface while striking
        faceToward(eye, mob->center, 3.f);
        if (attackCd_ == 0) {
            setAttack(true);
            attackPulse_ = 2; attackCd_ = 11; ++landedHits_; lastEngageTick_ = combatTicks_;
            if (isCreeperType(combatType_)) retreatAfterHit_ = true;
        }
    }

    // -- boat ------------------------------------------------------------------
    // Long water crossings are faster (and safer) by boat when the hotbar
    // carries one: place it on the water, board it with the use button,
    // steer it with the view + forward key, and sneak to dismount at the
    // far shore. Every step has a timeout back to plain swimming, and a
    // target gets at most one boat attempt per run.
    int boatSlot() {
        const auto slots = hotbarItems();
        for (int i = 0; i < 9; ++i) {
            const auto& it = slots[static_cast<std::size_t>(i)];
            if (it.count && contains(localPart(it.id), "_boat")) return i;
        }
        return -1;
    }
    // Boats are actor type 90 (measured live). A shape-based fallback —
    // the only wide, flat actor on open water: 1.2-1.7 blocks wide and
    // at most 1 block tall, not an item and not a hostile — covers the
    // id shifting. Boats persist in the world across sessions, so
    // Deploy must only accept a hull that was NOT already floating
    // nearby when the attempt began (preBoatEids_); Board/Ride then
    // track that exact hull by eid (boatEid_).
    std::optional<MobInfo> findBoat(const bedrock::Vec3& feet, double maxDist,
                                    std::uint32_t onlyEid = 0, bool freshOnly = false) {
        if (!actorsReady_) return std::nullopt;
        const auto snap = runtimeActors_.snapshot(level_.load());
        if (!snap) return std::nullopt;
        void* self = world_.localPlayer(client_.load());
        std::optional<MobInfo> best, bestTyped;
        for (void* a : *snap) {
            if (a == self) continue;
            const auto type = actorTypeOf(a);
            if (!type || *type == 64 || isHostileType(*type)) continue;
            std::uint32_t eid = 0;
            actorRegistry_.readEntityId(a, eid);
            if (onlyEid && eid != onlyEid) continue;
            if (freshOnly && eid &&
                std::find(preBoatEids_.begin(), preBoatEids_.end(), eid) != preBoatEids_.end())
                continue;
            bedrock::ActorBounds b{};
            if (!actorRegistry_.readBounds(a, b)) continue;
            const float w = b.maximum.x - b.minimum.x, h = b.maximum.y - b.minimum.y;
            if (*type != 90 && (w < 1.2f || w > 1.7f || h > 1.0f)) continue;
            MobInfo m; m.type = *type; m.eid = eid; m.hasBounds = true; m.bmin = b.minimum; m.bmax = b.maximum;
            m.center = {(b.minimum.x + b.maximum.x) * .5f, (b.minimum.y + b.maximum.y) * .5f,
                        (b.minimum.z + b.maximum.z) * .5f};
            m.dist = std::hypot(m.center.x - feet.x, m.center.z - feet.z);
            if (m.dist > maxDist) continue;
            if (*type == 90) { if (!bestTyped || m.dist < bestTyped->dist) bestTyped = m; }
            else if (!best || m.dist < best->dist) best = m;
        }
        return bestTyped ? bestTyped : best;
    }
    enum class BoatStep { Deploy, Board, Ride, Dismount };
    void enterBoat() {
        releaseInputs();
        phase_ = Phase::Boat; boatStep_ = BoatStep::Deploy;
        boatTicks_ = 0; boatStepTicks_ = 0; boatPulse_ = 0; boatCd_ = 0; boatLockTicks_ = 0;
        boatTriedFor_ = target_;
        boatEid_ = 0; preBoatEids_.clear();
        boatGlueTicks_ = 0; boatProbeTicks_ = 0;
        // Remember every hull already afloat nearby: boats persist in
        // the world, and accepting a stale one as "just placed" sent a
        // verification run chasing a boat from an earlier session.
        if (actorsReady_) {
            if (auto snap = runtimeActors_.snapshot(level_.load())) {
                void* self = world_.localPlayer(client_.load());
                for (void* a : *snap) {
                    if (a == self) continue;
                    const auto type = actorTypeOf(a);
                    if (!type || *type != 90) continue;
                    std::uint32_t eid = 0;
                    actorRegistry_.readEntityId(a, eid);
                    if (eid) preBoatEids_.push_back(eid);
                }
            }
        }
        note("boat: deploying for the crossing");
        reply("ボートで渡るね");
    }
    void giveUpBoat() {
        releaseInputs();
        note("boat: gave up; swimming instead");
        phase_ = Phase::Navigate; navTicks_ = 0;
    }
    void tickBoat(const bedrock::Vec3& feet) {
        ++boatTicks_; ++boatStepTicks_;
        const bedrock::Vec3 eye{feet.x, feet.y + 1.62f, feet.z};
        const bedrock::Vec3 center{target_.x + .5f, target_.y + .5f, target_.z + .5f};
        const double distT = std::hypot(center.x - feet.x, center.z - feet.z);
        if (!hasTarget_ || localPart(blockAt(target_)) != targetId_) {
            if (boatStep_ == BoatStep::Ride) { boatStep_ = BoatStep::Dismount; boatStepTicks_ = 0; }
            else { releaseInputs(); phase_ = Phase::Scan; scanCursor_ = 0; bestDist2_ = 1e18; }
            if (boatStep_ != BoatStep::Dismount) return;
        }
        if (boatPulse_ > 0 && --boatPulse_ == 0) setUse(false);
        if (boatCd_ > 0) --boatCd_;
        switch (boatStep_) {
        case BoatStep::Deploy: {
            selectSlot(boatSlot());
            const double hl = std::max(0.001, distT);
            const bedrock::Vec3 aim{feet.x + static_cast<float>((center.x - feet.x) / hl * 2.5),
                                    feet.y + .4f,
                                    feet.z + static_cast<float>((center.z - feet.z) / hl * 2.5)};
            faceToward(eye, aim, 4.f);
            pressKey(kKeySpace, true); // stay at the surface while placing
            if (auto boat = findBoat(feet, 5.0, 0, true)) {
                boatEid_ = boat->eid;
                note("boat: placed (actor type=" + std::to_string(boat->type) + ")");
                boatStep_ = BoatStep::Board; boatStepTicks_ = 0;
                break;
            }
            if (boatCd_ == 0 && boatStepTicks_ > 10) { setUse(true); boatPulse_ = 3; boatCd_ = 14; }
            if (boatStepTicks_ > 20 * 8) giveUpBoat();
            break;
        }
        case BoatStep::Board: {
            auto boat = findBoat(feet, 6.0, boatEid_);
            if (!boat) { if (boatStepTicks_ > 20 * 6) giveUpBoat(); break; }
            boatCenter_ = boat->center;
            const double d = std::hypot(boat->center.x - feet.x, boat->center.z - feet.z);
            // Riding locks the player onto the boat at deck height;
            // swimming alongside keeps the feet well below the centre.
            if (d < 1.3 && feet.y > boat->center.y - .2f) {
                if (++boatLockTicks_ >= 6) {
                    boatStep_ = BoatStep::Ride; boatStepTicks_ = 0;
                    pressKey(kKeyW, false); pressKey(kKeySpace, false);
                    note("boat: boarded");
                    break;
                }
            } else boatLockTicks_ = 0;
            // Glue probe: live diagnostics showed the player parked at
            // d=0.05 with feet exactly at the hull's AABB bottom for the
            // whole Board window — a signature that fits RIDING (the
            // ride system glues the rider's hitbox into the hull) just
            // as well as a swimmer wedged inside it, and no AABB test
            // separates the two. Behaviour does: hold position glued
            // for 12 ticks, then row at the target for 2s. A ridden
            // boat carries its passenger several blocks; a pushed hull
            // barely moves. One failed probe ends the attempt.
            if (boatProbeTicks_ > 0) {
                faceAim(eye, {center.x, eye.y, center.z}, 6.f, .3f);
                pressKey(kKeyW, true); pressKey(kKeySpace, false);
                if (d > 1.6) { // separated without riding: resume Board
                    boatProbeTicks_ = 0; boatGlueTicks_ = 0;
                    pressKey(kKeyW, false);
                } else if (++boatProbeTicks_ > 40) {
                    const double movedB = std::hypot(boat->center.x - boatProbeStart_.x,
                                                     boat->center.z - boatProbeStart_.z);
                    pressKey(kKeyW, false);
                    if (movedB > 2.5) {
                        boatStep_ = BoatStep::Ride; boatStepTicks_ = 0;
                        note("boat: boarded (probe: hull carried player " +
                             std::to_string(movedB) + " blocks)");
                        break;
                    }
                    note("boat: probe failed (hull moved " + std::to_string(movedB) +
                         "); not riding");
                    giveUpBoat();
                }
                break;
            }
            if (d < 0.8) {
                pressKey(kKeyW, false); pressKey(kKeyS, false); pressKey(kKeySpace, true);
                if (++boatGlueTicks_ >= 12) {
                    boatProbeTicks_ = 1; boatProbeStart_ = boat->center;
                    note("boat: glued to hull; probing ride state");
                }
                break;
            }
            boatGlueTicks_ = 0;
            // Aim at the gunwale above the waterline, and keep the eye
            // OUTSIDE the hull (reach 1.5-3.0): the game's entity pick
            // ray does not register a boat whose AABB already contains
            // the camera — a swimmer who drifts into the hull (d=0.00,
            // verified live) can pulse use forever without boarding.
            // Boarding is only attempted with the head above water:
            // submerged, the downward aim drags the swimmer under the
            // hull instead (also verified live).
            faceAim(eye, {boat->center.x, boat->center.y + .35f, boat->center.z}, 4.f, .3f);
            const double reach = distToMob(eye, *boat);
            const bool submerged = headInWater(feet);
            if (submerged) { pressKey(kKeyW, false); pressKey(kKeyS, false); pressKey(kKeySpace, true); }
            else if (reach > 3.0) { pressKey(kKeyS, false); pressKey(kKeyW, true); pressKey(kKeySpace, true); }
            else if (reach < 1.5) { pressKey(kKeyW, false); pressKey(kKeyS, true); pressKey(kKeySpace, true); }
            else {
                pressKey(kKeyW, false); pressKey(kKeyS, false); pressKey(kKeySpace, true);
                if (boatCd_ == 0) { setUse(true); boatPulse_ = 3; boatCd_ = 10; }
            }
            if (boatStepTicks_ > 20 * 20) giveUpBoat();
            break;
        }
        case BoatStep::Ride: {
            auto boat = findBoat(feet, 4.0, boatEid_);
            if (!boat) { boatStep_ = BoatStep::Board; boatStepTicks_ = 0; pressKey(kKeyW, false); break; }
            boatCenter_ = boat->center;
            if (distT <= 4.5) {
                boatStep_ = BoatStep::Dismount; boatStepTicks_ = 0;
                pressKey(kKeyW, false);
                break;
            }
            faceAim(eye, {center.x, eye.y, center.z}, 6.f, .3f);
            pressKey(kKeyW, true);
            if (boatTicks_ > 20 * 75) { boatStep_ = BoatStep::Dismount; boatStepTicks_ = 0; note("boat: ride timeout"); }
            break;
        }
        case BoatStep::Dismount: {
            pressKey(kKeyW, false);
            pressKey(kKeySneak, true);
            if (boatStepTicks_ > 6) {
                pressKey(kKeySneak, false);
                phase_ = Phase::Navigate; pathFor_ = {INT_MIN, 0, 0}; navTicks_ = 0;
                wasInWater_ = false;
                note("boat: dismounted");
            }
            break;
        }
        }
    }

    // -- state machine -----------------------------------------------------------
    enum class Phase { Idle, Scan, Navigate, Mine, Collect, NeedTool, Escape, Combat, Boat, Craft, Descend, Hunt, Smelt, Ascend, Forage, Recover };
    enum class ForageStage { Hunt, Cook, Eat };
    void resetRun() {
        releaseInputs();
        phase_ = Phase::Idle;
        targetId_.clear();
        blacklist_.clear();
        hazardZones_.clear();
        path_.clear(); pathDig_.clear(); pathBridge_.clear(); pathIdx_ = 0;
        pathFor_ = {INT_MIN, 0, 0}; replans_ = 0;
        swimSubmerged_ = 0; surfacing_ = false; wasInWater_ = false;
        combatEid_ = 0; bowDraw_ = 0; attackPulse_ = 0;
        boatTriedFor_ = {INT_MIN, 0, 0};
        chainActive_ = false; chainJobStarted_ = false; chainWaitingCobble_ = false;
        chainForFurnace_ = false;
        wantPickaxeChain_ = false; chainSteps_.clear(); chainStep_ = 0;
        chainRetries_ = 0; chainNextRetryTick_ = 0;
        expStage_ = ExpStage::None;
        gatherActive_ = false; gatherDone_ = false; gatherFailed_ = false;
        scanIds_.clear(); scanOverrideHeld_ = false;
        huntLegDone_ = false; huntBlocked_ = false;
        descendDone_ = false; descendFailed_ = false; descendCave_ = false;
        cobbleNavFails_ = 0; cobbleDescend_ = false;
        moveStage_ = MoveStage::None; moveFailed_ = false;
        smeltDone_ = false; smeltFailed_ = false;
        forageActive_ = false; forageEid_ = 0;
        navReturn_ = false; chainReturnTrips_ = 0;
        savedId_.clear();
        hasTarget_ = false;
        mined_ = 0;
        goalCount_ = 0; goalBase_ = 0; goalProgress_ = 0; goalItem_.clear();
        needTier_ = 0;
        aimSmoothInit_ = false;
    }
    void startMining(std::string arg, int goal = 0) {
        sehStopped_ = false;
        resetRun();
        targetId_ = localPart(arg);
        if (targetId_.empty() || isAir(targetId_)) { reply("ブロック名を指定してね（例: /mine iron_ore）"); return; }
        phase_ = Phase::Scan;
        scanCursor_ = 0;
        bestDist2_ = 1e18;
        goalCount_ = goal;
        goalItem_ = dropOf(targetId_);
        goalBase_ = goal > 0 ? invCountOf(goalItem_) : 0;
        if (goal > 0)
            reply(targetId_ + " を掘って " + goalItem_ + " を " + std::to_string(goal) +
                  " 個集めるよ（半径" + std::to_string(radius_) + "）");
        else
            reply(targetId_ + " を探して掘るよ（半径" + std::to_string(radius_) + "）");
        note("mine start: " + targetId_ + (goal > 0 ? " goal=" + std::to_string(goal) +
             "x" + goalItem_ + " base=" + std::to_string(goalBase_) : ""));
    }
    // Count of an item across the whole inventory (adapter snapshot when
    // crafting is ready, else the hotbar).
    int invCountOf(const std::string& item) {
        // Exact id match: a substring count would call diamond_ore or a
        // diamond pickaxe "diamond" and fake goal/material counts.
        const auto have = craftReady_ ? chainCounts() : visibleItems();
        const auto it = have.find(item);
        return it == have.end() ? 0 : it->second;
    }
    bool goalReached() {
        if (goalCount_ <= 0) return false;
        // Always judge by the inventory delta. The old self-drop
        // shortcut (goal item == target block, e.g. /mine stone N)
        // compared against mined_, which counts EVERY block broken in
        // the run — after an expedition gathered 25 logs, /mine stone 3
        // "completed" instantly without mining any stone (found
        // 2026-11-17 during the 1.26.65.1 verification).
        return invCountOf(goalItem_) - goalBase_ >= goalCount_;
    }
    void finishGoal() {
        reply("目標達成！ " + goalItem_ + " を " + std::to_string(goalCount_) + " 個集めたよ（掘った数: " +
              std::to_string(mined_) + "）");
        note("goal reached: " + std::to_string(goalCount_) + "x" + goalItem_);
        releaseInputs();
        phase_ = Phase::Idle; hasTarget_ = false; goalCount_ = 0;
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
        const auto sp = rest.find(' ');
        const auto arg = lower(rest.substr(0, sp));
        int goal = 0;
        if (sp != std::string::npos) {
            const auto num = rest.substr(sp + 1);
            if (!num.empty() && num.find_first_not_of("0123456789") == std::string::npos)
                goal = std::clamp(std::atoi(num.c_str()), 1, 999);
        }
        if (arg.empty() || arg == "status") {
            reply(statusText());
        } else if (arg == "path") {
            showPath_ = !showPath_;
            reply(showPath_ ? "経路表示を ON にしたよ" : "経路表示を OFF にしたよ");
        } else if (arg == "stop") {
            stopMining("");
        } else if (arg == "food") {
            startForage(goal > 0 ? goal : 3);
        } else if (arg == "mobs") {
            probePending_ = true; // actor reads must run on the tick thread
            reply("probe: queued");
        } else if (arg == "crashtest") {
            note("crashtest: deliberate fault queued (crash reporter self-test)");
            crashTestPending_ = true;
        } else if (enabled_) {
            startMining(arg, goal);
        } else {
            reply("設定で無効になっているよ");
        }
    }
    bool loadActors(const bedrock::RuntimeProfile& profile, std::string& failure) {
        if (!bedrock::parseNativeActorLayout(profile.json(), actorLayout_, failure))
            return false;
        if (!runtimeActors_.load(profile.json(), failure)) return false;
        try {
            const auto root = bedrock::parseNativeProfile(profile.json());
            const auto& at = root.at("layouts").at("Actor").at("actor_type");
            actorTypeOffset_ = bedrock::native_profile::number(at.at("value_offset"));
            actorRegistry_.configure(
                actorLayout_, actorTypeOffset_,
                static_cast<std::uint32_t>(bedrock::native_profile::number(at.at("player_type_id"))));
        } catch (const std::exception& e) {
            failure = e.what();
            return false;
        }
        return true;
    }
    // -- verified screen state -------------------------------------------
    // Item moves used to dispatch transfers on a blind timer after the
    // E pulse; on the real client the inventory screen is not always
    // attached by then (or a different screen is up), and every
    // transfer comes back status 49 (user's run 2026-11-17: a
    // stone-pickaxe hotbar move failed 3x in a row underground). The
    // top screen NAME is ground truth — read it and only move items
    // while the inventory screen is verifiably on top.
    using GetTopScreenName = void*(__fastcall*)(void*, void*);
    GetTopScreenName topScreenFn_{};
    void* memDeallocate_{};
    bool screenReady_{};
    void loadScreen(const bedrock::RuntimeProfile& profile) {
        try {
            const auto root = bedrock::parseNativeProfile(profile.json());
            topScreenFn_ = reinterpret_cast<GetTopScreenName>(bedrock::native_profile::checked(
                root, nlohmann::json{{"symbol", "ClientInstance::getTopScreenName"}}));
            memDeallocate_ = bedrock::native_profile::checked(
                root, nlohmann::json{{"symbol", "Bedrock::Memory::deallocate"}});
            screenReady_ = true;
        } catch (...) {
            screenReady_ = false;
        }
    }
    // The game writes an MSVC std::string into the out buffer; names
    // longer than 15 chars allocate from the GAME's allocator, so the
    // buffer is released through the game's own deallocate — never the
    // mod CRT (the cross-allocator heap-corruption lesson).
    std::string topScreenName() {
        if (!screenReady_ || !client_.load()) return {};
        alignas(8) std::byte buf[32]{};
        __try {
            topScreenFn_(client_.load(), buf);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return {};
        }
        std::uint64_t size = 0, cap = 0;
        std::memcpy(&size, buf + 16, sizeof(size));
        std::memcpy(&cap, buf + 24, sizeof(cap));
        const char* data = reinterpret_cast<const char*>(buf);
        void* heap = nullptr;
        if (cap > 15) {
            std::memcpy(&heap, buf, sizeof(heap));
            data = static_cast<const char*>(heap);
        }
        std::string out;
        if (data && size <= 256) out.assign(data, static_cast<std::size_t>(size));
        if (heap && memDeallocate_ && cap < 0x800)
            reinterpret_cast<void(__fastcall*)(void*)>(memDeallocate_)(heap);
        return out;
    }
    bool inventoryScreenUp() {
        if (!screenReady_) return false;
        return contains(topScreenName(), "inventory");
    }
    // Ground truth for "some UI screen is on top": the gameplay-
    // active EVENT can stick in the inactive state after a screen
    // closes (measured 2026-11-17: after a craft chain the event
    // never flipped back, and the tick-level screen recovery kept
    // Esc-pulsing every 30 ticks — toggling the pause menu forever
    // while the mod aimed at blocks it could never break). The top
    // screen name does not lie; trust it over the event whenever it
    // is readable.
    bool anyScreenUp() {
        if (screenReady_) {
            const std::string screen = topScreenName();
            return !screen.empty() && screen != "hud_screen";
        }
        return !game_.gameplayActive();
    }
    std::optional<std::uint32_t> actorTypeOf(void* actor) {
        void* comp = actorRegistry_.component(actor, actorLayout_.actorTypeComponentHash, 0);
        if (!comp) return std::nullopt;
        std::uint32_t v = 0;
        __try {
            std::memcpy(&v, static_cast<const char*>(comp) + actorTypeOffset_, sizeof(v));
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return std::nullopt;
        }
        return v;
    }
    // Debug aid (/mine mobs): dump everything readable about nearby
    // actors — this is how the combat mob type ids were identified.
    void probeActors(const bedrock::Vec3& feet) {
        if (!actorsReady_) { note("probe: actor access not ready"); return; }
        const auto snap = runtimeActors_.snapshot(level_.load());
        if (!snap) { note("probe: snapshot failed"); return; }
        note("probe: raw actors=" + std::to_string(snap->size()));
        void* self = world_.localPlayer(client_.load());
        int n = 0;
        for (void* a : *snap) {
            bedrock::Vec3 pos;
            if (!actorRegistry_.readPosition(a, pos)) continue;
            const double d = std::hypot(pos.x - feet.x, pos.z - feet.z);
            if (d > 40) continue;
            ++n;
            bedrock::ActorBounds b{};
            const bool hasB = actorRegistry_.readBounds(a, b);
            std::uint32_t eid = 0;
            actorRegistry_.readEntityId(a, eid);
            const auto type = actorTypeOf(a);
            const bedrock::Actor apiActor{game_, a};
            const auto ident = apiActor.getIdentity();
            char buf[640];
            std::snprintf(buf, sizeof(buf),
                "actor d=%.1f self=%d type=%d eid=%u bounds=%.2fx%.2f pos=%.1f,%.1f,%.1f name='%s' tag='%s' player=%d",
                d, a == self ? 1 : 0, type ? static_cast<int>(*type) : -1, eid,
                hasB ? (b.maximum.x - b.minimum.x) : -1.f,
                hasB ? (b.maximum.y - b.minimum.y) : -1.f,
                pos.x, pos.y, pos.z,
                ident.name.c_str(), ident.nameTag.c_str(), ident.isPlayer ? 1 : 0);
            note(buf);
        }
        reply("probe: " + std::to_string(n) + " actors logged");
    }
    std::string statusText() const {
        switch (phase_) {
        case Phase::Idle: return "待機中。/mine <ブロック名> で開始";
        case Phase::Scan: return targetId_ + " を探索中…（掘った数: " + std::to_string(mined_) + "）" +
            (goalCount_ > 0 ? " 目標 " + goalItem_ + " " + std::to_string(goalProgress_) +
             "/" + std::to_string(goalCount_) : "");
        case Phase::Navigate: return targetId_ + " へ移動中（掘った数: " + std::to_string(mined_) + "）";
        case Phase::Mine: return targetId_ + " を採掘中（掘った数: " + std::to_string(mined_) + "）";
        case Phase::Collect: return "ドロップ回収中（掘った数: " + std::to_string(mined_) + "）";
        case Phase::NeedTool: return "ツール待ち（掘った数: " + std::to_string(mined_) + "）";
        case Phase::Escape: return "水から浮上中…（掘った数: " + std::to_string(mined_) + "）";
        case Phase::Combat: return "敵と戦闘中！（掘った数: " + std::to_string(mined_) + "）";
        case Phase::Boat: return "ボートで移動中（掘った数: " + std::to_string(mined_) + "）";
        case Phase::Craft: return "クラフト中（掘った数: " + std::to_string(mined_) + "）";
        case Phase::Ascend: return "地上へ帰還中（掘った数: " + std::to_string(mined_) + "）";
        case Phase::Forage: return "食料調達中（狩った数: " + std::to_string(forageKills_) + "）";
        case Phase::Recover: return "死亡した場所へ荷物を回収に戻り中";
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

    // -- SEH guard rails ------------------------------------------------------
    // The guarded entry points are split so no function mixes __try with
    // C++ objects or C++ try/catch (C2712): the SEH layer calls an inner
    // function that keeps the ordinary C++ error handling.
    void tickGuarded() {
        if (sehStopped_) return;
        __try { tick(); }
        __except (EXCEPTION_EXECUTE_HANDLER) { onSehFault("tick"); }
    }
    void onSehFault(const char* where) {
        InterlockedIncrement(&crashrec::g_sehFaults);
        note(std::string("SEH fault caught in ") + where + " (section: " +
             crashrec::g_section + "); stopping the run for safety");
        try { releaseInputs(); } catch (...) {}
        phase_ = Phase::Idle;
        sehStopped_ = true;
    }
    void craftUpdateInner(void* level) {
        try { craft_.update(client_.load(), level); }
        catch (...) { note("craft adapter update failed"); }
    }
    void craftUpdateGuarded(void* level) {
        __try { craftUpdateInner(level); }
        __except (EXCEPTION_EXECUTE_HANDLER) {
            InterlockedIncrement(&crashrec::g_sehFaults);
            note("craft adapter update faulted (SEH); disabling craft");
            craftReady_ = false;
        }
    }
    void drawPathInner(void* raw) { try { drawPath(raw); } catch (...) {} }
    void drawPathGuarded(void* raw) {
        __try { drawPathInner(raw); }
        __except (EXCEPTION_EXECUTE_HANDLER) {
            InterlockedIncrement(&crashrec::g_sehFaults);
            outlineReady_ = false;
        }
    }
    void markSection() {
        crashrec::g_ticks = static_cast<LONG>(tickCount_);
        switch (phase_) {
        case Phase::Idle: crashrec::setSection("tick/Idle"); break;
        case Phase::Scan: crashrec::setSection("tick/Scan"); break;
        case Phase::Navigate: crashrec::setSection("tick/Navigate"); break;
        case Phase::Mine: crashrec::setSection("tick/Mine"); break;
        case Phase::Collect: crashrec::setSection("tick/Collect"); break;
        case Phase::NeedTool: crashrec::setSection("tick/NeedTool"); break;
        case Phase::Escape: crashrec::setSection("tick/Escape"); break;
        case Phase::Combat: crashrec::setSection("tick/Combat"); break;
        case Phase::Boat: crashrec::setSection("tick/Boat"); break;
        case Phase::Craft: crashrec::setSection("tick/Craft"); break;
        case Phase::Descend: crashrec::setSection("tick/Descend"); break;
        case Phase::Hunt: crashrec::setSection("tick/Hunt"); break;
        case Phase::Smelt: crashrec::setSection("tick/Smelt"); break;
        case Phase::Ascend: crashrec::setSection("tick/Ascend"); break;
        case Phase::Forage: crashrec::setSection("tick/Forage"); break;
        case Phase::Recover: crashrec::setSection("tick/Recover"); break;
        }
    }

    void tick() {
        ++tickCount_;
        if ((tickCount_ % 200) == 0) // expire combat give-up entries
            for (auto it = combatIgnore_.begin(); it != combatIgnore_.end();)
                if (it->second <= tickCount_) it = combatIgnore_.erase(it); else ++it;
        if (pendingReset_) { pendingReset_ = false; gui_.reset(epoch_.load()); camera_.reset(); resetRun(); joinGuardTicks_ = 20 * 60; joinGuardNoted_ = false; joinDryTicks_ = 0; joinSurfaced_ = false; if (recoverActive_) phase_ = Phase::Recover; }
        tickDeathWatch();
        if (recoverActive_ && !deathPending_ &&
            phase_ != Phase::Recover && phase_ != Phase::Mine && phase_ != Phase::Collect)
            phase_ = Phase::Recover; // the corpse run owns the client until done
        {
            std::deque<std::string> native;
            { std::lock_guard lock(nativeMutex_); native.swap(nativeLines_); }
            for (auto& line : native) dispatchLine(std::move(line));
        }
        if (probePending_) {
            probePending_ = false;
            bedrock::Vec3 pf;
            if (feetPos(pf)) probeActors(pf);
            else note("probe: no feet");
        }
        if (phase_ == Phase::Idle &&
            (descendDone_ || huntLegDone_ || smeltDone_ || smeltFailed_)) {
            // Descend, Hunt and Smelt finish inside the phase switch,
            // parking in Idle with their done flags set for the routing hooks
            // further down — but the Idle early return below used to
            // swallow every later tick, so a finished descent stranded
            // the whole run in Idle (measured 2026-11-16: the cobble
            // shaft reached its depth and nothing ever resumed).
            // Route those completions onward here first.
            bedrock::Vec3 pf;
            if (feetPos(pf)) {
                if (cobbleDescend_ && descendDone_) {
                    cobbleDescend_ = false;
                    descendDone_ = false; descendFailed_ = false; descendCave_ = false;
                    phase_ = Phase::Scan; scanCursor_ = 0; bestDist2_ = 1e18;
                    hasTarget_ = false; emptyScans_ = 0;
                    note("craft chain: cobble shaft done, resuming stone scan");
                }
                if (huntLegDone_ && gatherActive_ && phase_ == Phase::Idle) {
                    huntLegDone_ = false;
                    if (huntBlocked_) {
                        huntBlocked_ = false;
                        const int ndx = huntDirZ_, ndz = -huntDirX_;
                        huntDirX_ = ndx; huntDirZ_ = ndz;
                        note("gather: hunt direction rotated");
                    }
                    phase_ = Phase::Scan; scanCursor_ = 0; bestDist2_ = 1e18;
                    hasTarget_ = false; emptyScans_ = 0;
                }
                if (phase_ == Phase::Idle) {
                    if (forageActive_) tickForage(pf);
                    else tickExpedition(pf);
                }
            }
        }
        if (phase_ == Phase::Idle) {
            bedrock::Vec3 pf;
            if (feetPos(pf) && tickIdleGuards(pf)) return;
            clearPath(); return;
        }
        if (anyScreenUp() && phase_ != Phase::Craft && phase_ != Phase::Smelt) {
            if (moveStage_ != MoveStage::None) {
                // A hotbar move is mid-flight (it opened this screen
                // itself): keep polling it instead of closing the
                // screen out from under it.
                (void)ensureHotbarTick(moveItem_);
                return;
            }
            // A screen left open by an aborted chain freezes every
            // other phase (they all expect gameplay input): pulse Esc
            // until it closes instead of waiting forever (measured
            // 2026-11-16: a failed pickaxe chain left the inventory
            // open in NeedTool and the retry timer could never run).
            // Craft and Smelt manage their own screens and are exempt.
            releaseInputs();
            if ((tickCount_ % 30) == 1) pressKey(0x1B, true);
            if ((tickCount_ % 30) == 3) pressKey(0x1B, false);
            return;
        }
        bedrock::Vec3 feet;
        if (!feetPos(feet)) { releaseInputs(); return; }
        // Drowning safety net: in the stationary working phases, if the
        // head stays submerged for a few ticks, abandon the target and
        // swim up. Target selection already avoids water, but the
        // standpoint next to a dry-column target can still be below the
        // waterline (e.g. digging into a shore face), and waiting there
        // in NeedTool is what actually drowned the player during
        // verification. Navigate is excluded: crossing water by swimming
        // is intentional there and has its own air management.
        if (phase_ == Phase::Escape) { tickEscape(feet); updatePath(feet); return; }
        if (phase_ == Phase::Mine || phase_ == Phase::Collect || phase_ == Phase::NeedTool ||
            phase_ == Phase::Combat || phase_ == Phase::Forage) {
            if (headInWater(feet)) {
                if (++submergedTicks_ >= 3) { startEscape(); tickEscape(feet); updatePath(feet); return; }
            } else submergedTicks_ = 0;
        }
        // Hostile interruption: a mob that gets close during work preempts
        // the run — fight first, then resume (exitCombat restores the run).
        if (game_.gameplayActive() &&
            (phase_ == Phase::Scan || phase_ == Phase::Navigate || phase_ == Phase::Mine ||
             phase_ == Phase::Collect || phase_ == Phase::NeedTool || phase_ == Phase::Craft)) {
            if ((++threatScanTick_ % 4) == 0) {
                // Relevance: within 12 blocks and near the player's level,
                // or point-blank at any height. A drowned in the sea far
                // below a platform is not a threat to a miner on top of it.
                if (auto mob = scanHostile(feet, 12.0)) {
                    const double dy = std::abs(mob->center.y - feet.y);
                    if (dy <= 4.0 || mob->dist <= 6.0) { enterCombat(*mob); tickCombat(feet); updatePath(feet); return; }
                }
            }
        }
        if (chainWaitingCobble_ && phase_ == Phase::Scan) {
            if (countAllItems("cobblestone") >= 3) {
                // The mining that gathered the cobble may have wandered
                // far from the placed table (seabed stone, this run): the
                // table grid only answers while its screen is open, and
                // the screen only opens within reach — walk back first.
                const double tdx = chainTablePos_.x + .5 - feet.x;
                const double tdy = chainTablePos_.y + .5 - feet.y;
                const double tdz = chainTablePos_.z + .5 - feet.z;
                const double tableDist = std::sqrt(tdx * tdx + tdy * tdy + tdz * tdz);
                if (!chainTablePlaced_ && countAllItems("crafting_table") >= 1) {
                    // The placed table is gone but the player carries one
                    // (crafted earlier and never placed, or the drop from
                    // the broken one): put it down right here first.
                    chainSteps_.insert(chainSteps_.begin() + chainStep_,
                                        ChainStep{ChainStep::Kind::PlaceTable});
                    chainWaitingCobble_ = false;
                    phase_ = Phase::Craft; chainStepTicks_ = 0; chainAimedTick_ = -1;
                    note("craft chain: placing carried table for the stone pickaxe");
                } else if (!chainTablePlaced_) {
                    chainFail("作業台が無い");
                } else if (tableDist <= 4.0) {
                    chainWaitingCobble_ = false;
                    phase_ = Phase::Craft; chainStepTicks_ = 0; chainAimedTick_ = -1;
                    note("craft chain: cobblestone ready, resuming");
                } else if (countAllItems("crafting_table") >= 1) {
                    // Far from the placed table but carrying a spare: put
                    // it down here instead of walking back across mined-
                    // out terrain (the return walk kept timing out around
                    // the holes that mining leaves behind).
                    chainSteps_.insert(chainSteps_.begin() + chainStep_,
                                        ChainStep{ChainStep::Kind::PlaceTable});
                    chainWaitingCobble_ = false;
                    phase_ = Phase::Craft; chainStepTicks_ = 0; chainAimedTick_ = -1;
                    note("craft chain: placing carried table for the stone pickaxe");
                } else if (countAllItems("planks") >= 4) {
                    // No table at hand, but enough planks to make one on
                    // the spot (personal grid) and place it right here.
                    chainSteps_.insert(chainSteps_.begin() + chainStep_,
                                       {ChainStep{ChainStep::Kind::OpenInventory},
                                        ChainStep{ChainStep::Kind::Craft,
                                                  "minecraft:WorkBench_recipeId",
                                                  "minecraft:crafting_table", 1, false},
                                        ChainStep{ChainStep::Kind::CloseUi},
                                        ChainStep{ChainStep::Kind::PlaceTable}});
                    chainWaitingCobble_ = false;
                    phase_ = Phase::Craft; chainStepTicks_ = 0; chainAimedTick_ = -1;
                    note("craft chain: crafting a fresh table on the spot");
                } else if (countAllItems("_log") >= 1 || countAllItems("_wood") >= 1) {
                    // Not enough planks for a table, but logs convert
                    // 1 -> 4 planks on the personal grid: make the
                    // planks and the table on the spot instead of
                    // walking back (measured 2026-11-17 on the user's
                    // run: the return from the cobble shaft was
                    // A*-impossible, three 45 s trips were burned, and
                    // the stone pickaxe was abandoned while logs sat
                    // unused in the inventory).
                    int logCount = 0;
                    const std::string wood = chainWood(chainCounts(), logCount);
                    const int deficit = 4 - countAllItems("planks");
                    if (!wood.empty() && deficit > 0) {
                        chainSteps_.insert(chainSteps_.begin() + chainStep_,
                                           {ChainStep{ChainStep::Kind::OpenInventory},
                                            ChainStep{ChainStep::Kind::Craft,
                                                      "minecraft:" + wood + "_planks",
                                                      "minecraft:" + wood + "_planks",
                                                      (deficit + 3) / 4, false},
                                            ChainStep{ChainStep::Kind::Craft,
                                                      "minecraft:WorkBench_recipeId",
                                                      "minecraft:crafting_table", 1, false},
                                            ChainStep{ChainStep::Kind::CloseUi},
                                            ChainStep{ChainStep::Kind::PlaceTable}});
                        chainWaitingCobble_ = false;
                        phase_ = Phase::Craft; chainStepTicks_ = 0; chainAimedTick_ = -1;
                        note("craft chain: crafting planks and a table on the spot");
                    } else if (++chainReturnTrips_ <= 3) {
                        target_ = chainTablePos_; hasTarget_ = true; navReturn_ = true;
                        path_.clear(); pathFor_ = {INT_MIN, 0, 0};
                        phase_ = Phase::Navigate;
                        note("craft chain: returning to the crafting table");
                    } else {
                        chainWaitingCobble_ = false;
                        chainActive_ = false;
                        reply("作業台に戻れないから、石のツルハシは諦めるね");
                        note("craft chain: could not return to the table");
                    }
                } else if (++chainReturnTrips_ <= 3) {
                    target_ = chainTablePos_; hasTarget_ = true; navReturn_ = true;
                    path_.clear(); pathFor_ = {INT_MIN, 0, 0};
                    phase_ = Phase::Navigate;
                    note("craft chain: returning to the crafting table");
                } else {
                    chainWaitingCobble_ = false;
                    chainActive_ = false;
                    reply("作業台に戻れないから、石のツルハシは諦めるね");
                    note("craft chain: could not return to the table");
                }
            } else if (cobbleNavFails_ >= 2) {
                // Surface stone keeps failing to path (swamp pools,
                // stone buried under dirt): dig straight down to the
                // stone layer instead — down there stone is adjacent
                // in every direction and navigation is trivial.
                cobbleNavFails_ = 0;
                descendTargetY_ = static_cast<int>(std::floor(feet.y)) - 10;
                descendDone_ = false; descendFailed_ = false; descendCave_ = false;
                descendLastY_ = INT_MAX; descendStuck_ = 0;
                descendWide_ = false; descendCenterTicks_ = 0;
                descendEntryY_ = static_cast<int>(std::floor(feet.y));
                cobbleDescend_ = true;
                releaseInputs();
                phase_ = Phase::Descend;
                note("craft chain: cobble targets unreachable, digging down to stone");
            } else if (++chainCobbleTicks_ > 20 * 180) {
                chainWaitingCobble_ = false;
                chainActive_ = false;
                reply("丸石が集まらないから、石のツルハシは諦めるね");
                note("craft chain: cobblestone wait timed out");
            }
        }
        if (goalCount_ > 0 && phase_ != Phase::Idle && tickCount_ % 20 == 0) {
            goalProgress_ = goalItem_ == targetId_ ? mined_
                            : invCountOf(goalItem_) - goalBase_;
            if (goalReached()) { finishGoal(); return; }
        }
        // Scan override housekeeping: an override (the chain's cobble
        // wait, an expedition gather) owns targetId_ while it runs; give
        // the original target back once nothing claims the override.
        if (scanOverrideHeld_ && !chainWaitingCobble_ && !gatherActive_)
            endScanOverride();
        // Expedition gather completion / timeout.
        if (gatherActive_) {
            int total = 0;
            for (const auto& id : gatherCountIds_) total += countAllItems(id);
            if (total >= gatherWant_) {
                gatherActive_ = false; gatherDone_ = true;
                releaseInputs(); phase_ = Phase::Idle; hasTarget_ = false;
                note("gather: done (" + std::to_string(total) + " collected)");
            } else if (tickCount_ > gatherDeadline_) {
                gatherActive_ = false; gatherFailed_ = true;
                releaseInputs(); phase_ = Phase::Idle; hasTarget_ = false;
                note("gather: timed out");
            } else if (gatherHunt_ && phase_ == Phase::Scan &&
                       (emptyScans_ >= 3 || escapeStreak_ >= 3) &&
                       !chainActive_) {
                // Nothing in scan range: tunnel a leg and rescan. The
                // escape-streak arm covers the flooded-cavern loop
                // (measured 2026-11-18): scans kept FINDING ore across
                // a lake, every approach submerged the head, and each
                // escape only blacklisted that one vein cell — the bot
                // bobbed in place forever. Three escapes with no block
                // mined in between mean this patch of water is a trap:
                // relocate like an empty scan does.
                if (escapeStreak_ >= 3)
                    note("gather: escape loop (x" + std::to_string(escapeStreak_) +
                         "), relocating");
                escapeStreak_ = 0; emptyScans_ = 0;
                huntStage_ = 0; huntCells_ = 0; huntLegDone_ = false; digPos_ = {INT_MIN, 0, 0};
                phase_ = Phase::Hunt;
                note("gather: hunt leg (" + std::to_string(huntDirX_) + "," +
                     std::to_string(huntDirZ_) + ")");
            }
        }
        // Scan stall: nothing of the target in range and the ordinary
        // flow cannot fix it (no target ever reaches Navigate/Mine, so
        // NeedTool never fires) — the expedition takes over from here.
        if (phase_ == Phase::Scan && emptyScans_ >= 6 && !chainActive_ &&
            !gatherActive_) {
            if (!expeditionActive()) (void)maybeStartExpedition();
            if (!expeditionActive() && goalCount_ > 0 &&
                requiredTier(targetId_) >= 4 && hasPickaxeTier(requiredTier(targetId_))) {
                needTier_ = requiredTier(targetId_);
                // Already at diamond depth with the right pickaxe
                // (e.g. a re-issued /mine after a client crash):
                // skip the descent and hunt directly — without
                // this, the plain scan loop rescans forever when
                // no ore is within radius (measured 2026-11-17 on
                // fresh2 after a crash at y=-44).
                startExpedition(feet.y > -40 ? ExpStage::ToDiamondLevel
                                             : ExpStage::HuntOre);
            }
        }
        if (huntLegDone_ && gatherActive_ && phase_ == Phase::Idle) {
            huntLegDone_ = false;
            if (huntBlocked_) {
                // Rotate 90 degrees: the old heading is a cave edge, a
                // lava pocket or a wall the advance could not pass.
                huntBlocked_ = false;
                const int ndx = huntDirZ_, ndz = -huntDirX_;
                huntDirX_ = ndx; huntDirZ_ = ndz;
                note("gather: hunt direction rotated");
            }
            phase_ = Phase::Scan; scanCursor_ = 0; bestDist2_ = 1e18;
            hasTarget_ = false; emptyScans_ = 0;
        }
        if (cobbleDescend_ && descendDone_) {
            // The cobble-wait shaft is done (reached depth, cave mouth
            // or a refusal): hand control back to the cobble scan,
            // which now runs surrounded by stone.
            cobbleDescend_ = false;
            descendDone_ = false; descendFailed_ = false; descendCave_ = false;
            phase_ = Phase::Scan; scanCursor_ = 0; bestDist2_ = 1e18;
            hasTarget_ = false; emptyScans_ = 0;
            note("craft chain: cobble shaft done, resuming stone scan");
        }
        tickExpedition(feet);
        if (forageActive_ && (phase_ == Phase::Forage || phase_ == Phase::Scan))
            tickForage(feet);
        markSection();
        // Sprint is a Navigate-only state; every other phase clears
        // it every tick (pressKey dedups, so this is free). Forage
        // chases sprint too.
        if (phase_ != Phase::Navigate && phase_ != Phase::Forage)
            pressKey(kKeySprint, false);
        switch (phase_) {
        case Phase::Scan: tickScan(feet); break;
        case Phase::Navigate: tickNavigate(feet); break;
        case Phase::Mine: tickMine(feet); break;
        case Phase::Collect: tickCollect(feet); break;
        case Phase::NeedTool: tickNeedTool(feet); break;
        case Phase::Ascend: tickAscend(feet); break;
        case Phase::Combat: tickCombat(feet); break;
        case Phase::Boat: tickBoat(feet); break;
        case Phase::Craft: tickCraft(feet); break;
        case Phase::Descend: tickDescend(feet); break;
        case Phase::Hunt: tickHunt(feet); break;
        case Phase::Smelt: tickSmelt(feet); break;
        case Phase::Forage: break; // driven beside tickExpedition above
        case Phase::Recover: tickRecover(feet); break;
        case Phase::Idle: case Phase::Escape: break;
        }
        updatePath(feet);
    }

    // Death watch + corpse recovery (2026-11-18, user request).
    // Death has no direct readout, but its signature is unambiguous
    // (measured live with a /kill probe): while alive the top screen
    // is 'hud_screen' with gameplay active; on the death screen it is
    // steadily 'ui://./hbui/gameplay.html' with gameplay inactive.
    // The self actor stays in the snapshot either way. On death:
    // remember the last alive position, abandon the run (the
    // inventory is gone with it), pulse Enter — the death screen's
    // respawn button answers Enter, verified live — and after the
    // respawn walk back to the death point to pick the drops up
    // (this world drops everything on death; despawn is 5 minutes).
    void tickDeathWatch() {
        const std::string screen = topScreenName();
        if (contains(screen, "world_loading")) joinArmPending_ = true;
        const bool aliveSig = screen == "hud_screen" && game_.gameplayActive();
        if ((screen == "hud_screen" || screen == "hud_crosshair_screen") && joinArmPending_) {
            // Arm the join drowning guard on the first fully-loaded
            // tick. It used to be armed only in the WorldChange
            // reset, which does not fire on a plain server join —
            // the guard never armed at all in the first pond tests.
            joinArmPending_ = false;
            joinGuardTicks_ = 20 * 60; joinGuardNoted_ = false; joinDryTicks_ = 0;
            joinSurfaced_ = false;
        }
        if (aliveSig) {
            deathSigTicks_ = 0;
            bedrock::Vec3 f;
            if (feetPos(f)) { lastAlivePos_ = f; hasLastAlivePos_ = true; }
            if (deathPending_) {
                deathPending_ = false;
                if (hasDeathPos_) startRecovery();
            }
            return;
        }
        const bool deathSig = screen == "ui://./hbui/gameplay.html" && !game_.gameplayActive();
        if (!deathSig || !hasLastAlivePos_) { deathSigTicks_ = 0; return; }
        if (++deathSigTicks_ < 20) return; // 1 s sustained: not a transient
        if (!deathPending_) {
            deathPending_ = true; hasDeathPos_ = true;
            deathPos_ = lastAlivePos_;
            note("death: detected at " + std::to_string(static_cast<int>(deathPos_.x)) + "," +
                 std::to_string(static_cast<int>(deathPos_.y)) + "," +
                 std::to_string(static_cast<int>(deathPos_.z)) + "; respawning");
            reply("死んじゃった…リスポーンして荷物を取りに戻るね");
            resetRun();
        }
        if ((tickCount_ % 30) == 1) pressKey(0x0D, true);
        if ((tickCount_ % 30) == 3) pressKey(0x0D, false);
    }
    void startRecovery() {
        recoverActive_ = true;
        recoverTicks_ = 0; recoverSweepTicks_ = 0; recoverStuckTicks_ = 0;
        recoverLastPos_ = deathPos_;
        phase_ = Phase::Recover;
        // Sentinel goal id: the path follower's dig detours save and
        // restore targetId_, and an EMPTY saved id silently converts
        // the run into mining the dug block forever (measured
        // 2026-11-18: the corpse run became an endless grass_block
        // gather). A non-block id keeps the save/restore intact.
        targetId_ = "corpse_run";
        pathFor_ = {INT_MIN, 0, 0}; path_.clear();
        note("recover: heading to the death point");
        reply("リスポーンしたよ。荷物を取りに戻るね");
    }
    void recoverDone(bool arrived) {
        releaseInputs();
        recoverActive_ = false; hasDeathPos_ = false; hasTarget_ = false;
        phase_ = Phase::Idle;
        note(arrived ? "recover: back at the death point" : "recover: gave up");
        reply(arrived ? "荷物の場所に戻ったよ" : "荷物の回収はできなかった…");
    }
    void tickRecover(const bedrock::Vec3& feet) {
        ++recoverTicks_;
        targetId_ = "corpse_run"; // dig detours may have overwritten it
        if (recoverTicks_ > 20 * 240) { recoverDone(false); return; } // 4 min cap
        const double dx = deathPos_.x - feet.x;
        const double dy = deathPos_.y - feet.y;
        const double dz = deathPos_.z - feet.z;
        const double dist = std::sqrt(dx * dx + dy * dy + dz * dz);
        // No-progress guard: the corpse run has no tools, so a death
        // spot behind solid rock is unreachable — stop early instead
        // of pressing into a wall for the full 4 minutes.
        const double moved = std::hypot(feet.x - recoverLastPos_.x, feet.z - recoverLastPos_.z);
        if (moved > 1.0) { recoverLastPos_ = feet; recoverStuckTicks_ = 0; }
        else if (++recoverStuckTicks_ > 20 * 45 && dist > 6.0) { recoverDone(false); return; }
        if (dist <= 4.5) {
            // Final sweep: step onto the spot and linger so the drops
            // are picked up.
            if (++recoverSweepTicks_ > 100) { recoverDone(true); return; }
            bedrock::Vec3 eye;
            if (eyePos(eye)) faceAim(eye, deathPos_, 12.f, 0.3f);
            pressKey(kKeyW, dist > 0.8);
            pressKey(kKeySpace, inWaterNow(feet));
            return;
        }
        target_ = BlockPos3{static_cast<int>(std::floor(deathPos_.x)),
                            static_cast<int>(std::floor(deathPos_.y)),
                            static_cast<int>(std::floor(deathPos_.z))};
        hasTarget_ = true;
        if (!(pathFor_ == target_)) {
            if (!(planFailFor_ == target_) || tickCount_ - planFailTick_ >= 200) {
                if (!planPath(feet) && path_.empty()) {
                    planFailFor_ = target_; planFailTick_ = tickCount_;
                }
            }
        }
        if (!path_.empty() && pathIdx_ < path_.size() && pathDig_[pathIdx_]) {
            // The follower enters a dig waypoint with no distance
            // gate, and the Mine phase bounces straight back when
            // the cell is out of reach (4.4 from the eye) — the
            // bounce lands back here and the follower re-enters the
            // dig instantly, deadlocking on the spot (measured
            // 2026-11-18: 549 dig entries, 2 completions). Walk up
            // to the cell first, then let the follower dig it.
            const BlockPos3& wp = path_[pathIdx_];
            const bedrock::Vec3 eye{feet.x, feet.y + 1.62f, feet.z};
            const bedrock::Vec3 c{wp.x + .5f, wp.y + .5f, wp.z + .5f};
            const double ddx = c.x - eye.x, ddy = c.y - eye.y, ddz = c.z - eye.z;
            if (ddx * ddx + ddy * ddy + ddz * ddz > 3.2 * 3.2) {
                faceAim(eye, c, 12.f, 0.3f);
                pressKey(kKeyW, true);
                pressKey(kKeySpace, inWaterNow(feet));
                return;
            }
        }
        if (!path_.empty()) { tickFollowPath(feet); return; }
        bedrock::Vec3 eye;
        if (!eyePos(eye)) return;
        faceAim(eye, deathPos_, 12.f, 0.3f);
        pressKey(kKeyW, true);
        pressKey(kKeySpace, inWaterNow(feet) || headInWater(feet));
    }

    // Join drowning guard (2026-11-18): if the player (re)joins with
    // the head underwater — the logout spot was in a flooded cave —
    // the run resets to Idle and nothing else ever surfaces the
    // player: it drowned twice during verification. For the first
    // minute after a reset, while Idle, hold jump to swim up and
    // stay in control until the head has been dry for 10 ticks.
    bool tickJoinGuard(const bedrock::Vec3& feet) {
        if (joinGuardTicks_ <= 0) return false;
        --joinGuardTicks_;
        if (headInWater(feet)) {
            joinDryTicks_ = 0;
            if (!joinGuardNoted_) {
                joinGuardNoted_ = true;
                note("join guard: head underwater after join, surfacing");
                reply("水の中にいたから浮上するね");
            }
            pressKey(kKeySpace, true);
            return true;
        }
        if (joinGuardNoted_) {
            pressKey(kKeySpace, false);
            if (++joinDryTicks_ >= 10 && !joinSurfaced_) {
                joinSurfaced_ = true;
                note("join guard: surfaced, breathing (watching until the window ends)");
            }
            // Stay armed until the window ends: a player who
            // surfaced in deep water sinks again once the key is
            // released, and the wet branch above re-engages.
            return joinDryTicks_ < 10; // keep control until breathing is confirmed
        }
        // Joined on dry land: guard off after 5 s of continuously
        // dry head. A one-tick dry reading is not enough — the HUD
        // name (and with it the arming) can appear while the player
        // is still at a transient pre-placement position, and the
        // earlier instant disarm killed the guard on the very tick
        // it armed, before the server placed the player into the
        // pond (measured 2026-11-18: rejoined at y=56, drowned,
        // guard never fired).
        if (++joinDryTicks_ >= 100) joinGuardTicks_ = 0;
        return false;
    }
    // Idle-time guards, run every tick while the run is Idle:
    // - death watch: remember the last position seen alive (the
    //   corpse-recovery uses it) and log foreign screens once per
    //   change; the death screen parks the client in Idle with a
    //   screen up, invisible to every other check.
    // - the join drowning guard above.
    // - idle self-defense: a hostile that closes in on a waiting
    //   player is fought off (2026-11-18: a zombie killed the idle
    //   bot at night after a failed forage — nothing defended it).
    bool tickIdleGuards(const bedrock::Vec3& feet) {
        if (deathPending_) return false; // the death watch owns the client now
        const std::string screen = topScreenName();
        if (screen != lastIdleScreen_) {
            lastIdleScreen_ = screen;
            if (!screen.empty() && screen != "hud_screen")
                note("idle screen: '" + screen + "'");
        }
        if (screen.empty() || screen == "hud_screen" || screen == "hud_crosshair_screen") {
            lastAlivePos_ = feet; hasLastAlivePos_ = true;
        }
        if (tickJoinGuard(feet)) return true;
        if (game_.gameplayActive() && (tickCount_ % 4) == 0) {
            if (auto mob = scanHostile(feet, 12.0)) {
                const double dy = std::abs(mob->center.y - feet.y);
                if (dy <= 4.0 || mob->dist <= 6.0) {
                    enterCombat(*mob);
                    tickCombat(feet);
                    return true;
                }
            }
        }
        return false;
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
    bool inWaterNow(const bedrock::Vec3& feet) {
        const auto feetId = blockAt({static_cast<int>(std::floor(feet.x)),
                                     static_cast<int>(std::floor(feet.y)),
                                     static_cast<int>(std::floor(feet.z))});
        return contains(localPart(feetId), "water") || headInWater(feet);
    }
    void startEscape() {
        ++escapeStreak_;
        if (combatEid_) {
            // Surfacing abandons the fight: ignore that mob for a while,
            // or the threat scan re-enters combat the moment we surface
            // and the two states chase each other forever.
            combatIgnore_[combatEid_] = tickCount_ + 20 * 60;
            combatEid_ = 0;
            note("combat: abandoned by surfacing");
        }
        if (!savedId_.empty()) {
            // Abandoning a mine-through detour: blame the real goal, and
            // restore the run's block type — otherwise the next scan
            // would hunt the detour's block (e.g. seabed stone).
            target_ = savedTarget_; targetId_ = savedId_; savedId_.clear();
            hasTarget_ = true;
        }
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
        pathWalk_.clear(); pathDigLine_.clear(); pathBridgeLine_.clear(); pathBox_ = false;
    }
    void updatePath(const bedrock::Vec3& feet) {
        std::vector<bedrock::Vec3> points;
        bool box = false; BlockPos3 boxPos{};
        if (showPath_ && hasTarget_ && (phase_ == Phase::Navigate || phase_ == Phase::Mine)) {
            const bedrock::Vec3 goal{target_.x + .5f, target_.y + .5f, target_.z + .5f};
            if (!path_.empty() && phase_ == Phase::Navigate) {
                // Draw the actual A* route: remaining waypoints, then goal.
                // Segments are coloured by the kind of cell they enter:
                // walk = green, dig = yellow, bridge = cyan.
                std::vector<bedrock::Vec3> walk, dig, bridge;
                bedrock::Vec3 prev{feet.x, feet.y + 0.25f, feet.z};
                auto seg = [&](const bedrock::Vec3& to, std::size_t i) {
                    auto& dst = (i < path_.size() && pathBridge_[i]) ? bridge
                                : (i < path_.size() && pathDig_[i]) ? dig : walk;
                    dst.push_back(prev); dst.push_back(to); prev = to;
                };
                for (std::size_t i = pathIdx_; i < path_.size(); ++i)
                    seg({path_[i].x + .5f, path_[i].y + 0.3f, path_[i].z + .5f}, i);
                seg(goal, path_.size());
                box = true; boxPos = target_;
                std::lock_guard lock(pathMutex_);
                pathWalk_ = std::move(walk); pathDigLine_ = std::move(dig);
                pathBridgeLine_ = std::move(bridge);
                pathBox_ = box; pathBoxPos_ = boxPos;
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
        pathWalk_ = std::move(points); pathDigLine_.clear(); pathBridgeLine_.clear();
        pathBox_ = box; pathBoxPos_ = boxPos;
    }
    static void BDS_HOOK_CALL onRenderLevel(void* user, void* raw, BDS_HookNext next, void* frame) {
        auto& self = *static_cast<AutoMineMod*>(user);
        // Draw AFTER the native world pass: it is what supplies this frame's
        // view/projection, render origin and depth state. Drawing before it
        // (the first attempt) projected the lines through the previous pass
        // and they came out as huge black wireframes.
        next(frame, raw);
        self.drawPathGuarded(raw);
    }
    void drawPath(void* raw) {
        crashrec::Section section("drawPath");
        if (!outlineReady_ || !showPath_) return;
        const auto& args = *static_cast<BDS_RenderLevelArgs*>(raw);
        if (!args.renderer || !args.screen) return;
        std::vector<bedrock::Vec3> walk, dig, bridge; bool box{}; BlockPos3 boxPos{};
        { std::lock_guard lock(pathMutex_);
          walk = pathWalk_; dig = pathDigLine_; bridge = pathBridgeLine_;
          box = pathBox_; boxPos = pathBoxPos_; }
        if (walk.size() < 2 && dig.size() < 2 && bridge.size() < 2 && !box) return;
        const auto origin = outline_.renderOrigin(args.renderer);
        if (!origin) return;
        auto rel = [&](const bedrock::Vec3& p) {
            return bedrock::Vec3{p.x - origin->x, p.y - origin->y, p.z - origin->z};
        };
        bedrock::WorldOutlineGeometry geometry;
        geometry.origin = *origin;
        auto addLine = [&](const std::vector<bedrock::Vec3>& pts,
                           const std::array<float, 4>& color) {
            if (pts.size() < 2) return;
            bedrock::WorldGeometryBatch batch;
            batch.color = color;
            for (std::size_t i = 1; i < pts.size(); ++i) {
                batch.vertices.push_back(rel(pts[i - 1]));
                batch.vertices.push_back(rel(pts[i]));
            }
            geometry.lines.push_back(std::move(batch));
        };
        addLine(walk, {0.3f, 1.f, 0.35f, 1.f});    // route: bright green
        addLine(dig, {1.f, 0.8f, 0.15f, 1.f});     // dig-through: yellow
        addLine(bridge, {0.3f, 0.9f, 1.f, 1.f});   // bridge cells: cyan
        if (box) {
            bedrock::WorldGeometryBatch batch;
            batch.color = {1.f, 0.45f, 0.1f, 1.f};
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
        crashrec::Section section("scan");
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
            // The chain's crafting table stands on this block: mining it
            // (or the table itself via dig-through) would strand the
            // chain, so the cell under the table is never a target.
            if (chainTablePlaced_ && pos.x == chainTablePos_.x &&
                pos.y == chainTablePos_.y - 1 && pos.z == chainTablePos_.z)
                continue;
            bedrock::BlockPos bp; bp.x = pos.x; bp.y = pos.y; bp.z = pos.z;
            auto cell = world_.getBlock(src, bp);
            if (cell.identifier.empty() || isAir(cell.identifier)) continue;
            if (scanIds_.empty()) {
                if (localPart(cell.identifier) != targetId_) continue;
            } else if (std::find(scanIds_.begin(), scanIds_.end(),
                                 localPart(cell.identifier)) == scanIds_.end()) {
                continue;
            }
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
                scanFoundId_ = localPart(cell.identifier);
                if (d2 < 36.0) { scanCursor_ = total; break; } // close enough: go now
            }
        }
        if (scanCursor_ < total) return;
        if (hasTarget_) {
            emptyScans_ = 0;
            if (!scanIds_.empty()) {
                // Override scan (gather / cobble wait): adopt the found
                // block as the working target so tool/tier logic sees the
                // real block; the original target is restored when the
                // override ends.
                if (!scanOverrideHeld_) { scanSavedId_ = targetId_; scanOverrideHeld_ = true; }
                targetId_ = scanFoundId_;
            }
            note("target: " + targetId_ + " at " + std::to_string(target_.x) + "," +
                 std::to_string(target_.y) + "," + std::to_string(target_.z));
            phase_ = Phase::Navigate;
            stuckTicks_ = 0; lastX_ = feet.x; lastZ_ = feet.z; navTicks_ = 0;
            pathFor_ = {INT_MIN, 0, 0}; replans_ = 0; path_.clear(); pathDig_.clear(); pathBridge_.clear(); pathIdx_ = 0;
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
    // and entering a Dig cell at its real break-time cost (blockDigCost:
    // dirt ~2, stone ~6, deepslate ~9 per blocking block) — the planner
    // compares tunnelling against the detour honestly and picks the
    // shortcut when it is genuinely faster. Fluids are never entered, so
    // routes stay on land and go around water instead of through it.
    enum class CellKind : std::uint8_t { Stand, Dig, Blocked, Bridge };
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
               l == "leaf_litter" || l == "bush" || l == "vine" ||
               l == "sculk_vein" || l == "glow_lichen" ||
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
    // Break time of one blocking block, in walk-step units (1.0 = one
    // walk step): the planner weighs digging through against walking
    // around with real numbers, so a short tunnel wins when it is
    // genuinely faster (the old flat 25.0 made almost every detour
    // "win" on paper, then execution fell back to ad-hoc digging).
    static double blockDigCost(const std::string& id) {
        const auto l = localPart(id);
        if (contains(l, "leaves")) return 1.5;
        if (l == "dirt" || l == "grass_block" || l == "coarse_dirt" || l == "podzol" ||
            l == "rooted_dirt" || l == "mud" || contains(l, "sand") || l == "gravel" ||
            l == "clay" || l == "snow" || l == "soul_soil")
            return 2.0;
        if (contains(l, "log") || contains(l, "wood") || contains(l, "planks") ||
            l == "crafting_table")
            return 4.0;
        if (contains(l, "deepslate") || l == "tuff" || l == "basalt") return 9.0;
        if (l == "obsidian" || l == "crying_obsidian") return 80.0;
        return 6.0; // stone family and everything else solid
    }
    struct CellInfo {
        CellKind kind{CellKind::Blocked};
        double digCost{0.0}; // valid when kind == Dig: total enter cost
    };
    using CellCache = std::unordered_map<CellKey, CellInfo, CellKeyHash>;
    CellKind classifyCell(CellKey c, CellCache& cache) {
        if (auto it = cache.find(c); it != cache.end()) return it->second.kind;
        CellKind kind = CellKind::Blocked;
        double digCost = 0.0;
        // Never plan a dig through the chain's crafting table.
        if (chainTablePlaced_ && c.x == chainTablePos_.x && c.z == chainTablePos_.z &&
            (c.y == chainTablePos_.y || c.y + 1 == chainTablePos_.y)) {
            cache.emplace(c, CellInfo{kind, 0.0});
            return kind;
        }
        const auto ground = blockAt({c.x, c.y - 1, c.z});
        const auto feetB = blockAt({c.x, c.y, c.z});
        const auto headB = blockAt({c.x, c.y + 1, c.z});
        if (!ground.empty() && !feetB.empty() && !headB.empty() &&
            !isFluidCell(ground) && !isFluidCell(feetB) && !isFluidCell(headB) &&
            isSolidCell(ground)) {
            const bool fP = isPassableCell(feetB), hP = isPassableCell(headB);
            if (fP && hP) kind = CellKind::Stand;
            else if ((fP || isDiggableCell(feetB)) && (hP || isDiggableCell(headB))) {
                kind = CellKind::Dig;
                digCost = 1.0; // the step itself
                if (!fP) digCost += blockDigCost(feetB);
                if (!hP) digCost += blockDigCost(headB);
            }
        }
        if (kind == CellKind::Blocked && planSpares_ > 0 &&
            !ground.empty() && isAir(ground) &&
            !feetB.empty() && !headB.empty() &&
            isPassableCell(feetB) && isPassableCell(headB) &&
            !bridgeBlocked_.count({c.x, c.y - 1, c.z})) {
            // Bridge: the floor cell is air, but a spare block can be
            // placed into it — an anchor face must exist (a solid cell
            // under the floor, or a solid horizontal neighbour of it).
            const BlockPos3 g{c.x, c.y - 1, c.z};
            bool anchor = isSolidCell(blockAt({g.x, g.y - 1, g.z}));
            if (!anchor) {
                // One chain hop is allowed: a gap cell whose neighbour is
                // itself bridgeable (air floor next to a solid cell)
                // becomes reachable once that neighbour is filled, so
                // gaps up to 3 wide can be planned across.
                constexpr int kD[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
                for (const auto& d : kD) {
                    const BlockPos3 n{g.x + d[0], g.y, g.z + d[1]};
                    const auto nid = blockAt(n);
                    if (isSolidCell(nid)) { anchor = true; break; }
                    if (!isAir(nid)) continue;
                    for (const auto& e : kD)
                        if (isSolidCell(blockAt({n.x + e[0], g.y, n.z + e[1]}))) {
                            anchor = true; break;
                        }
                    if (anchor) break;
                }
            }
            if (anchor) kind = CellKind::Bridge;
        }
        // Cells the follower physically failed on during this target
        // are planned around on the next attempt (the obstruction is
        // fed back into the planner instead of being re-walked into).
        if (planBlocked_.count(c) && kind != CellKind::Blocked) {
            kind = CellKind::Blocked;
            digCost = 0.0;
        }
        cache.emplace(c, CellInfo{kind, digCost});
        return kind;
    }
    double digCostOf(CellKey c, CellCache& cache) {
        classifyCell(c, cache);
        const auto it = cache.find(c);
        return it == cache.end() ? 25.0 : it->second.digCost;
    }

    // Spare blocks are whatever mining piles up; bridging consumes them
    // as scaffolding, so only expendable block types qualify.
    // Gravity blocks fall into any hole dug beneath them, so a pocket
    // dug under one refills before a table/furnace can be placed in it
    // (measured 2026-11-17: the table pocket cell stayed gravel
    // through 25 s of placement pulses and the smelt then failed the
    // same way). Never dig a pocket under one, and never place into a
    // cell with one above it.
    bool isGravityBlock(const std::string& local) {
        return local == "gravel" || local == "sand" || local == "red_sand" ||
               contains(local, "concrete_powder") || contains(local, "suspicious_");
    }
    bool isSpareBlock(const std::string& local) {
        return local == "cobblestone" || local == "dirt" || local == "stone" ||
               local == "andesite" || local == "granite" || local == "diorite" ||
               local == "cobbled_deepslate" || local == "netherrack" || local == "tuff" ||
               local == "coarse_dirt" || local == "mossy_cobblestone" ||
               local == "grass_block" || local == "podzol" || local == "rooted_dirt";
    }
    int spareCount() {
        int n = 0;
        for (const auto& it : hotbarItems())
            if (isSpareBlock(localPart(it.id))) n += it.count;
        return n;
    }
    // Spares across the whole inventory: the ascend pulls stacks
    // up from the main inventory as it climbs, so its trigger
    // counts everything, not just the hotbar.
    int totalSpareCount() {
        if (!craftReady_) return spareCount();
        int n = 0;
        for (const auto& s : craft_.snapshot())
            if (s.count && isSpareBlock(localPart(s.item))) n += s.count;
        return n;
    }
    int spareSlot() { // hotbar slot holding the largest spare stack, or -1
        const auto slots = hotbarItems();
        int best = -1, bestCount = 0;
        for (int i = 0; i < 9; ++i) {
            const auto& it = slots[static_cast<std::size_t>(i)];
            if (it.count > bestCount && isSpareBlock(localPart(it.id))) {
                best = i; bestCount = it.count;
            }
        }
        return best;
    }

    // A failed (re)plan must not throw away a route that is still being
    // followed: one transient failure (a pocket start, a momentarily
    // unreadable chunk) used to drop the bot into straight-line legacy
    // navigation, which walked it into walls and water. Only a path that
    // has been fully consumed is worthless.
    void discardPathIfExhausted() {
        if (pathIdx_ >= path_.size()) { path_.clear(); pathDig_.clear(); pathBridge_.clear(); pathIdx_ = 0; pathPartial_ = false; }
    }

    bool planPath(bedrock::Vec3 feet) {
        crashrec::Section section("planPath");
        if (!(planBlockedFor_ == target_)) { planBlocked_.clear(); planBlockedFor_ = target_; }
        pathFor_ = target_;
        planSpares_ = spareCount();
        CellCache cache;
        CellKey start{static_cast<int>(std::floor(feet.x)),
                      static_cast<int>(std::floor(feet.y)),
                      static_cast<int>(std::floor(feet.z))};
        // The cell under the player can classify Blocked right after a
        // mine-through or at a ledge lip; A* then expands only the start
        // and reports "no route (1 expansions)". Snap the start to the
        // nearest usable cell instead of failing the whole plan.
        if (classifyCell(start, cache) == CellKind::Blocked) {
            double bestD = 1e18;
            CellKey best = start;
            bool found = false;
            for (int dx = -1; dx <= 1; ++dx)
                for (int dy = -1; dy <= 1; ++dy)
                    for (int dz = -1; dz <= 1; ++dz) {
                        if (!dx && !dy && !dz) continue;
                        const CellKey c{start.x + dx, start.y + dy, start.z + dz};
                        if (classifyCell(c, cache) == CellKind::Blocked) continue;
                        const double d = std::hypot(c.x + .5 - feet.x, c.z + .5 - feet.z) +
                                         std::abs(c.y + .5 - feet.y);
                        if (d < bestD) { bestD = d; best = c; found = true; }
                    }
            if (found) start = best;
        }
        // Goal cells: Stand/Dig cells around the target whose eye point is
        // within mining reach (4.2) of the target centre.
        std::vector<CellKey> goals;
        bool anyStandGoal = false;
        for (int dx = -1; dx <= 1; ++dx)
            for (int dy = -1; dy <= 2; ++dy)
                for (int dz = -1; dz <= 1; ++dz) {
                    const CellKey g{target_.x + dx, target_.y + dy, target_.z + dz};
                    const auto gk = classifyCell(g, cache);
                    if (gk == CellKind::Blocked) continue;
                    const double ex = dx, ey = g.y + 1.62 - (target_.y + .5), ez = dz;
                    if (ex * ex + ey * ey + ez * ez <= 4.2 * 4.2) {
                        goals.push_back(g);
                        if (gk == CellKind::Stand) anyStandGoal = true;
                    }
                }
        if (goals.empty()) {
            note("path: no standpoint near target");
            discardPathIfExhausted();
            return false;
        }
        // Heuristic weight: when every standpoint is a Dig cell (an ore
        // embedded in rock), the approach IS tunnelling — estimate the
        // cost-to-go at dig prices (~10/step) so the search dives at
        // the goal instead of flooding every open cave first (measured
        // 2026-11-17: 24,000 expansions spent in caves a few blocks
        // from a deepslate diamond ore, no route). With a Stand
        // standpoint the approach is mostly walking: weight 1.5 keeps
        // the search goal-directed without giving up detours.
        const double hWeight = anyStandGoal ? 1.5 : 10.0;
        auto heuristic = [&](const CellKey& c) {
            double best = 1e18;
            for (const auto& g : goals) {
                const double d = std::sqrt(static_cast<double>(
                    (c.x - g.x) * (c.x - g.x) + (c.y - g.y) * (c.y - g.y) + (c.z - g.z) * (c.z - g.z)));
                if (d < best) best = d;
            }
            return best * hWeight;
        };
        struct OpenNode {
            double f; CellKey c;
            bool operator<(const OpenNode& o) const noexcept { return f > o.f; } // min-heap
        };
        std::priority_queue<OpenNode> open;
        std::unordered_map<CellKey, double, CellKeyHash> gScore;
        std::unordered_map<CellKey, CellKey, CellKeyHash> cameFrom;
        std::unordered_map<CellKey, bool, CellKeyHash> digFlag;
        std::unordered_map<CellKey, bool, CellKeyHash> bridgeFlag;
        gScore[start] = 0;
        open.push({heuristic(start), start});
        constexpr int kDirs[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
        constexpr int kMaxExpansions = 24000;
        int expansions = 0;
        CellKey found{}; bool haveFound = false;
        // Best-effort fallback (the Baritone approach): if the budget
        // dies before a goal cell is reached, keep the path to the
        // closest expanded node — following it makes real progress
        // and the next plan starts from there, instead of declaring
        // "no route" while embedded in diggable rock.
        CellKey bestNode = start;
        double bestH = heuristic(start);
        while (!open.empty() && expansions < kMaxExpansions) {
            const OpenNode cur = open.top(); open.pop();
            const double cg = gScore.count(cur.c) ? gScore[cur.c] : 1e18;
            if (cur.f > cg + heuristic(cur.c) + 1e-9) continue; // stale entry
            ++expansions;
            { const double h = heuristic(cur.c); if (h < bestH - 1e-9) { bestH = h; bestNode = cur.c; } }
            if (std::find(goals.begin(), goals.end(), cur.c) != goals.end()) {
                found = cur.c; haveFound = true; break;
            }
            auto relax = [&](const CellKey& next, double cost, CellKind kind) {
                const double ng = cg + cost;
                if (!gScore.count(next) || ng < gScore[next] - 1e-9) {
                    gScore[next] = ng;
                    cameFrom[next] = cur.c;
                    digFlag[next] = (kind == CellKind::Dig);
                    bridgeFlag[next] = (kind == CellKind::Bridge);
                    open.push({ng + heuristic(next), next});
                }
            };
            for (const auto& d : kDirs) {
                const int nx = cur.c.x + d[0], nz = cur.c.z + d[1];
                {   // walk, same level
                    const CellKey n{nx, cur.c.y, nz};
                    const auto k = classifyCell(n, cache);
                    if (k != CellKind::Blocked)
                        relax(n, k == CellKind::Dig ? digCostOf(n, cache)
                             : k == CellKind::Bridge ? 5.0 : 1.0, k);
                }
                {   // step up one block: the block above our own head
                    // is swept during the jump — passable is free,
                    // diggable adds its break cost (an up-tunnel step),
                    // anything else refuses the move.
                    const CellKey n{nx, cur.c.y + 1, nz};
                    const auto k = classifyCell(n, cache);
                    const auto aboveHead = blockAt({cur.c.x, cur.c.y + 2, cur.c.z});
                    const bool ahPass = isPassableCell(aboveHead);
                    const bool ahDig = !ahPass && isDiggableCell(aboveHead);
                    if (k != CellKind::Blocked && k != CellKind::Bridge && (ahPass || ahDig)) {
                        double cost = k == CellKind::Dig ? digCostOf(n, cache) + 0.6 : 1.6;
                        if (ahDig) cost += blockDigCost(aboveHead);
                        relax(n, cost, k);
                    }
                }
                // drop 1-3 blocks onto a Stand cell, fall shaft passable
                if (isPassableCell(blockAt({nx, cur.c.y + 1, nz}))) {
                    for (int drop = 1; drop <= 3; ++drop) {
                        const CellKey n{nx, cur.c.y - drop, nz};
                        const auto nk = classifyCell(n, cache);
                        if (nk == CellKind::Dig) {
                            // One level only: dig the landing cell's
                            // feet block out and drop in (a diagonal
                            // down-tunnel step).
                            if (drop == 1) relax(n, digCostOf(n, cache) + 0.3, CellKind::Dig);
                            break;
                        }
                        if (nk != CellKind::Stand) break;
                        bool shaft = true;
                        for (int k2 = 0; k2 < drop; ++k2)
                            if (!isPassableCell(blockAt({nx, cur.c.y - k2, nz}))) { shaft = false; break; }
                        if (!shaft) break;
                        relax(n, 1.0 + 0.3 * drop, CellKind::Stand);
                    }
                }
            }
            {   // dig straight down: the cell below is a Dig cell whose
                // feet block is our own floor — dig it out and drop in.
                // Without this move the planner cannot descend through
                // rock at all (drops only land on Stand cells), which
                // is why ore approaches from above degenerated into
                // unplanned straight-line digging (measured 2026-11-17:
                // a diamond ore 5 blocks below, 24,000 expansions, no
                // route and no node closer than the start).
                const CellKey n{cur.c.x, cur.c.y - 1, cur.c.z};
                if (classifyCell(n, cache) == CellKind::Dig)
                    relax(n, digCostOf(n, cache) + 0.3, CellKind::Dig);
            }
        }
        bool partial = false;
        if (!haveFound) {
            if (!(bestNode == start) && bestH < heuristic(start) - 1e-9) {
                found = bestNode; haveFound = true; partial = true;
            } else {
                note("path: no route (" + std::to_string(expansions) + " expansions)");
                discardPathIfExhausted();
                return false;
            }
        }
        path_.clear(); pathDig_.clear(); pathBridge_.clear(); pathIdx_ = 0;
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
            const bool bridge = bridgeFlag.count(cells[i]) && bridgeFlag[cells[i]];
            const bool last = (i + 1 == cells.size());
            bool keep = last || dig || bridge;
            if (!keep) {
                const auto& a = cells[i - 1]; const auto& b = cells[i]; const auto& n = cells[i + 1];
                keep = !(a.y == b.y && b.y == n.y && (b.x - a.x) == (n.x - b.x) &&
                         (b.z - a.z) == (n.z - b.z));
            }
            if (keep) {
                path_.push_back({cells[i].x, cells[i].y, cells[i].z});
                pathDig_.push_back(dig);
                pathBridge_.push_back(bridge);
            }
        }
        pathPartial_ = partial;
        note("path: " + std::to_string(path_.size()) + " waypoints (" +
             std::to_string(expansions) + " expansions)" + (partial ? " partial" : ""));
        return true;
    }

    void enterMineCell(BlockPos3 cell, const std::string& id) {
        savedTarget_ = target_; savedId_ = targetId_;
        target_ = cell; targetId_ = localPart(id);
        hasTarget_ = true; phase_ = Phase::Mine; mineTicks_ = 0;
        path_.clear(); pathDig_.clear(); pathBridge_.clear(); pathIdx_ = 0;
        pathFor_ = {INT_MIN, 0, 0};
        pathDigResume_ = false;
        note("mine-through: " + targetId_);
    }
    // A Dig waypoint ON the planned route: mine it without discarding
    // the route. The old flow cleared the path and re-planned from a
    // full rescan after every single block (measured 2026-11-17 in
    // the diamond approach: one 9,000-expansion replan + rescan per
    // deepslate block, ~15 s per block). With the route kept, the
    // follower resumes at the same waypoint the moment the cell
    // opens and walks the rest of the plan.
    void enterPathDigCell(BlockPos3 cell, const std::string& id) {
        savedTarget_ = target_; savedId_ = targetId_;
        target_ = cell; targetId_ = localPart(id);
        hasTarget_ = true; phase_ = Phase::Mine; mineTicks_ = 0;
        pathDigResume_ = true;
        note("path dig: " + targetId_);
    }

    // Fill the current run of bridge waypoints with spare blocks.
    // Cells are filled far-to-near: from a standing eye, the side face of
    // the block under the player can never be hit (the ray clips its top
    // face first and the block lands one cell short, on top of the edge
    // — measured live), while the far anchor's face is approached across
    // open air and hits cleanly. Each filled cell then becomes the
    // anchor for the next one nearer the player. A solid cell under a
    // gap (a pit floor) is filled through its top face instead. Aiming
    // and the use pulse reuse the crafting-table placement pattern.
    // Returns true while work is in progress; false once every floor
    // cell of the run is solid and normal following can resume.
    bool tickBridge(bedrock::Vec3 feet) {
        // The gap run: the current bridge waypoint and the consecutive
        // bridge waypoints after it.
        std::vector<BlockPos3> run;
        for (std::size_t i = pathIdx_; i < path_.size() && pathBridge_[i]; ++i)
            run.push_back({path_[i].x, path_[i].y - 1, path_[i].z});
        bool allSolid = true;
        for (const auto& g : run)
            if (!isSolidCell(blockAt(g))) { allSolid = false; break; }
        if (allSolid) { bridgeTicks_ = 0; setUse(false); return false; }
        pressKey(kKeyW, false); pressKey(kKeySpace, false);
        pressKey(kKeyA, false); pressKey(kKeyD, false);
        if (++bridgeTicks_ > 200) {
            bridgeTicks_ = 0;
            for (const auto& g : run)
                if (!isSolidCell(blockAt(g))) bridgeBlocked_.insert(g);
            setUse(false);
            note("bridge: placement failed; replanning without it");
            pathFor_ = {INT_MIN, 0, 0};
            // Escalation: if every replan for the same target keeps
            // producing bridge runs that cannot be placed (measured
            // 2026-11-17 on fresh3: 51 failed runs in a tight dirt
            // pocket, ~11 min, zero progress — each replan found a
            // neighbouring bridge cell and failed the same way, and
            // tickBridge returning "working" starved the navigate
            // timeout), abandon the target itself and rescan.
            if (!navReturn_) {
                if (!(bridgeFailFor_ == target_)) { bridgeFailFor_ = target_; bridgeFailStreak_ = 0; }
                if (++bridgeFailStreak_ >= 3 && hasTarget_) {
                    note("bridge: giving up on this target after repeated failures");
                    blacklist_.insert(target_);
                    hasTarget_ = false;
                    path_.clear();
                    phase_ = Phase::Scan; scanCursor_ = 0; bestDist2_ = 1e18;
                    bridgeFailStreak_ = 0;
                }
            }
            return true;
        }
        const int slot = spareSlot();
        if (slot < 0) {
            bridgeTicks_ = 0;
            for (const auto& g : run) bridgeBlocked_.insert(g);
            setUse(false);
            note("bridge: no spare blocks; replanning");
            pathFor_ = {INT_MIN, 0, 0};
            // Escalation (same as placement failures): with no blocks
            // to place, every replan for this target fails the same
            // way, and tickFollowPath resets the navigate timeout on
            // every bridge tick, so the loop never ends on its own
            // (measured 2026-11-17: 2,000+ repeats at a ravine edge).
            if (!navReturn_) {
                if (!(bridgeFailFor_ == target_)) { bridgeFailFor_ = target_; bridgeFailStreak_ = 0; }
                if (++bridgeFailStreak_ >= 3 && hasTarget_) {
                    note("bridge: giving up on this target (no spare blocks)");
                    blacklist_.insert(target_);
                    hasTarget_ = false;
                    path_.clear();
                    phase_ = Phase::Scan; scanCursor_ = 0; bestDist2_ = 1e18;
                    bridgeFailStreak_ = 0;
                }
            }
            return true;
        }
        selectSlot(slot);
        bedrock::Vec3 eye;
        if (!eyePos(eye)) return true;
        // Choose the farthest missing cell that has a usable anchor, and
        // aim at the anchor face that fills it.
        bedrock::Vec3 aim{};
        bool haveAim = false;
        BlockPos3 fillCell{INT_MIN, 0, 0};
        double bestDist = -1;
        constexpr int kD[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
        for (const auto& g : run) {
            if (isSolidCell(blockAt(g))) continue;
            const double gd = std::hypot(g.x + .5 - feet.x, g.z + .5 - feet.z);
            for (const auto& d : kD) {
                const BlockPos3 a{g.x + d[0], g.y, g.z + d[1]};
                if (!isSolidCell(blockAt(a))) continue;
                if (gd > bestDist) {
                    bestDist = gd;
                    aim = {a.x + .5f - .5f * static_cast<float>(d[0]), g.y + .5f,
                           a.z + .5f - .5f * static_cast<float>(d[1])};
                    fillCell = g;
                    haveAim = true;
                }
            }
            if (isSolidCell(blockAt({g.x, g.y - 1, g.z})) && gd > bestDist) {
                bestDist = gd;
                aim = {g.x + .5f, static_cast<float>(g.y), g.z + .5f};
                fillCell = g;
                haveAim = true;
            }
        }
        if (!haveAim) {
            bridgeTicks_ = 0;
            for (const auto& g : run)
                if (!isSolidCell(blockAt(g))) bridgeBlocked_.insert(g);
            pathFor_ = {INT_MIN, 0, 0};
            return true;
        }
        // Filling a cell the player is standing over: do it the way a
        // player does — jump, place the block underneath while
        // airborne, land on it. Placing into the cell while standing in
        // its space ejects the player sideways (measured live: pushed
        // off the corner into the water, client died moments later).
        {
            const double cdx = fillCell.x + .5 - (feet.x + .3);
            const double cdz = fillCell.z + .5 - (feet.z + .3);
            if (std::hypot(cdx, cdz) < 1.05) {
                faceToward(eye, aim, 3.f);
                if (bridgeJumpTicks_ == 0) {
                    pressKey(kKeySpace, true);
                    bridgeJumpTicks_ = 1;
                    setUse(false);
                } else {
                    pressKey(kKeySpace, false);
                    setUse(bridgeJumpTicks_ <= 6);
                    if (++bridgeJumpTicks_ > 9) bridgeJumpTicks_ = 0;
                }
                return true;
            }
            bridgeJumpTicks_ = 0;
            pressKey(kKeySpace, false);
        }
        // Survival placement reach is ~4.5 blocks from the eye; the far
        // anchor of a 3-wide gap sits ~4.7 away from where the follower
        // stops, so the pulses silently hit nothing (measured live).
        // Creep toward the gap first, probing the floor half a step
        // ahead of the body centre: the creep stops at the last solid
        // floor, so the player can never walk off the edge (a plain
        // distance threshold let the body centre cross it and the
        // player fell in — measured live twice).
        const double aimDist = std::hypot(aim.x - eye.x, aim.y - eye.y, aim.z - eye.z);
        if (aimDist > 4.2) {
            const double cx = feet.x + .3, cz = feet.z + .3;
            const double hx = aim.x - cx, hz = aim.z - cz;
            const double hl = std::hypot(hx, hz);
            const double px = cx + (hl > 1e-6 ? hx / hl * .45 : 0);
            const double pz = cz + (hl > 1e-6 ? hz / hl * .45 : 0);
            const BlockPos3 probe{static_cast<int>(std::floor(px)),
                                  static_cast<int>(std::floor(feet.y)) - 1,
                                  static_cast<int>(std::floor(pz))};
            if (isSolidCell(blockAt(probe))) {
                // Fail fast when the approach is walled off (a terrain
                // rise the creep cannot climb): no movement for 30 ticks
                // means this stance will never reach the anchor.
                if (std::hypot(feet.x - bridgeLastX_, feet.z - bridgeLastZ_) > .05) {
                    bridgeLastX_ = feet.x; bridgeLastZ_ = feet.z; bridgeStuckTicks_ = 0;
                } else if (++bridgeStuckTicks_ > 30) {
                    bridgeStuckTicks_ = 0;
                    bridgeTicks_ = 0;
                    for (const auto& g : run)
                        if (!isSolidCell(blockAt(g))) bridgeBlocked_.insert(g);
                    setUse(false);
                    note("bridge: approach blocked; replanning without it");
                    pathFor_ = {INT_MIN, 0, 0};
                    return true;
                }
                faceAim(eye, {aim.x, eye.y, aim.z}, 10.f, 0.25f);
                pressKey(kKeyW, true);
                setUse(false);
                return true;
            }
        }
        pressKey(kKeyW, false);
        faceToward(eye, aim, 3.f);
        setUse((bridgeTicks_ % 12) < 2);
        return true;
    }

    void tickFollowPath(bedrock::Vec3 feet) {
        ++navTicks_;
        const std::size_t idxAtEntry = pathIdx_;
        if (navTicks_ > 20 * 45) { // same overall budget as legacy navigation
            note("navigate timeout (path); blacklist target");
            if (chainWaitingCobble_) ++cobbleNavFails_;
            blacklist_.insert(target_); hasTarget_ = false;
            pressKey(kKeyW, false); pressKey(kKeySpace, false);
            path_.clear(); pathDig_.clear(); pathBridge_.clear();
            phase_ = Phase::Scan; scanCursor_ = 0; bestDist2_ = 1e18;
            return;
        }
        if (pathIdx_ >= path_.size()) {
            pressKey(kKeyW, false); pressKey(kKeySpace, false);
            if (pathPartial_) {
                // A best-effort path that ran out is progress, not a
                // failure: replan from here without spending the
                // failure budget, and reset the navigate clock.
                pathPartial_ = false;
                pathFor_ = {INT_MIN, 0, 0};
                navTicks_ = 0;
                return;
            }
            if (++replans_ > 2) {
                note("path exhausted; blacklist target");
                blacklist_.insert(target_); hasTarget_ = false;
                phase_ = Phase::Scan; scanCursor_ = 0; bestDist2_ = 1e18;
            } else pathFor_ = {INT_MIN, 0, 0}; // force a re-plan next tick
            return;
        }
        if (inWaterNow(feet)) {
            // Fell in, or the route starts in water: swim at the surface
            // toward the current waypoint, then resume walking it.
            // Submerged means surface first (backing out from under a
            // ceiling if there is one), exactly as in legacy swimming.
            const BlockPos3 wp0 = path_[pathIdx_];
            const bedrock::Vec3 eye0{feet.x, feet.y + 1.62f, feet.z};
            faceAim(eye0, {wp0.x + .5f, eye0.y, wp0.z + .5f}, 10.f, 0.25f);
            if (headInWater(feet)) {
                pressKey(kKeyW, false);
                pressKey(kKeySpace, true);
                const auto above = blockAt({static_cast<int>(std::floor(feet.x)),
                                            static_cast<int>(std::floor(feet.y + 2.62f)),
                                            static_cast<int>(std::floor(feet.z))});
                const bool ceiling = !above.empty() && !isAir(above) &&
                                     !contains(localPart(above), "water");
                pressKey(kKeyS, ceiling);
            } else {
                pressKey(kKeyS, false);
                pressKey(kKeyW, true); pressKey(kKeySpace, true);
                if (std::hypot(wp0.x + .5 - feet.x, wp0.z + .5 - feet.z) < 1.2) ++pathIdx_;
            }
            return;
        }
        if (pathBridge_[pathIdx_]) {
            if (tickBridge(feet)) { navTicks_ = 0; return; } // bridging is progress
            // Floor filled: fall through to normal following this tick.
        }
        const BlockPos3 wp = path_[pathIdx_];
        if (pathDig_[pathIdx_]) {
            // Mine out this waypoint's blocking cell, keeping the
            // route: the Mine completion resumes following it.
            pressKey(kKeyW, false); pressKey(kKeySpace, false);
            const auto feetId = blockAt({wp.x, wp.y, wp.z});
            const auto headId = blockAt({wp.x, wp.y + 1, wp.z});
            if (isDiggableCell(feetId)) { enterPathDigCell({wp.x, wp.y, wp.z}, feetId); return; }
            if (isDiggableCell(headId)) { enterPathDigCell({wp.x, wp.y + 1, wp.z}, headId); return; }
            ++pathIdx_; // already cleared somehow: move on
            return;
        }
        if (!pathBridge_[pathIdx_]) {
            // Waypoint invalidation: the world changed under a
            // walking waypoint (a block appeared in it). Dig it out
            // if it is diggable; if it became impossible, feed it to
            // the planner and replan NOW instead of walking into it
            // until the stuck detector notices.
            const auto wf = blockAt({wp.x, wp.y, wp.z});
            const auto wh = blockAt({wp.x, wp.y + 1, wp.z});
            const bool fSolid = isSolidCell(wf), hSolid = isSolidCell(wh);
            if (fSolid || hSolid) {
                pressKey(kKeyW, false);
                if (fSolid && isDiggableCell(wf)) { enterPathDigCell({wp.x, wp.y, wp.z}, wf); return; }
                if (hSolid && isDiggableCell(wh)) { enterPathDigCell({wp.x, wp.y + 1, wp.z}, wh); return; }
                planBlockedFor_ = target_;
                if (planBlocked_.size() < 64) planBlocked_.insert({wp.x, wp.y, wp.z});
                pathFor_ = {INT_MIN, 0, 0};
                note("path: waypoint blocked, replanning");
                return;
            }
        }
        // --- pure-pursuit following -------------------------------------
        // Advance past every waypoint we have effectively passed: within
        // a generous radius for its level relation, or already behind us.
        // The previous rule (pass within 0.7 blocks of the cell centre)
        // let momentum carry the player past waypoints, after which the
        // damped view control walked a wide U-turn back to them — the
        // visible circling. Dig waypoints are never skipped this way.
        const int feetCellY = static_cast<int>(std::floor(feet.y));
        float yawNow{}, pitchNow{};
        const bool haveYaw = rotation(yawNow, pitchNow);
        while (pathIdx_ < path_.size() && !pathDig_[pathIdx_] && !pathBridge_[pathIdx_]) {
            const BlockPos3 w = path_[pathIdx_];
            const double hx = w.x + .5 - feet.x, hz = w.z + .5 - feet.z;
            const double horiz = std::hypot(hx, hz);
            bool passed = false;
            if (w.y == feetCellY) passed = horiz < 1.05;
            else if (w.y > feetCellY) passed = (feetCellY == w.y && horiz < 1.15);
            else passed = (feet.y <= w.y + 0.6 && horiz < 1.2);
            if (!passed && haveYaw && horiz > 1e-6 && horiz < 2.4) {
                const double rad = yawNow * 3.141592653589793 / 180.0;
                const double fx = -std::sin(rad), fz = std::cos(rad);
                if ((fx * hx + fz * hz) / horiz < -0.35) passed = true; // behind us
            }
            if (!passed) break;
            ++pathIdx_;
            stuckTicks_ = 0;
        }
        // Steady waypoint progress resets the navigate clock: the
        // 45 s budget exists to catch a follower that is NOT getting
        // anywhere, not to kill a long route that is advancing.
        if (pathIdx_ != idxAtEntry) navTicks_ = 0;
        if (pathIdx_ >= path_.size()) {
            pressKey(kKeyW, false); pressKey(kKeySpace, false);
            pressKey(kKeyA, false); pressKey(kKeyD, false);
            return; // the exhausted-path branch at the top replans next tick
        }
        // Aim at the first waypoint at least ~1.7 blocks out (lookahead),
        // or at the current one when it is a Dig cell / the last one.
        std::size_t aimIdx = pathIdx_;
        if (!pathDig_[pathIdx_] && !pathBridge_[pathIdx_]) {
            for (std::size_t i = pathIdx_; i < path_.size(); ++i) {
                aimIdx = i;
                if (std::hypot(path_[i].x + .5 - feet.x, path_[i].z + .5 - feet.z) >= 1.7)
                    break;
            }
        }
        const bedrock::Vec3 eye{feet.x, feet.y + 1.62f, feet.z};
        const bedrock::Vec3 aim{path_[aimIdx].x + .5f, eye.y, path_[aimIdx].z + .5f};
        faceAim(eye, aim, 10.f, 0.25f);
        // Sharp turns pivot in place instead of arcing wide.
        bool wDown = true;
        if (haveYaw) {
            const double want =
                std::atan2(-(aim.x - feet.x), aim.z - feet.z) * 180.0 / 3.141592653589793;
            const double err = std::fmod(want - yawNow + 540.0, 360.0) - 180.0;
            if (std::abs(err) > 65.0) wDown = false;
        }
        pressKey(kKeyW, wDown);
        // Sprint on real straights: W held, the aim point well ahead,
        // and the next stretch level walking (no dig/bridge/step-up
        // coming). On long level stretches, sprint-jump: a short jump
        // pulse on a ~13-tick cycle is the fastest land movement.
        bool sprintJump = false;
        {
            bool wantSprint = false;
            if (wDown) {
                // Distance to the END of the route (the lookahead aim
                // point is capped ~2 blocks out and would never pass
                // the sprint threshold).
                const BlockPos3& last = path_.back();
                const double aimDist = std::hypot(last.x + .5 - feet.x, last.z + .5 - feet.z);
                bool levelAhead = true;
                int levelRun = 0;
                for (std::size_t i = pathIdx_; i < path_.size() && i < pathIdx_ + 4; ++i) {
                    if (pathDig_[i] || pathBridge_[i] || path_[i].y != feetCellY) { levelAhead = false; break; }
                    ++levelRun;
                }
                wantSprint = aimDist >= 4.0 && levelAhead;
                sprintJump = wantSprint && levelRun >= 3 && aimDist >= 6.0;
            }
            if (wantSprint && !sprintWasOn_) note(sprintJump ? "sprint-jump" : "sprint");
            sprintWasOn_ = wantSprint;
            pressKey(kKeySprint, wantSprint);
        }
        // Jump only when the step-up is actually at our feet (the old code
        // held jump whenever any upcoming waypoint was higher, bunny-
        // hopping across flat ground toward a distant rise).
        bool jump = false;
        for (std::size_t i = pathIdx_; i < path_.size() && i < pathIdx_ + 3; ++i) {
            if (path_[i].y < feetCellY) break;
            if (path_[i].y > feetCellY) {
                jump = std::hypot(path_[i].x + .5 - feet.x, path_[i].z + .5 - feet.z) < 1.7;
                break;
            }
        }
        if (sprintJump) {
            jump = (tickCount_ % 13) < 2;
            jumpHoldTicks_ = 0; // sprint jumps are not step-up attempts
        }
        pressKey(kKeySpace, jump);
        // Jump watchdog: a genuine step-up gains the level within a
        // second. Jumping for ~2.5 s without rising means the step
        // is not climbable from here (the ice-pocket failure: the
        // bot bounced against a wall indefinitely). Stop, feed the
        // waypoint to the planner as blocked, and replan.
        if (jump) {
            if (jumpHoldTicks_ == 0) jumpBaseY_ = feetCellY;
            if (++jumpHoldTicks_ > 50 && feetCellY <= jumpBaseY_) {
                jumpHoldTicks_ = 0;
                pressKey(kKeySpace, false); pressKey(kKeyW, false);
                planBlockedFor_ = target_;
                if (planBlocked_.size() < 64) planBlocked_.insert({wp.x, wp.y, wp.z});
                pathFor_ = {INT_MIN, 0, 0};
                note("path: jump watchdog, replanning");
                return;
            }
        } else jumpHoldTicks_ = 0;
        // Stuck on this stretch: the first two checks wiggle (jump +
        // alternating strafe, applied below); only then re-plan, and give
        // up on the target after the re-plan budget runs out.
        if (navTicks_ % 10 == 0) {
            const double moved = std::hypot(feet.x - lastX_, feet.z - lastZ_);
            if (moved < 0.12) {
                if (++stuckTicks_ >= 3) {
                    stuckTicks_ = 0;
                    pressKey(kKeyW, false); pressKey(kKeySpace, false);
                    pressKey(kKeyA, false); pressKey(kKeyD, false);
                    if (++replans_ > 2) {
                        note("path follow stuck; blacklist target");
                        blacklist_.insert(target_); hasTarget_ = false;
                        path_.clear(); pathDig_.clear(); pathBridge_.clear();
                        phase_ = Phase::Scan; scanCursor_ = 0; bestDist2_ = 1e18;
                    } else {
                        // Feed the obstruction back into the planner:
                        // the waypoint we could not enter (and the
                        // cell physically in front) is planned around
                        // on the next attempt instead of re-walked.
                        if (pathIdx_ < path_.size() && planBlocked_.size() < 64) {
                            const BlockPos3 w = path_[pathIdx_];
                            planBlockedFor_ = target_;
                            planBlocked_.insert({w.x, w.y, w.z});
                            note("path: replan around blocked cell " +
                                 std::to_string(w.x) + "," + std::to_string(w.y) + "," +
                                 std::to_string(w.z));
                        }
                        pathFor_ = {INT_MIN, 0, 0};
                    }
                    return;
                }
            } else stuckTicks_ = 0;
            lastX_ = feet.x; lastZ_ = feet.z;
        }
        if (stuckTicks_ == 1 || stuckTicks_ == 2) {
            pressKey(kKeySpace, true);
            pressKey(kKeyD, stuckTicks_ == 1);
            pressKey(kKeyA, stuckTicks_ == 2);
        } else {
            pressKey(kKeyA, false); pressKey(kKeyD, false);
        }
    }

    void tickNavigate(bedrock::Vec3 feet) {
        if (!hasTarget_) { phase_ = Phase::Scan; scanCursor_ = 0; bestDist2_ = 1e18; return; }
        // Target may have changed (another player mined it, physics, ...).
        const auto targetNow = blockAt(target_);
        // A return trip abandoned by a navigate timeout leaves the flag
        // set while a fresh mining target gets assigned; that is not the
        // table vanishing — just drop the stale flag.
        if (navReturn_ && !(target_ == chainTablePos_)) navReturn_ = false;
        if (navReturn_) {
            if (!contains(localPart(targetNow), "crafting_table")) {
                note("craft chain: table gone; abandoning return");
                navReturn_ = false; chainTablePlaced_ = false;
                hasTarget_ = false; phase_ = Phase::Scan; scanCursor_ = 0; bestDist2_ = 1e18;
                releaseInputs(); return;
            }
        } else if (targetNow.empty() || localPart(targetNow) != targetId_) {
            note("target vanished; rescan");
            hasTarget_ = false; phase_ = Phase::Scan; scanCursor_ = 0; bestDist2_ = 1e18;
            releaseInputs(); return;
        }
        const bedrock::Vec3 eye{feet.x, feet.y + 1.62f, feet.z};
        const bedrock::Vec3 center{target_.x + .5f, target_.y + .5f, target_.z + .5f};
        const double dx = center.x - eye.x, dy = center.y - eye.y, dz = center.z - eye.z;
        const double dist = std::sqrt(dx * dx + dy * dy + dz * dz);
        // Tool first: if we will need a tool we cannot provide, stop early.
        if (!navReturn_ && !ensureTool(targetId_)) { enterNeedTool(); return; }
        if (dist <= 4.0) {
            pressKey(kKeyW, false); pressKey(kKeySpace, false);
            if (navReturn_) {
                navReturn_ = false; hasTarget_ = false;
                chainWaitingCobble_ = false;
                phase_ = Phase::Craft; chainStepTicks_ = 0; chainAimedTick_ = -1;
                note("craft chain: back at the table, resuming");
                return;
            }
            phase_ = Phase::Mine; mineTicks_ = 0;
            return;
        }
        // Planned route (A*): plan once per target and follow the waypoints
        // when a route exists. No route (or a failed plan) falls back to
        // the legacy straight-line navigation below. A failed plan is
        // retried at most every 200 ticks: re-running a 9,000-expansion
        // search every tick starved the fallback navigation entirely
        // (measured 2026-11-17: a deepslate target looped "no route"
        // for 15 minutes without the 45 s navigate timeout ever
        // accumulating).
        if (!(pathFor_ == target_)) {
            if (!(planFailFor_ == target_) || tickCount_ - planFailTick_ >= 200) {
                if (!planPath(feet) && path_.empty()) {
                    planFailFor_ = target_;
                    planFailTick_ = tickCount_;
                }
            }
        }
        if (!path_.empty()) { tickFollowPath(feet); return; }
        // No land route and open water ahead on a long crossing: craft a
        // boat here on the shore while there is still ground to work on
        // (one attempt per target; failure falls through to swimming).
        if (!navReturn_ && !inWaterNow(feet) && boatSlot() < 0 && dist > 10.0 &&
            !(boatChainTriedFor_ == target_) &&
            routeCrossesWater(feet, center) && tryStartBoatChain()) {
            tickCraft(feet); return;
        }
        // Swimming: no land route — cross the water at the surface.
        // Travel happens only with the head at the surface; whenever
        // the head is submerged, surfacing comes first. If a ceiling
        // blocks the way up (swam under an overhang — a real drowning
        // trap observed in verification), back out from under it while
        // rising, and strafe along it if backing out takes too long.
        if (inWaterNow(feet)) {
            if (!wasInWater_) note("swimming to target");
            wasInWater_ = true;
            // A long crossing with a boat in the hotbar: ride instead.
            if (boatSlot() >= 0 && dist > 10.0 && !(boatTriedFor_ == target_)) {
                enterBoat(); tickBoat(feet); return;
            }
            faceAim(eye, {center.x, eye.y, center.z}, 10.f, 0.25f);
            if (headInWater(feet)) {
                ++swimSubmerged_;
                pressKey(kKeyW, false);
                pressKey(kKeySpace, true);
                const auto above = blockAt({static_cast<int>(std::floor(feet.x)),
                                            static_cast<int>(std::floor(feet.y + 2.62f)),
                                            static_cast<int>(std::floor(feet.z))});
                const bool ceiling = !above.empty() && !isAir(above) &&
                                     !contains(localPart(above), "water");
                pressKey(kKeyS, ceiling);
                pressKey(kKeyD, ceiling && swimSubmerged_ > 20 * 6);
            } else {
                swimSubmerged_ = 0;
                pressKey(kKeyS, false); pressKey(kKeyD, false);
                pressKey(kKeyW, true); pressKey(kKeySpace, true);
            }
            ++navTicks_;
            if (navTicks_ % 10 == 0) {
                const double moved = std::hypot(feet.x - lastX_, feet.z - lastZ_);
                if (moved < 0.12) {
                    if (++stuckTicks_ >= 10) {
                        note("swim stuck; blacklist target");
                        blacklist_.insert(target_); hasTarget_ = false;
                        pressKey(kKeyW, false); pressKey(kKeySpace, false);
                        phase_ = Phase::Scan; scanCursor_ = 0; bestDist2_ = 1e18;
                        return;
                    }
                } else stuckTicks_ = 0;
                lastX_ = feet.x; lastZ_ = feet.z;
            }
            if (navTicks_ > 20 * 45) {
                note("navigate timeout (swim); blacklist target");
                if (chainWaitingCobble_) ++cobbleNavFails_;
                blacklist_.insert(target_); hasTarget_ = false;
                pressKey(kKeyW, false); pressKey(kKeySpace, false);
                phase_ = Phase::Scan; scanCursor_ = 0; bestDist2_ = 1e18;
            }
            return;
        }
        if (wasInWater_) { // reached land: re-plan from the shore
            wasInWater_ = false; swimSubmerged_ = 0; surfacing_ = false;
            pressKey(kKeyS, false); pressKey(kKeyD, false);
            pathFor_ = {INT_MIN, 0, 0};
        }
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
                // A drop that lands in water is a swim entry, not a
                // cliff: walk in and let the swim branch take over.
                // The first non-air cell down the column decides (the
                // sea surface can be far below a high shore).
                bool waterBelow = false;
                for (int y = fy - 1; y >= fy - 24; --y) {
                    const auto id = blockAt({fx, y, fz});
                    if (id.empty()) break;
                    if (contains(localPart(id), "water")) { waterBelow = true; break; }
                    if (!isAir(id)) break;
                }
                if (!waterBelow) {
                    pressKey(kKeyW, false);
                    note("cliff guard: target unreachable on foot");
                    blacklist_.insert(target_); hasTarget_ = false;
                    phase_ = Phase::Scan; scanCursor_ = 0; bestDist2_ = 1e18;
                    return;
                }
            }
        }
        pressKey(kKeyW, true);
        pressKey(kKeySprint, dist > 6.0); // legacy straight-line travel sprints too
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
                        if (chainWaitingCobble_) ++cobbleNavFails_;
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
            if (chainWaitingCobble_) ++cobbleNavFails_;
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
            // Only a genuinely diggable solid can block movement. A
            // no-collision decoration (leaf litter, grass, vines, ...)
            // in the front cell is not the obstacle — targeting it made
            // the mine phase swing at a hitbox that sits at the cell's
            // floor under the aim point, time out, blacklist the goal,
            // and re-pick the same decoration forever (user-reported
            // 2026-11-17: wandering around leaf-littered ground without
            // ever digging forward).
            if (!id.empty() && isDiggableCell(id)) {
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
        pressKey(kKeyW, false);
        // Mining from the water is fine, but stay at the surface:
        // without jump held the player sinks mid-mine and drowns.
        pressKey(kKeySpace, inWaterNow(feet));
        const auto current = blockAt(target_);
        if (current.empty()) { setAttack(false); return; } // chunk hiccup: wait
        if (isAir(current) || localPart(current) != targetId_) {
            setAttack(false);
            ++mined_;
            escapeStreak_ = 0; // real progress: the water patch is workable
            chargeToolUse();
            note("mined " + targetId_ + " total=" + std::to_string(mined_));
            if (ascendDig_) {
                // The ascend's ceiling block is gone: hand control
                // back to the climb. A broken head block is
                // progress, so the climb's stuck timer resets.
                ascendDig_ = false;
                hasTarget_ = false;
                mineTicks_ = 0;
                ascendStuck_ = 0;
                phase_ = Phase::Ascend;
                return;
            }
            if (!savedId_.empty() && pathDigResume_ && !contains(targetId_, "_ore")) {
                // A route cell (not an ore — ore drops detour through
                // Collect as before): restore the goal and resume
                // following the kept path at the same waypoint. The
                // drop falls on the route cell, which the follower is
                // about to walk through anyway.
                pathDigResume_ = false;
                target_ = savedTarget_; targetId_ = savedId_; savedId_.clear();
                mineTicks_ = 0; navTicks_ = 0;
                phase_ = Phase::Navigate;
                return;
            }
            if (!savedId_.empty()) {
                // A leaf occluder drops nothing worth collecting, and
                // the Collect detour forced a full A* replan before the
                // next leaf of the same tree (measured 2026-11-17 on
                // the user's run and in reproduction: one replan per
                // leaf, minutes per tree). Restore the goal and keep
                // mining in place; the occlusion check below picks the
                // next leaf or the goal block itself.
                if (contains(targetId_, "leaves")) {
                    target_ = savedTarget_; targetId_ = savedId_; savedId_.clear();
                    mineTicks_ = 0;
                    return;
                }
                // Mine-through block cleared: its drop is worth as much
                // as a direct target's (measured 2026-11-17: 48 diamond
                // ore broken but only 3 diamonds held, the rest were
                // occluders whose drops were abandoned by going
                // straight back to Scan). Collect first, restore the
                // goal target when Collect finishes.
                phase_ = Phase::Collect; collectTicks_ = 0;
                return;
            }
            phase_ = Phase::Collect; collectTicks_ = 0;
            return;
        }
        if (!ensureTool(current)) { setAttack(false); enterNeedTool(); return; }
        const bedrock::Vec3 eye{feet.x, feet.y + 1.62f, feet.z};
        const bedrock::Vec3 center{target_.x + .5f, target_.y + .5f, target_.z + .5f};
        const double dx = center.x - eye.x, dy = center.y - eye.y, dz = center.z - eye.z;
        if (std::sqrt(dx * dx + dy * dy + dz * dz) > 4.4) {
            setAttack(false);
            if (ascendDig_) { ascendDig_ = false; hasTarget_ = false; mineTicks_ = 0; phase_ = Phase::Ascend; return; }
            phase_ = Phase::Navigate; return;
        }
        // Occlusion: if another solid block blocks the ray to the target, mine
        // that block first (dig-through), then resume the original target.
        {
            BlockPos3 occluder;
            if (!lineOfSight(eye, target_, occluder)) {
                const auto occId = blockAt(occluder);
                if (chainTablePlaced_ && occluder.x == chainTablePos_.x &&
                    occluder.y == chainTablePos_.y && occluder.z == chainTablePos_.z) {
                    // The only thing between us and the target is the
                    // chain's own crafting table: abandon this target
                    // rather than break the table.
                    note("target occluded by the crafting table; rescan");
                    setAttack(false);
                    pathDigResume_ = false;
                    blacklist_.insert(savedId_.empty() ? target_ : savedTarget_);
                    if (!savedId_.empty()) { targetId_ = savedId_; savedId_.clear(); }
                    hasTarget_ = false;
                    phase_ = Phase::Scan; scanCursor_ = 0; bestDist2_ = 1e18;
                    return;
                }
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
            if (ascendDig_) {
                // Do not blacklist the ceiling cell (the climb
                // still needs it gone); charge the attempt to the
                // climb's stuck budget so two failed 12 s digs
                // give the climb up instead of looping forever.
                note("ascend dig: mine timeout, back to ascend");
                ascendDig_ = false;
                setAttack(false);
                hasTarget_ = false;
                mineTicks_ = 0;
                ascendStuck_ += 20 * 12;
                phase_ = Phase::Ascend;
                return;
            }
            note("mine timeout; blacklist");
            pathDigResume_ = false;
            setAttack(false);
            // In mine-through mode target_ is the occluder cell, not the
            // goal: blacklist the goal cell (same rule as the crafting
            // table occlusion branch), or the scan re-picks the same
            // unreachable goal forever (measured 2026-11-17 on a vine-
            // occluded jungle log: 120 timeouts on one cell).
            blacklist_.insert(savedId_.empty() ? target_ : savedTarget_); hasTarget_ = false;
            if (!savedId_.empty()) { targetId_ = savedId_; savedId_.clear(); }
            phase_ = Phase::Scan; scanCursor_ = 0; bestDist2_ = 1e18;
        }
    }

    void tickCollect(bedrock::Vec3 feet) {
        // Step onto the mined cell briefly to pick the drop up, when safe.
        const auto below = blockAt({target_.x, target_.y - 1, target_.z});
        const auto cell = blockAt(target_);
        if (collectTicks_ == 0 && (below.empty() || isAir(below) || !isAir(cell))) {
            restoreMineThroughGoal();
            phase_ = Phase::Scan; scanCursor_ = 0; bestDist2_ = 1e18; hasTarget_ = false; return;
        }
        const bedrock::Vec3 eye{feet.x, feet.y + 1.62f, feet.z};
        const bedrock::Vec3 center{target_.x + .5f, feet.y + .5f, target_.z + .5f};
        faceAim(eye, center, 12.f, 0.3f);
        pressKey(kKeyW, true);
        pressKey(kKeySpace, inWaterNow(feet)); // swim at the surface to the drop
        if (++collectTicks_ >= 7) {
            pressKey(kKeyW, false);
            restoreMineThroughGoal();
            hasTarget_ = false;
            phase_ = Phase::Scan; scanCursor_ = 0; bestDist2_ = 1e18;
        }
    }
    // After a mine-through Collect, restore the goal target the
    // occlusion detour had stashed (no-op for direct targets).
    // Same restore as the Escape path: cell and id together.
    void restoreMineThroughGoal() {
        pathDigResume_ = false; // any Collect/Escape exit ends the route resume
        if (!savedId_.empty()) {
            target_ = savedTarget_; targetId_ = savedId_; savedId_.clear();
        }
    }

    void enterNeedTool() {
        releaseInputs();
        phase_ = Phase::NeedTool; needTicks_ = 0;
        needTier_ = requiredTier(targetId_);
        if (classForBlock(targetId_) == ToolClass::Pickaxe) {
            if (needTier_ >= 4) {
                // The block drops nothing without a high-tier pickaxe
                // (diamond ore needs iron+): never swing a weaker tool
                // at it. Craft the required pickaxe when its material
                // is on hand; otherwise say exactly what is missing.
                const bool diamondTier = needTier_ >= 5;
                const int mats = invCountOf(diamondTier ? "diamond" : "iron_ingot");
                const std::string tool = diamondTier ? "ダイヤのツルハシ" : "鉄のツルハシ";
                const std::string mat = diamondTier ? "ダイヤモンド" : "鉄インゴット";
                reply("この鉱石は" + tool + "以上じゃないと掘れないよ。" +
                      (mats >= 3 ? mat + "があるから作るね"
                                 : mat + "が3個必要だよ（今" + std::to_string(mats) + "個）"));
                note("needtool: tier " + std::to_string(needTier_) + " for " + targetId_ +
                     ", " + mat + "=" + std::to_string(mats));
                wantPickaxeChain_ = true;
                tryStartPickaxeChain();
                return;
            }
            const auto plan = planPickaxe(visibleItems());
            reply("ツルハシがホットバーに無いよ。" + plan.summary);
            note("needtool: " + plan.summary + " possible=" + std::to_string(plan.possible));
            if (plan.possible) { wantPickaxeChain_ = true; tryStartPickaxeChain(); }
        } else {
            reply("このブロック用のツールがホットバーに無いよ");
        }
    }
    void tickNeedTool(bedrock::Vec3 feet) {
        if (moveStage_ != MoveStage::None) { (void)ensureHotbarTick(moveItem_); return; }
        if (++needTicks_ % 20) return;
        if (needTicks_ % 200 == 20) {
            // Diagnostic (2026-11-18): a crafted pickaxe sat in the
            // inventory server-side while NeedTool waited forever —
            // dump what the adapter actually sees plus the latches
            // that gate the fetch branch below.
            std::string snap;
            for (const auto& s : craft_.snapshot()) {
                if (!s.count) continue;
                snap += " " + std::to_string(s.slot) + ":" + localPart(s.item) + "x" +
                        std::to_string(s.count);
            }
            note("needtool diag: tier=" + std::to_string(needTier_) +
                 " moveFailed=" + std::to_string(moveFailed_) +
                 " moveStage=" + std::to_string(static_cast<int>(moveStage_)) +
                 " wantChain=" + std::to_string(wantPickaxeChain_) +
                 " chainActive=" + std::to_string(chainActive_) + " snap:" + snap);
        }
        // Hopeless underground: no pickaxe of the needed tier in
        // hand or in reserve, and the craft chain cannot start (no
        // materials). Instead of waiting forever at the bottom,
        // climb back to the surface the expedition descended from
        // and rebuild there — provided enough spare blocks remain
        // to pillar up with.
        if (expeditionActive() && needTier_ >= 2) {
            const int surfY = lastSurfaceY_ == INT_MAX ? 70 : lastSurfaceY_;
            if (static_cast<int>(std::floor(feet.y)) < surfY - 8 &&
                !hasPickaxeTier(needTier_) && !hasSparePickaxe(needTier_)) {
                if (wantPickaxeChain_ && tryStartPickaxeChain()) return; // chain path taken
                const int climb = surfY - static_cast<int>(std::floor(feet.y));
                if (totalSpareCount() >= climb) {
                    releaseInputs();
                    phase_ = Phase::Ascend; ascendTicks_ = 0;
                    ascendLastY_ = INT_MAX; ascendStuck_ = 0; ascendPullTicks_ = 0;
                    ascendTargetY_ = surfY;
                    ascendOpenSky_ = lastSurfaceY_ == INT_MAX;
                    ascendNoteY_ = INT_MIN;
                    ascendDig_ = false;
                    ascendCenterActive_ = false; ascendCenterGiveUp_ = false;
                    reply("道具が作れないから、地上に戻って仕切り直すね");
                    note("ascend: returning to surface y=" + std::to_string(surfY));
                    return;
                }
            }
        }
        if (wantPickaxeChain_ && tickCount_ >= chainNextRetryTick_ &&
            tryStartPickaxeChain())
            return;
        if (maybeStartExpedition()) return;
        // A good-enough pickaxe that only exists in the main inventory
        // (deposited there while the hotbar was full) is invisible to
        // ensureTool: move it into the hotbar first.
        if (needTier_ > 0 && !moveFailed_) {
            bool hotbarHas = false;
            std::string mainPick;
            int mainTier = -1;
            for (const auto& s : craft_.snapshot()) {
                if (!s.count) continue;
                int t{};
                if (classForItem(localPart(s.item), t) != ToolClass::Pickaxe || t < needTier_)
                    continue;
                if (s.slot < 9) hotbarHas = true;
                else if (t > mainTier) { mainTier = t; mainPick = localPart(s.item); }
            }
            if (!hotbarHas && !mainPick.empty()) {
                (void)ensureHotbarTick(mainPick);
                return;
            }
        }
        if (ensureTool(targetId_)) {
            reply("ツールを確認。再開するね");
            phase_ = Phase::Scan; scanCursor_ = 0; bestDist2_ = 1e18; hasTarget_ = false;
        }
    }

    // -- AutoCraft chain -------------------------------------------------------
    // Real crafting through the NativeItemStackRequests adapter: a queue of
    // small steps (adapter craft jobs, placing/opening a crafting table,
    // closing the UI) executed by the Craft phase. Built for the pickaxe
    // chain (NeedTool) and for boats (long water crossings). Any failure
    // falls back to the pre-AutoCraft behaviour: NeedTool waits for a manual
    // tool, navigation swims.
    struct ChainStep {
        enum class Kind { Craft, PlaceTable, OpenTable, CloseUi, WaitCobble, OpenInventory };
        Kind kind{Kind::Craft};
        std::string recipeId, outputName;
        int times{1};
        bool tableGrid{};
    };
    std::vector<ChainStep> chainSteps_;
    std::size_t chainStep_{};
    bool chainActive_{};
    bool chainJobStarted_{};
    int chainStepTicks_{};
    int chainAimedTick_{-1};
    bool chainRetriedOpen_{};
    int chainRecoverTicks_{-1};
    std::string chainPendingFail_;
    Phase chainReturn_{Phase::Scan};
    bool chainForPickaxe_{};
    bool chainForFurnace_{};
    bool wantPickaxeChain_{};
    int needTier_{};
    long long chainNextRetryTick_{};
    int chainRetries_{};
    bool chainWaitingCobble_{};
    bool navReturn_{};
    int chainReturnTrips_{};
    int chainSubmerged_{};
    int chainCobbleTicks_{};
    BlockPos3 chainTablePos_{INT_MIN, 0, 0};
    bool chainTablePlaced_{};
    BlockPos3 chainTableBad_{INT_MIN, 0, 0};
    long long chainTableBadUntil_{};
    BlockPos3 chainPlaceCell_{INT_MIN, 0, 0};
    BlockPos3 chainPocket_{INT_MIN, 0, 0};
    int chainPocketBad_{};
    int chainPocketStage_{};
    bool chainPocketing_{};
    bool chainPocketDug_{};
    BlockPos3 chainPocketCell_{INT_MIN, 0, 0};
    int chainNoSlotTicks_{};
    bool chainPlaceArmed_{};
    std::size_t chainPlaceSlotIdx_{};
    int chainPlaceSlotTicks_{};
    int chainNoSlotRetries_{};
    std::vector<BlockPos3> chainPlaceCells_;
    std::size_t chainPlaceIdx_{};
    int placeCellTicks_{};
    BlockPos3 boatChainTriedFor_{INT_MIN, 0, 0};

    // Does the straight route toward the target cross water within the
    // next few blocks? Used to start the boat chain while still on land.
    bool routeCrossesWater(const bedrock::Vec3& feet, const bedrock::Vec3& center) {
        const double dx = center.x - feet.x, dz = center.z - feet.z;
        const double len = std::hypot(dx, dz);
        if (len < 6.0) return false;
        const int by = static_cast<int>(std::floor(feet.y));
        for (double d = 2.0; d <= std::min(len, 9.0); d += 1.5) {
            const int bx = static_cast<int>(std::floor(feet.x + dx / len * d));
            const int bz = static_cast<int>(std::floor(feet.z + dz / len * d));
            if (contains(localPart(blockAt({bx, by, bz})), "water") ||
                contains(localPart(blockAt({bx, by - 1, bz})), "water"))
                return true;
        }
        return false;
    }
    int countAllItems(std::string_view key) {
        int total = 0;
        if (craftReady_)
            for (const auto& s : craft_.snapshot())
                if (contains(localPart(s.item), key)) total += s.count;
        return total;
    }
    // Item counts over ALL 36 slots from the adapter snapshot (its
    // response-tracked overlay keeps forged-craft results visible, which
    // the plain memory view would miss until a relog).
    std::unordered_map<std::string, int> chainCounts() {
        std::unordered_map<std::string, int> have;
        if (craftReady_)
            for (const auto& s : craft_.snapshot())
                if (s.count > 0 && !s.item.empty()) have[localPart(s.item)] += s.count;
        return have;
    }
    static int countIn(const std::unordered_map<std::string, int>& items, std::string_view key) {
        int total = 0;
        for (const auto& [id, count] : items) if (contains(id, key)) total += count;
        return total;
    }
    bool placedTableExists() {
        if (chainTablePlaced_ &&
            contains(localPart(blockAt(chainTablePos_)), "crafting_table") &&
            !tableBanned(chainTablePos_)) {
            // The remembered table may be far away (placed for an
            // earlier chain in another area): its screen cannot be
            // opened from here, so for chain purposes it does not
            // exist and a new one must be placed nearby.
            bedrock::Vec3 feet;
            if (!feetPos(feet)) return true;
            const double dx = chainTablePos_.x + .5 - feet.x;
            const double dy = chainTablePos_.y + .5 - feet.y;
            const double dz = chainTablePos_.z + .5 - feet.z;
            if (dx * dx + dy * dy + dz * dz <= 36.0) return true;
        }
        // Widened reuse (2026-11-18): the remembered table is not the
        // only one in the world. Every chain used to remember just its
        // own latest table, so a new craft site a few blocks from an
        // OLD table still placed a fresh bench — the pile of tables
        // the user spotted in a photo. Adopt ANY placed table within
        // opening reach (the same 6-block gate) that is not banned.
        bedrock::Vec3 feet;
        if (!feetPos(feet)) return false;
        const int fx = static_cast<int>(std::floor(feet.x));
        const int fy = static_cast<int>(std::floor(feet.y));
        const int fz = static_cast<int>(std::floor(feet.z));
        BlockPos3 best{INT_MIN, 0, 0};
        double bestD2 = 36.0;
        for (int dx = -6; dx <= 6; ++dx)
            for (int dy = -2; dy <= 2; ++dy)
                for (int dz = -6; dz <= 6; ++dz) {
                    const double d2 = static_cast<double>(dx * dx + dy * dy + dz * dz);
                    if (d2 > bestD2) continue;
                    const BlockPos3 c{fx + dx, fy + dy, fz + dz};
                    if (tableBanned(c)) continue;
                    if (!contains(localPart(blockAt(c)), "crafting_table")) continue;
                    best = c; bestD2 = d2;
                }
        if (best.x == INT_MIN) return false;
        chainTablePos_ = best; chainTablePlaced_ = true;
        note("chain: reusing placed table at " + std::to_string(best.x) + "," +
             std::to_string(best.y) + "," + std::to_string(best.z));
        return true;
    }
    int findTableHotbarSlot() {
        const auto slots = hotbarItems();
        for (int i = 0; i < 9; ++i) {
            const auto& it = slots[static_cast<std::size_t>(i)];
            if (it.count && localPart(it.id) == "crafting_table") return i;
        }
        return -1;
    }
    // -- hotbar move -----------------------------------------------------
    // A crafted table/furnace/pickaxe is deposited into the first free
    // tracked inventory slot, which is in the MAIN inventory whenever
    // the hotbar is full of gathered junk — and placement/tool use only
    // see the hotbar (measured 2026-11-16: the chain crafted a table,
    // deposited it at main slot 12+, and PlaceTable failed in a retry
    // loop with "not in hotbar"). Move the item into the hotbar with
    // the adapter's one-shot transfer, evicting a junk stack first if
    // every hotbar slot is taken. The transfers only succeed while a
    // screen is attached (a no-screen place into the inventory is
    // rejected with status 49, measured 2026-11-16), so the move opens
    // the inventory screen first and uses the section addressing the
    // open screen expects (hotbar = container 28, main = 29), then
    // closes the screen again. Returns true when the item is in the
    // hotbar; false while the move runs or after giving up
    // (moveFailed_ distinguishes, caller resets it).
    enum class MoveStage { None, OpenInv, Evict, Bring, CloseUi };
    MoveStage moveStage_{MoveStage::None};
    std::string moveItem_; // full id being moved
    int moveSrc_{-1}, moveDst_{-1}, moveCount_{};
    std::int32_t moveSrcNet_{};
    int moveEvict_{-1}, moveEvictCount_{}, moveFreeMain_{-1};
    std::int32_t moveEvictNet_{};
    std::string moveEvictId_; // full id being evicted
    bool moveNeedEvict_{};
    int moveAttempts_{};
    int moveScreenOkTicks_{-1};
    bool moveReplan_{};
    bool moveEvictDone_{};
    bool moveFailPending_{};
    long long moveStageStart_{};
    long long moveDeadline_{};
    bool moveFailed_{};
    // The hotbar move always works against the personal inventory
    // screen it opens with E, whose inventory is container 12 with
    // absolute slots 0..35 (the same addressing the craft jobs use).
    // The old 28/29 section ids belong to the furnace/table screens:
    // with only the personal screen open, every move transfer was
    // rejected with status 49 (measured 2026-11-17: the stone
    // pickaxe could not be brought into the hotbar mid-hunt and the
    // iron stage stalled in a retry loop).
    static std::uint32_t invContainer(int) { return 12u; }
    bool ensureHotbarTick(const std::string& localId) {
        const long long stageTicks = tickCount_ - moveStageStart_;
        switch (moveStage_) {
        case MoveStage::None: {
            const auto snap = craft_.snapshot();
            bool occupied[36]{};
            for (const auto& s : snap)
                if (s.count > 0 && s.slot >= 0 && s.slot < 36) occupied[s.slot] = true;
            // "Already in the hotbar" must be visible to the memory
            // hotbar read the placement code uses (hotbarItems()): a
            // craft result can be deposited into a hotbar slot the
            // memory read still shows empty (shadow-only) — trusting
            // the snapshot there stranded PlaceTable on fresh worlds
            // (measured 2026-11-17) — and selecting that slot would
            // wipe the stack. A shadow-only hotbar stack is a move
            // SOURCE instead: a forged transfer moves it server-side
            // regardless of what the memory read shows.
            const auto hb = hotbarItems();
            for (int i = 0; i < 9 && i < static_cast<int>(hb.size()); ++i)
                if (hb[static_cast<std::size_t>(i)].count > 0 &&
                    localPart(hb[static_cast<std::size_t>(i)].id) == localId)
                    return true;
            int src = -1, srcCount = 0;
            std::int32_t srcNet = 0;
            std::string fullId;
            for (const auto& s : snap)
                if (s.count > 0 && s.slot >= 9 && localPart(s.item) == localId) {
                    src = s.slot; srcCount = s.count; srcNet = s.netId; fullId = s.item;
                    break;
                }
            if (src < 0)
                for (const auto& s : snap)
                    if (s.count > 0 && s.slot >= 0 && s.slot < 9 &&
                        localPart(s.item) == localId &&
                        !(s.slot < static_cast<int>(hb.size()) &&
                          hb[static_cast<std::size_t>(s.slot)].count > 0 &&
                          localPart(hb[static_cast<std::size_t>(s.slot)].id) == localId)) {
                        src = s.slot; srcCount = s.count; srcNet = s.netId; fullId = s.item;
                        break;
                    }
            if (src < 0) { moveFailed_ = true; return false; }
            moveItem_ = fullId; moveSrc_ = src; moveSrcNet_ = srcNet; moveCount_ = srcCount;
            moveNeedEvict_ = false; moveFailPending_ = false;
            if (!moveReplan_) moveAttempts_ = 0; // retries keep their budget
            moveReplan_ = false;
            moveEvictDone_ = false;
            int dst = -1;
            for (int i = 0; i < 9; ++i)
                if (!occupied[i]) { dst = i; break; }
            if (dst >= 0) {
                moveDst_ = dst;
                note("hotbar move: " + localId + " from main slot " + std::to_string(src) +
                     " to hotbar " + std::to_string(dst));
            } else {
                // Hotbar full: evict the least valuable non-tool stack
                // to a free main-inventory slot first.
                int evict = -1, evictScore = -1, evictCount = 0;
                std::int32_t evictNet = 0;
                std::string evictId;
                for (const auto& s : snap) {
                    if (s.count <= 0 || s.slot >= 9) continue;
                    const std::string id = localPart(s.item);
                    if (id == localId || id == "crafting_table" || id == "furnace") continue;
                    if (contains(id, "sword") || id == "bow") continue;
                    int t{};
                    if (classForItem(id, t) != ToolClass::None) continue;
                    int score = 10;
                    if (contains(id, "sapling")) score = 100;
                    else if (contains(id, "seed")) score = 90;
                    else if (id == "apple") score = 80;
                    else if (contains(id, "sand")) score = 70;
                    else if (contains(id, "dirt")) score = 60;
                    else if (contains(id, "gravel")) score = 50;
                    else if (contains(id, "flower")) score = 40;
                    if (score > evictScore) {
                        evictScore = score; evict = s.slot; evictCount = s.count;
                        evictNet = s.netId; evictId = s.item;
                    }
                }
                int freeMain = -1;
                for (int i = 9; i < 36; ++i)
                    if (!occupied[i]) { freeMain = i; break; }
                if (evict < 0 || freeMain < 0) { moveFailed_ = true; return false; }
                moveDst_ = evict;
                moveEvict_ = evict; moveEvictCount_ = evictCount;
                moveEvictNet_ = evictNet; moveEvictId_ = evictId;
                moveFreeMain_ = freeMain;
                moveNeedEvict_ = true;
                note("hotbar move: evicting " + localPart(evictId) + " from hotbar " +
                     std::to_string(evict) + " for " + localId);
            }
            moveStage_ = MoveStage::OpenInv;
            moveStageStart_ = tickCount_;
            return false;
        }
        case MoveStage::OpenInv: {
            // Dispatch only against a VERIFIED inventory screen. The
            // gameplay-active event does not reliably flip for this
            // screen (measured 2026-11-17 on fresh3: the E pulse
            // visibly opened the inventory, yet the event stayed
            // "active" for the whole 300-tick wait), and a blind
            // warmup still raced the real client (user's run
            // 2026-11-17: a stone-pickaxe move failed 3x status 49
            // underground). The top screen name is ground truth: E
            // until it reads as an inventory screen (Esc first when a
            // foreign screen is on top), a short settle, then send.
            // Without the screen helper, fall back to the old blind
            // 25-tick warmup path below.
            if (screenReady_) {
                const std::string screen = topScreenName();
                if (contains(screen, "inventory")) {
                    if (moveScreenOkTicks_ < 0) moveScreenOkTicks_ = 0;
                    if (++moveScreenOkTicks_ < 10) return false;
                    const bool evicting = moveNeedEvict_ && !moveEvictDone_;
                    const bool ok = evicting
                        ? craft_.startTransfer(invContainer(moveEvict_), moveEvict_, moveEvictNet_,
                                               invContainer(moveFreeMain_), moveFreeMain_,
                                               moveEvictCount_, moveEvictId_)
                        : craft_.startTransfer(invContainer(moveSrc_), moveSrc_, moveSrcNet_,
                                               invContainer(moveDst_), moveDst_,
                                               moveCount_, moveItem_);
                    if (ok) {
                        moveStage_ = evicting ? MoveStage::Evict : MoveStage::Bring;
                        moveDeadline_ = tickCount_ + 20 * 15;
                        moveScreenOkTicks_ = -1;
                        return false;
                    }
                    if (stageTicks > 400) {
                        note("hotbar move: transfer never dispatched");
                        moveFailed_ = true; moveStage_ = MoveStage::None;
                    }
                    return false;
                }
                moveScreenOkTicks_ = -1;
                if (!screen.empty() && screen != "hud_screen") {
                    if ((stageTicks % 45) == 1) pressKey(0x1B, true);
                    if ((stageTicks % 45) == 3) pressKey(0x1B, false);
                } else {
                    if ((stageTicks % 45) == 1) pressKey(0x45, true);  // VK_E
                    if ((stageTicks % 45) == 3) pressKey(0x45, false);
                }
                if (stageTicks > 400) {
                    note("hotbar move: inventory screen did not open (top: " + screen + ")");
                    moveFailed_ = true; moveStage_ = MoveStage::None;
                }
                return false;
            }
            if (stageTicks >= 25) {
                const bool evicting = moveNeedEvict_ && !moveEvictDone_;
                const bool ok = evicting
                    ? craft_.startTransfer(invContainer(moveEvict_), moveEvict_, moveEvictNet_,
                                           invContainer(moveFreeMain_), moveFreeMain_,
                                           moveEvictCount_, moveEvictId_)
                    : craft_.startTransfer(invContainer(moveSrc_), moveSrc_, moveSrcNet_,
                                           invContainer(moveDst_), moveDst_,
                                           moveCount_, moveItem_);
                if (ok) {
                    moveStage_ = evicting ? MoveStage::Evict : MoveStage::Bring;
                    moveDeadline_ = tickCount_ + 20 * 15;
                    return false;
                }
                // Adapter gate busy (an earlier transfer is still
                // resolving): keep waiting rather than failing.
                if (stageTicks > 300) {
                    note("hotbar move: transfer never dispatched");
                    moveFailed_ = true; moveStage_ = MoveStage::None;
                }
                return false;
            }
            // Pulse E only on the first attempt: on a retry the
            // screen may still be open from the failed attempt, and
            // E would close it again.
            if (moveAttempts_ == 0 && (stageTicks % 45) == 1) pressKey(0x45, true);  // VK_E
            if (moveAttempts_ == 0 && (stageTicks % 45) == 3) pressKey(0x45, false);
            if (stageTicks > 300) {
                note("hotbar move: inventory screen did not open");
                moveFailed_ = true; moveStage_ = MoveStage::None;
            }
            return false;
        }
        case MoveStage::Evict:
        case MoveStage::Bring: {
            if (tickCount_ <= moveDeadline_) {
                const auto st = craft_.transferStatus();
                if (st == bedrock::CraftJobStatus::Running) return false;
                craft_.clearTransferStatus();
                if (st != bedrock::CraftJobStatus::Failed) {
                    if (moveStage_ == MoveStage::Evict) {
                        moveEvictDone_ = true;
                        if (craft_.startTransfer(invContainer(moveSrc_), moveSrc_, moveSrcNet_,
                                                 invContainer(moveDst_), moveDst_,
                                                 moveCount_, moveItem_)) {
                            moveStage_ = MoveStage::Bring;
                            moveDeadline_ = tickCount_ + 20 * 15;
                            return false;
                        }
                    } else {
                        moveStage_ = MoveStage::CloseUi;
                        moveStageStart_ = tickCount_;
                        return false;
                    }
                }
                note("hotbar move: transfer failed");
            } else {
                note("hotbar move: transfer timed out");
            }
            if (moveAttempts_ < 2) {
                ++moveAttempts_;
                // Re-plan from a fresh snapshot instead of resending
                // the identical transfer: a stale plan (the source
                // stack moved, the destination filled meanwhile)
                // fails identically forever — the user's log showed
                // the same status-49 transfer three times in a row.
                note("hotbar move: retrying with a fresh plan");
                moveStage_ = MoveStage::None;
                moveReplan_ = true;
                moveStageStart_ = tickCount_;
                return false;
            }
            moveFailPending_ = true;
            moveStage_ = MoveStage::CloseUi;
            moveStageStart_ = tickCount_;
            return false;
        }
        case MoveStage::CloseUi: {
            // Verified close when the screen name is readable: the
            // gameplay-active event alone has lied in both directions.
            bool closed = game_.gameplayActive();
            if (screenReady_) {
                const std::string screen = topScreenName();
                closed = screen.empty() || screen == "hud_screen";
            }
            if (closed || stageTicks > 70) {
                moveStage_ = MoveStage::None;
                if (moveFailPending_) {
                    moveFailPending_ = false; moveFailed_ = true;
                    return false;
                }
                note("hotbar move: done");
                return true;
            }
            if (stageTicks == 1 || stageTicks == 30) pressKey(0x1B, true);
            if (stageTicks == 3 || stageTicks == 32) pressKey(0x1B, false);
            return false;
        }
        }
        return false;
    }
    // The wood ("oak" of oak_log) carrying the most logs, for per-wood
    // planks recipes.
    static std::string chainWood(const std::unordered_map<std::string, int>& items, int& logCount) {
        std::string best;
        logCount = 0;
        for (const auto& [id, count] : items) {
            std::string wood;
            for (const char* suffix : {"_log", "_wood", "_stem", "_hyphae"})
                if (id.size() > std::string_view(suffix).size() &&
                    id.compare(id.size() - std::string_view(suffix).size(),
                               std::string_view(suffix).size(), suffix) == 0)
                    wood = id.substr(0, id.size() - std::string_view(suffix).size());
            if (!wood.empty() && count > logCount) { logCount = count; best = wood; }
        }
        return best;
    }
    bool tryStartPickaxeChain(bool force = false) {
        if (!craftReady_ || !craft_.recipesReady() || !craft_.ready() || chainActive_) return false;
        if (craft_.status() == bedrock::CraftJobStatus::Running) return false; // retry later
        craft_.clearStatus();
        const auto have = chainCounts();
        const int planks = countIn(have, "planks");
        const int sticks = countIn(have, "stick");
        const int tables = countIn(have, "crafting_table");
        const int cobble = countIn(have, "cobblestone");
        // The chain's goal is set by what the current target demands
        // (needTier_, from requiredTier at NeedTool entry): the classic
        // route tops out at a stone pickaxe, but diamond-tier ores need
        // iron+ and obsidian needs diamond — build exactly the pickaxe
        // the job requires, skipping the lower stages when the material
        // for the required one is already on hand.
        const int goalTier = needTier_ >= 4 ? needTier_ : 2;
        int bestPickTier = 0;
        for (const auto& [id, count] : have) {
            if (!count || !contains(id, "_pickaxe")) continue;
            int t{};
            if (classForItem(id, t) == ToolClass::Pickaxe) bestPickTier = std::max(bestPickTier, t);
        }
        if (bestPickTier >= goalTier && !force)
            return false; // a usable pickaxe exists somewhere; not our problem
        auto exactIn = [&](const char* id) {
            const auto it = have.find(id);
            return it == have.end() ? 0 : it->second;
        };
        if (goalTier == 4 && exactIn("iron_ingot") < 3) return false;
        if (goalTier == 5 && exactIn("diamond") < 3) return false;
        int logCount = 0;
        const std::string wood = chainWood(have, logCount);
        std::vector<ChainStep> steps;
        const int plankNeed = (goalTier >= 4 ? 0 : 3) + (sticks < 2 ? 2 : 0) + (tables < 1 ? 4 : 0);
        const int deficit = plankNeed - planks;
        const int logCrafts = deficit > 0 ? (deficit + 3) / 4 : 0;
        if (logCrafts > 0) {
            if (wood.empty() || logCount < logCrafts) return false;
            steps.push_back(ChainStep{ChainStep::Kind::Craft,
                                      "minecraft:" + wood + "_planks",
                                      "minecraft:" + wood + "_planks", logCrafts, false});
        }
        if (sticks < 2)
            steps.push_back(ChainStep{ChainStep::Kind::Craft, "minecraft:stick",
                                      "minecraft:stick", 1, false});
        if (tables < 1)
            steps.push_back(ChainStep{ChainStep::Kind::Craft, "minecraft:WorkBench_recipeId",
                                      "minecraft:crafting_table", 1, false});
        if (!steps.empty()) {
            // Personal-grid crafts need the inventory screen open: the
            // server rejects places into the 2x2 grid (status 49) while no
            // screen is attached (measured 2026-11-16; the real client only
            // ever crafts with a screen open).
            steps.insert(steps.begin(), ChainStep{ChainStep::Kind::OpenInventory});
            steps.push_back(ChainStep{ChainStep::Kind::CloseUi});
        }
        if (!placedTableExists())
            steps.push_back(ChainStep{ChainStep::Kind::PlaceTable});
        if (goalTier >= 4) {
            const std::string pick = goalTier == 4 ? "minecraft:iron_pickaxe"
                                                   : "minecraft:diamond_pickaxe";
            steps.push_back(ChainStep{ChainStep::Kind::OpenTable});
            steps.push_back(ChainStep{ChainStep::Kind::Craft, pick, pick, 1, true});
            steps.push_back(ChainStep{ChainStep::Kind::CloseUi});
        } else {
            steps.push_back(ChainStep{ChainStep::Kind::OpenTable});
            steps.push_back(ChainStep{ChainStep::Kind::Craft, "minecraft:wooden_pickaxe",
                                      "minecraft:wooden_pickaxe", 1, true});
            steps.push_back(ChainStep{ChainStep::Kind::CloseUi});
            if (cobble < 3) steps.push_back(ChainStep{ChainStep::Kind::WaitCobble});
            steps.push_back(ChainStep{ChainStep::Kind::OpenTable});
            steps.push_back(ChainStep{ChainStep::Kind::Craft, "minecraft:stone_pickaxe",
                                      "minecraft:stone_pickaxe", 1, true});
            steps.push_back(ChainStep{ChainStep::Kind::CloseUi});
        }
        chainSteps_ = std::move(steps);
        chainStep_ = 0; chainActive_ = true; chainJobStarted_ = false;
        chainPlaceCell_ = {INT_MIN, 0, 0};
        chainPlaceCells_.clear(); chainPlaceIdx_ = 0; placeCellTicks_ = 0;
        chainPlaceSlotIdx_ = 0; chainPlaceSlotTicks_ = 0;
        chainPocketing_ = false; chainPocketDug_ = false; chainPocketBad_ = 0; chainPocket_ = {INT_MIN, 0, 0};
        chainStepTicks_ = 0; chainAimedTick_ = -1; chainRetriedOpen_ = false;
        chainRecoverTicks_ = -1; chainPendingFail_.clear();
        chainWaitingCobble_ = false;
        chainForPickaxe_ = true; chainReturn_ = Phase::Scan;
        phase_ = Phase::Craft;
        reply(goalTier == 4 ? "鉄のツルハシをクラフトするね"
              : goalTier == 5 ? "ダイヤのツルハシをクラフトするね"
                              : "材料があるから、ツルハシをクラフトするね");
        note("craft chain: pickaxe tier " + std::to_string(goalTier) +
             " (" + std::to_string(chainSteps_.size()) + " steps)");
        return true;
    }
    // Weapon preparation: a stone sword via the same chain
    // machinery, started only when the materials are already on
    // hand (2 cobblestone + 1 stick + a table route).
    bool tryStartSwordChain() {
        if (!craftReady_ || !craft_.recipesReady() || !craft_.ready() || chainActive_) return false;
        if (craft_.status() == bedrock::CraftJobStatus::Running) return false;
        craft_.clearStatus();
        const auto have = chainCounts();
        if (countIn(have, "cobblestone") < 2) return false;
        const int tables = countIn(have, "crafting_table");
        const bool placed = placedTableExists();
        const int sticks = countIn(have, "stick");
        std::vector<ChainStep> steps;
        {
            // Personal-grid prep, mirroring the pickaxe chain: the
            // sword needs a stick, and the stick (and the table, if
            // one is missing) may have to come from planks — and the
            // planks from logs. The old gate silently skipped the
            // whole weapon prep whenever no stick happened to be on
            // hand (measured 2026-11-18: on a natural run no sword
            // was ever crafted, because the pickaxe chain had eaten
            // every stick).
            const int plankNeed = (sticks < 1 ? 2 : 0) + ((tables < 1 && !placed) ? 4 : 0);
            int planks = countIn(have, "planks");
            if (planks < plankNeed) {
                int logCount = 0;
                const std::string wood = chainWood(have, logCount);
                const int logCrafts = (plankNeed - planks + 3) / 4;
                if (wood.empty() || logCount < logCrafts) return false;
                steps.push_back(ChainStep{ChainStep::Kind::Craft,
                                          "minecraft:" + wood + "_planks",
                                          "minecraft:" + wood + "_planks", logCrafts, false});
                planks += logCrafts * 4;
            }
            if (sticks < 1)
                steps.push_back(ChainStep{ChainStep::Kind::Craft, "minecraft:stick",
                                          "minecraft:stick", 1, false});
            if (tables < 1 && !placed)
                steps.push_back(ChainStep{ChainStep::Kind::Craft, "minecraft:WorkBench_recipeId",
                                          "minecraft:crafting_table", 1, false});
            if (!steps.empty()) {
                steps.insert(steps.begin(), ChainStep{ChainStep::Kind::OpenInventory});
                steps.push_back(ChainStep{ChainStep::Kind::CloseUi});
            }
        }
        if (!placed) steps.push_back(ChainStep{ChainStep::Kind::PlaceTable});
        steps.push_back(ChainStep{ChainStep::Kind::OpenTable});
        steps.push_back(ChainStep{ChainStep::Kind::Craft, "minecraft:stone_sword",
                                  "minecraft:stone_sword", 1, true});
        steps.push_back(ChainStep{ChainStep::Kind::CloseUi});
        chainSteps_ = std::move(steps);
        chainStep_ = 0; chainActive_ = true; chainJobStarted_ = false;
        chainPlaceCell_ = {INT_MIN, 0, 0};
        chainPlaceCells_.clear(); chainPlaceIdx_ = 0; placeCellTicks_ = 0;
        chainPocketing_ = false; chainPocketDug_ = false; chainPocketBad_ = 0; chainPocket_ = {INT_MIN, 0, 0};
        chainStepTicks_ = 0; chainAimedTick_ = -1; chainRetriedOpen_ = false;
        chainRecoverTicks_ = -1; chainPendingFail_.clear();
        chainWaitingCobble_ = false;
        chainForPickaxe_ = false; chainReturn_ = Phase::Scan;
        phase_ = Phase::Craft;
        note("craft chain: stone sword (weapon prep)");
        return true;
    }
    // Before cave stages: a sword in the hotbar (moved from the
    // main inventory, or crafted from materials on hand).
    void ensureSwordPrep() {
        if (!craftReady_) return;
        std::string mainSword;
        for (const auto& s : craft_.snapshot()) {
            if (!s.count || !contains(localPart(s.item), "_sword")) continue;
            if (s.slot < 9) return; // already in the hotbar
            mainSword = localPart(s.item);
        }
        if (!mainSword.empty()) {
            (void)ensureHotbarTick(mainSword);
            note("weapon prep: moving " + mainSword + " to the hotbar");
            return;
        }
        (void)tryStartSwordChain();
    }
    bool tryStartBoatChain() {
        if (!craftReady_ || !craft_.recipesReady() || !craft_.ready() || chainActive_) return false;
        if (craft_.status() == bedrock::CraftJobStatus::Running) return false;
        craft_.clearStatus();
        boatChainTriedFor_ = target_; // one attempt per target from here on
        const auto have = chainCounts();
        const int planks = countIn(have, "planks");
        const int tables = countIn(have, "crafting_table");
        const bool placed = placedTableExists();
        int logCount = 0;
        const std::string wood = chainWood(have, logCount);
        // Boat wood follows the planks that will actually be consumed: the
        // most plentiful planks, else the logs this chain turns into planks.
        std::string boatWood = wood;
        int bestPlanks = 0;
        for (const auto& [id, count] : have)
            if (contains(id, "_planks") && count > bestPlanks) {
                bestPlanks = count;
                boatWood = id.substr(0, id.size() - std::string_view("_planks").size());
            }
        if (boatWood.empty()) return false;
        std::vector<ChainStep> steps;
        const int plankNeed = 5 + ((tables < 1 && !placed) ? 4 : 0);
        const int deficit = plankNeed - planks;
        const int logCrafts = deficit > 0 ? (deficit + 3) / 4 : 0;
        if (logCrafts > 0) {
            if (wood.empty() || logCount < logCrafts) return false;
            steps.push_back(ChainStep{ChainStep::Kind::Craft,
                                      "minecraft:" + wood + "_planks",
                                      "minecraft:" + wood + "_planks", logCrafts, false});
        }
        if (tables < 1 && !placed)
            steps.push_back(ChainStep{ChainStep::Kind::Craft, "minecraft:WorkBench_recipeId",
                                      "minecraft:crafting_table", 1, false});
        if (!steps.empty()) {
            steps.insert(steps.begin(), ChainStep{ChainStep::Kind::OpenInventory});
            steps.push_back(ChainStep{ChainStep::Kind::CloseUi});
        }
        if (!placed) steps.push_back(ChainStep{ChainStep::Kind::PlaceTable});
        steps.push_back(ChainStep{ChainStep::Kind::OpenTable});
        steps.push_back(ChainStep{ChainStep::Kind::Craft, "minecraft:boat",
                                  "minecraft:" + boatWood + "_boat", 1, true});
        steps.push_back(ChainStep{ChainStep::Kind::CloseUi});
        chainSteps_ = std::move(steps);
        chainStep_ = 0; chainActive_ = true; chainJobStarted_ = false;
        chainPlaceCell_ = {INT_MIN, 0, 0};
        chainPlaceCells_.clear(); chainPlaceIdx_ = 0; placeCellTicks_ = 0;
        chainPocketing_ = false; chainPocketDug_ = false; chainPocketBad_ = 0; chainPocket_ = {INT_MIN, 0, 0};
        chainStepTicks_ = 0; chainAimedTick_ = -1; chainRetriedOpen_ = false;
        chainRecoverTicks_ = -1; chainPendingFail_.clear();
        chainWaitingCobble_ = false;
        chainForPickaxe_ = false; chainReturn_ = Phase::Navigate;
        phase_ = Phase::Craft;
        reply("ボートが無いから、木からクラフトするね");
        note("craft chain: boat (" + std::to_string(chainSteps_.size()) + " steps)");
        return true;
    }
    void chainAdvance() {
        ++chainStep_;
        chainStepTicks_ = 0; chainAimedTick_ = -1; chainJobStarted_ = false;
        chainPlaceArmed_ = false; chainNoSlotRetries_ = 0; chainNoSlotTicks_ = 0;
        // A place step may be holding Use / backing off when the
        // block lands: never leak a held input into the next step.
        setUse(false); pressKey(kKeyS, false);
        chainPlaceCell_ = {INT_MIN, 0, 0};
        chainPlaceCells_.clear(); chainPlaceIdx_ = 0; placeCellTicks_ = 0;
        chainPlaceSlotIdx_ = 0; chainPlaceSlotTicks_ = 0;
        chainPocketing_ = false; chainPocketDug_ = false; chainPocketBad_ = 0; chainPocket_ = {INT_MIN, 0, 0};
        if (chainStep_ >= chainSteps_.size()) chainFinish();
    }
    void chainFinish() {
        chainActive_ = false;
        chainRetries_ = 0; chainNextRetryTick_ = 0;
        setUse(false);
        pressKey(kKeySpace, false);
        if (chainForPickaxe_) {
            wantPickaxeChain_ = false;
            reply("ツルハシ完成！採掘を再開するね");
            note("craft chain: pickaxe done");
            phase_ = Phase::Scan; scanCursor_ = 0; bestDist2_ = 1e18; hasTarget_ = false;
        } else if (chainForFurnace_) {
            chainForFurnace_ = false;
            note("craft chain: furnace done");
            phase_ = chainReturn_;
        } else {
            reply("ボート完成！");
            note("craft chain: boat done");
            phase_ = chainReturn_;
        }
    }
    void chainFail(const std::string& why) {
        note("craft chain failed: " + why);
        chainActive_ = false; chainJobStarted_ = false; chainWaitingCobble_ = false;
        setUse(false);
        pressKey(kKeySpace, false);
        if (chainForPickaxe_) {
            // Transient failures (a screen that had not finished opening, a
            // lagging response) recover on their own: retry a few times on a
            // cooldown before falling back to waiting for a manual tool.
            if (++chainRetries_ <= 5) {
                chainNextRetryTick_ = tickCount_ + 20 * 20;
                if (chainRetries_ == 1)
                    reply("クラフト失敗（" + why + "）。少し待って再挑戦するね");
            } else {
                wantPickaxeChain_ = false;
                reply("クラフト失敗（" + why + "）。手でツルハシを用意してね");
            }
            phase_ = Phase::NeedTool; needTicks_ = 0;
        } else {
            chainForFurnace_ = false;
            phase_ = chainReturn_;
        }
    }
    void dispatchCraftStep(const ChainStep& step) {
        bedrock::CraftRecipe recipe;
        if (!craft_.copyRecipe(step.recipeId, recipe)) {
            chainFail("レシピ未取得: " + step.recipeId);
            return;
        }
        const int base = step.tableGrid ? 32 : 28;
        const int width = step.tableGrid ? 3 : 2;
        if (!craft_.startCraft(recipe, step.outputName, step.times, base, width)) {
            std::string why = "クラフト開始失敗";
            if (craft_.status() == bedrock::CraftJobStatus::Failed) {
                why = craft_.statusDetail();
                craft_.clearStatus();
            }
            chainFail(why);
            return;
        }
        chainJobStarted_ = true;
        chainStepTicks_ = 0;
    }
    void tickCraft(bedrock::Vec3 feet) {
        // A chain can start at the water's edge (boat) or be caught by a
        // rising shore: hold the swim-up key while wet and only abandon the
        // chain after 3 s fully submerged.
        if (inWaterNow(feet)) pressKey(kKeySpace, true);
        if (headInWater(feet)) {
            if (++chainSubmerged_ >= 60) {
                pressKey(kKeySpace, false);
                chainFail("水中");
                startEscape();
                return;
            }
        } else chainSubmerged_ = 0;
        if (!chainActive_) { phase_ = chainReturn_; return; }
        if (chainRecoverTicks_ >= 0) {
            // Post-failure cleanup before any retry: close the crafting
            // screen so the game itself returns grid/cursor leftovers
            // to the inventory, then reset the adapter's shadow. A
            // failed job leaves its placed ingredients in the grid;
            // retrying against that grid fails every place with
            // status 50 while the parked ingredients read as missing
            // (user's run 2026-11-17: the same status=50 failure
            // repeated across four chain restarts).
            ++chainRecoverTicks_;
            if (game_.gameplayActive() || chainRecoverTicks_ > 40) {
                craft_.resetAfterFailure();
                chainRecoverTicks_ = -1;
                const std::string why = chainPendingFail_;
                chainPendingFail_.clear();
                // A table-grid craft failing usually means the table
                // UI never opened: rewind to the open step, retry once.
                if (chainStep_ < chainSteps_.size() && chainSteps_[chainStep_].tableGrid &&
                    !chainRetriedOpen_) {
                    for (std::size_t i = chainStep_; i-- > 0;)
                        if (chainSteps_[i].kind == ChainStep::Kind::OpenTable) {
                            chainRetriedOpen_ = true;
                            chainStep_ = i; chainStepTicks_ = 0; chainAimedTick_ = -1;
                            note("craft chain: reopening table after failure: " + why);
                            return;
                        }
                }
                chainFail(why);
                return;
            }
            if (chainRecoverTicks_ == 1 || chainRecoverTicks_ == 20) pressKey(0x1B, true);
            if (chainRecoverTicks_ == 3 || chainRecoverTicks_ == 22) pressKey(0x1B, false);
            return;
        }
        ++chainStepTicks_;
        if (chainJobStarted_) {
            const auto st = craft_.status();
            if (st == bedrock::CraftJobStatus::Running) {
                if (chainStepTicks_ > 20 * 240) chainFail("クラフト応答なし");
                return;
            }
            chainJobStarted_ = false;
            if (st == bedrock::CraftJobStatus::Failed) {
                // Route through the clean-state recovery above; the
                // rewind-or-fail decision runs when it completes.
                chainPendingFail_ = craft_.statusDetail();
                craft_.clearStatus();
                chainRecoverTicks_ = 0;
                return;
            }
            craft_.clearStatus(); // Done
            chainAdvance();
            return;
        }
        if (chainStep_ >= chainSteps_.size()) { chainFinish(); return; }
        const ChainStep& step = chainSteps_[chainStep_];
        switch (step.kind) {
        case ChainStep::Kind::Craft:
            // Give the screen the previous step opened a moment to
            // attach server-side: a grid place sent the same tick the
            // client first shows the screen can still be rejected
            // (status 49, measured 2026-11-16).
            if (chainStepTicks_ >= 10) dispatchCraftStep(step);
            break;
        case ChainStep::Kind::PlaceTable: tickPlaceTable(feet); break;
        case ChainStep::Kind::OpenTable: tickOpenTable(feet); break;
        case ChainStep::Kind::CloseUi: tickCloseUi(); break;
        case ChainStep::Kind::OpenInventory: tickOpenInventory(); break;
        case ChainStep::Kind::WaitCobble:
            ++chainStep_; chainStepTicks_ = 0;
            chainWaitingCobble_ = true; chainCobbleTicks_ = 0; chainReturnTrips_ = 0;
            cobbleNavFails_ = 0; cobbleDescend_ = false;
            scanIds_ = {"stone"};
            phase_ = Phase::Scan; scanCursor_ = 0; bestDist2_ = 1e18; hasTarget_ = false;
            reply("木のツルハシ完成。丸石を3つ集めるね");
            note("craft chain: waiting for cobblestone");
            break;
        }
    }
    // Dig a one-cell side pocket beside the player for a placement
    // that has no air neighbour at feet level (a 1-wide shaft —
    // measured 2026-11-17: PlaceTable failed at y=16 inside the
    // descend shaft with "no place"). The first ticks back the player
    // against the far wall so the pocket cell clears the player's
    // AABB (a cell the AABB reaches into can never accept a block).
    // Returns 0 = working, 1 = pocket open, 2 = every direction
    // refused.
    int digPocketTick(bedrock::Vec3 feet, BlockPos3& pocket, int& badMask, int& stage) {
        static constexpr int kDX[4] = {1, -1, 0, 0};
        static constexpr int kDZ[4] = {0, 0, 1, -1};
        const int fx = static_cast<int>(std::floor(feet.x));
        const int fy = static_cast<int>(std::floor(feet.y));
        const int fz = static_cast<int>(std::floor(feet.z));
        if (pocket.x == INT_MIN) {
            stage = 0;
            for (int i = 0; i < 4; ++i) {
                if (badMask & (1 << i)) continue;
                const BlockPos3 cell{fx + kDX[i], fy, fz + kDZ[i]};
                const auto at = blockAt(cell);
                if (contains(localPart(at), "bedrock")) { badMask |= 1 << i; continue; }
                const auto ground = blockAt({cell.x, cell.y - 1, cell.z});
                if (ground.empty() || isAir(ground) || contains(localPart(ground), "water")) {
                    badMask |= 1 << i;
                    continue;
                }
                // A gravity block in the cell itself, or directly above
                // it, refills the pocket the moment it is dug.
                if (isGravityBlock(localPart(at)) ||
                    isGravityBlock(localPart(blockAt({cell.x, cell.y + 1, cell.z})))) {
                    badMask |= 1 << i;
                    continue;
                }
                // Air/water cells are valid pockets too: the blocker
                // there is the player's own AABB reaching into the
                // cell, and the back-off stage below is what clears
                // it (measured 2026-11-17: the only air neighbour at
                // y=16 was rejected purely for AABB overlap).
                pocket = cell;
                break;
            }
            if (pocket.x == INT_MIN) {
                // Debug (2026-11-17): why was every direction refused?
                std::string dbg = "pocket refused:";
                for (int i = 0; i < 4; ++i) {
                    const BlockPos3 cell{fx + kDX[i], fy, fz + kDZ[i]};
                    dbg += " [" + localPart(blockAt(cell)) + "|g:" +
                           localPart(blockAt({cell.x, cell.y - 1, cell.z})) + "|a:" +
                           localPart(blockAt({cell.x, cell.y + 1, cell.z})) + "]";
                }
                dbg += " feet=" + std::to_string(fx) + "," + std::to_string(fy) + "," +
                       std::to_string(fz);
                note(dbg);
                return 2;
            }
        }
        // Back off until the player's AABB no longer overlaps the
        // pocket cell's column — a fixed 6 ticks was not enough when
        // the facing had not converged (S then pushed the player INTO
        // the pocket block and the cell stayed overlapped, measured
        // 2026-11-17: feet.x moved only 0.06 of the 0.12 needed).
        // Only hold S while actually aimed at the pocket, and give up
        // after 60 ticks (the caller's collection keeps the dug cell
        // as a candidate and the placement loop keeps retreating).
        // feetPos is the AABB CENTRE (span +-0.3), not the min
        // corner: the old +0.6 form modelled the player as
        // [feet, feet+0.6], which never excludes the west/north
        // neighbour and misjudges the back-off distance (measured
        // 2026-11-18: a furnace cell the AABB reached into was
        // pulsed for the full 25 s budget; the game rejected every
        // placement for intersecting the player).
        const bool intersects = pocket.x < feet.x + .31f && pocket.x + 1 > feet.x - .31f &&
                                pocket.z < feet.z + .31f && pocket.z + 1 > feet.z - .31f;
        if (intersects && stage < 60) {
            bedrock::Vec3 eye;
            bool aimed = false;
            if (eyePos(eye)) {
                const bedrock::Vec3 c{pocket.x + .5f, pocket.y + .5f, pocket.z + .5f};
                aimed = faceToward(eye, c, 0.15f);
            }
            pressKey(kKeyS, aimed);
            ++stage;
            return 0;
        }
        pressKey(kKeyS, false);
        const auto atNow = blockAt(pocket);
        if (isAir(atNow) || contains(localPart(atNow), "water")) {
            return 1; // keep the finished cell in `pocket` for the caller
        }
        const int r = digCell(pocket, feet, /*toolOptional=*/true);
        if (r == 1) return 1; // same: caller reads the cell from `pocket`
        if (r == 2) {
            const int dx = pocket.x - fx, dz = pocket.z - fz;
            for (int i = 0; i < 4; ++i)
                if (kDX[i] == dx && kDZ[i] == dz) badMask |= 1 << i;
            pocket = {INT_MIN, 0, 0};
            stage = 0;
        }
        return 0;
    }
    void tickPlaceTable(bedrock::Vec3 feet) {
        // A table placed by an earlier pulse (or an earlier chain
        // attempt) may sit in a neighbour cell this step never
        // re-checks: adopt any reachable table next to the player
        // before pulsing (measured 2026-11-17: four tables on the
        // ground while the chain kept timing out on placement).
        {
            bedrock::Vec3 eye0;
            if (eyePos(eye0)) {
                const int fx = static_cast<int>(std::floor(feet.x));
                const int fy = static_cast<int>(std::floor(feet.y));
                const int fz = static_cast<int>(std::floor(feet.z));
                // The ring at feet level AND one step down/up: a
                // table placed from a 1-block rise sits at fy-1 and
                // the feet-level-only scan missed it while placement
                // cycled overhead (measured 2026-11-17: a placed
                // table in plain view, 27 futile candidate cycles).
                // fy+1 matters just as much: on a slope the aim ray
                // clips the uphill block's side face and the table
                // lands ON it, one above feet level (measured
                // 2026-11-18 at a forest spawn: a placed table in
                // plain view, chain failing in a loop).
                for (int dy = 1; dy >= -1; --dy) {
                    const BlockPos3 nbs[8] = {{fx + 1, fy + dy, fz}, {fx - 1, fy + dy, fz},
                                              {fx, fy + dy, fz + 1}, {fx, fy + dy, fz - 1},
                                              {fx + 1, fy + dy, fz + 1}, {fx + 1, fy + dy, fz - 1},
                                              {fx - 1, fy + dy, fz + 1}, {fx - 1, fy + dy, fz - 1}};
                    for (const auto& nb : nbs) {
                        if (!contains(localPart(blockAt(nb)), "crafting_table")) continue;
                        if (tableBanned(nb)) continue;
                        BlockPos3 occ;
                        if (!lineOfSight(eye0, nb, occ)) continue;
                        chainTablePos_ = nb;
                        chainTablePlaced_ = true;
                        note("craft chain: adopting nearby table");
                        chainAdvance();
                        return;
                    }
                }
            }
        }
        if (chainStepTicks_ > 20 * 25) { chainFail("作業台を置けなかった"); return; }
        if (anyScreenUp() && moveStage_ == MoveStage::None) {
            // Same leftover-screen guard as tickPlaceFurnace: a screen
            // left open by an earlier step eats every placement pulse,
            // and Phase::Craft is exempt from tick()'s recovery. Never
            // while a hotbar move is in flight: the screen it reads as
            // "leftover" can be the move's own inventory screen (the
            // gameplay-active event flips unreliably for it), and an
            // Esc here closes that screen mid-move — every transfer
            // then fails status 49 (measured 2026-11-17: a table
            // eviction failed 3x inside an otherwise healthy chain).
            if (tickCount_ % 30 == 1) pressKey(0x1B, true);
            if (tickCount_ % 30 == 3) pressKey(0x1B, false);
            return;
        }
        if (chainPocketing_) {
            const int r = digPocketTick(feet, chainPocket_, chainPocketBad_, chainPocketStage_);
            if (r == 2) { chainPocketing_ = false; chainFail("作業台を置く場所が無い"); return; }
            if (r == 1) { chainPocketing_ = false; chainPocketDug_ = true; chainPocketCell_ = chainPocket_; }
            else return;
        }
        bedrock::Vec3 eye;
        if (!eyePos(eye)) return;
        if (chainPlaceCell_.x == INT_MIN) {
            const int fx = static_cast<int>(std::floor(feet.x));
            const int fy = static_cast<int>(std::floor(feet.y));
            const int fz = static_cast<int>(std::floor(feet.z));
            // Try every horizontal neighbour, the target direction first.
            // A neighbour that already holds a crafting table (left over
            // from an earlier run — the world persists) is adopted as-is.
            std::array<BlockPos3, 4> dirs{{{1, 0, 0}, {-1, 0, 0}, {0, 0, 1}, {0, 0, -1}}};
            if (hasTarget_) {
                bedrock::Vec3 want{target_.x + .5f - feet.x, 0, target_.z + .5f - feet.z};
                std::stable_sort(dirs.begin(), dirs.end(),
                                 [&](const BlockPos3& a, const BlockPos3& b) {
                                     return (a.x * want.x + a.z * want.z) >
                                            (b.x * want.x + b.z * want.z);
                                 });
            }
            // Collect EVERY viable neighbour, not just the first: the
            // aim ray for one cell can be shadowed by a diagonal block
            // or land on a face whose placement cell intersects the
            // player diagonally, silently eating every pulse (measured
            // 2026-11-16: wedged against an old table on a slope, one
            // committed cell failed forever). The pulse loop below
            // cycles through the candidates.
            chainPlaceCells_.clear();
            for (const auto& d : dirs) {
                const BlockPos3 cell{fx + d.x, fy, fz + d.z};
                const bool isPocketCell = chainPocketDug_ && cell.x == chainPocketCell_.x &&
                                          cell.y == chainPocketCell_.y &&
                                          cell.z == chainPocketCell_.z;
                // A cell the player's own AABB reaches into can never
                // accept a block (the game rejects placements that
                // intersect the player) — with the feet point near the
                // cell edge this silently ate every pulse (measured
                // 2026-11-16: table placement timed out twice in a row
                // while the bot stood at the platform edge).
                if (cell.x < feet.x + .31 && cell.x + 1 > feet.x - .31 &&
                    cell.z < feet.z + .31 && cell.z + 1 > feet.z - .31 &&
                    !isPocketCell)
                    continue;
                const auto at = blockAt(cell);
                if (contains(localPart(at), "crafting_table")) {
                    // Only adopt a table the use ray can actually
                    // reach. A table shadowed by another block (a
                    // furnace left in the aim path by the smelt
                    // stage) opens THAT block's screen instead, and
                    // every craft then fails status 50 at phase 0 —
                    // measured 2026-11-17 on fresh2 (~900 failed
                    // crafts against a furnace screen). Skip a
                    // shadowed table and place a fresh one instead.
                    BlockPos3 occ;
                    if (!lineOfSight(eye, cell, occ)) continue;
                    chainTablePos_ = cell;
                    chainTablePlaced_ = true;
                    note("craft chain: adopting existing table");
                    chainAdvance();
                    return;
                }
                const auto ground = blockAt({cell.x, cell.y - 1, cell.z});
                // Passable decorations (leaf litter, tall grass, ...)
                // are replaceable: the game lets a placement take
                // their cell. Treating them as occupied rejected every
                // neighbour on litter-covered forest floors and the
                // chain failed with "no place" (measured 2026-11-17).
                if (ground.empty() || isAir(ground) || contains(localPart(ground), "water") ||
                    (!isAir(at) && !isPassableCell(at) && !contains(localPart(at), "water")))
                    continue;
                // A gravity block above the cell falls in while the
                // pocket is dug or the pulses run: the cell can never
                // stay placeable (measured 2026-11-17: the table cell
                // stayed gravel through 25 s of pulses).
                if (isGravityBlock(localPart(blockAt({cell.x, cell.y + 1, cell.z}))))
                    continue;
                // The placement ray must actually reach the ground
                // block: a diagonal neighbour can shadow the ray so
                // every pulse lands on the wrong face and the block
                // never appears (measured 2026-11-17 in a cave: two
                // air candidates, 13 pulse cycles, no placement).
                // The dug pocket cell is exempt: the player just
                // backed out of it while facing it, and in a tight
                // spot the voxel ray to the ground centre grazes the
                // pocket wall and reads as shadowed — the exemption
                // mismatch failed the whole chain with "no place"
                // right after a successful pocket dig (measured
                // 2026-11-17).
                BlockPos3 occluder;
                if (!isPocketCell &&
                    !lineOfSight(eye, {cell.x, cell.y - 1, cell.z}, occluder))
                    continue;
                chainPlaceCells_.push_back(cell);
            }
            if (chainPlaceCells_.empty()) {
                if (chainPocketDug_) {
                    std::string dbg = "place cells empty after pocket:";
                    for (const auto& d : dirs) {
                        const BlockPos3 cell{fx + d.x, fy, fz + d.z};
                        dbg += " [" + localPart(blockAt(cell)) + "|g:" +
                               localPart(blockAt({cell.x, cell.y - 1, cell.z})) + "]";
                    }
                    dbg += " pocket=" + std::to_string(chainPocketCell_.x) + "," +
                           std::to_string(chainPocketCell_.y) + "," + std::to_string(chainPocketCell_.z) +
                           " feet=" + std::to_string(fx) + "," + std::to_string(fy) + "," + std::to_string(fz);
                    note(dbg);
                    chainFail("作業台を置く場所が無い");
                    return;
                }
                // No air neighbour (1-wide shaft): dig a side pocket
                // and collect candidates again.
                chainPocketing_ = true;
                chainPocket_ = {INT_MIN, 0, 0};
                chainPocketStage_ = 0;
                note("craft chain: digging a pocket for the table");
                return;
            }
            chainPlaceIdx_ = 0;
            placeCellTicks_ = 0;
            chainPlaceCell_ = chainPlaceCells_[0];
        }
        // A placement pulse can land one cell off the committed cell
        // (the ray hits a diagonal neighbour's face): the table then
        // EXISTS but the check below never sees it, later pulses open
        // its screen instead of placing, and the step cycles all
        // candidates into a timeout (reproduced 2026-11-17 on BOL:
        // 82 futile candidate cycles with a placed table in view).
        // Scan the whole ring every tick while placing and adopt any
        // table that appeared.
        {
            const int fx = static_cast<int>(std::floor(feet.x));
            const int fy = static_cast<int>(std::floor(feet.y));
            const int fz = static_cast<int>(std::floor(feet.z));
            for (int dy = 1; dy >= -1; --dy) {
                const BlockPos3 ring[8] = {{fx + 1, fy + dy, fz}, {fx - 1, fy + dy, fz},
                                           {fx, fy + dy, fz + 1}, {fx, fy + dy, fz - 1},
                                           {fx + 1, fy + dy, fz + 1}, {fx + 1, fy + dy, fz - 1},
                                           {fx - 1, fy + dy, fz + 1}, {fx - 1, fy + dy, fz - 1}};
                for (const auto& nb : ring) {
                    if (!contains(localPart(blockAt(nb)), "crafting_table")) continue;
                    if (tableBanned(nb)) continue;
                    BlockPos3 occ;
                    if (!lineOfSight(eye, nb, occ)) continue;
                    chainTablePos_ = nb;
                    chainTablePlaced_ = true;
                    note("craft chain: adopting table placed beside the target cell");
                    chainAdvance();
                    return;
                }
            }
        }
        if (contains(localPart(blockAt(chainPlaceCell_)), "crafting_table")) {
            chainTablePos_ = chainPlaceCell_;
            chainTablePlaced_ = true;
            note("craft chain: table placed");
            chainAdvance();
            return;
        }
        {
            // The cell refilled after collection (falling gravel/sand):
            // drop it and re-collect instead of pulsing at a solid
            // cell for the rest of the budget.
            const auto atNow = blockAt(chainPlaceCell_);
            if (!isAir(atNow) && !isPassableCell(atNow) && !contains(localPart(atNow), "water")) {
                note("craft chain: place cell refilled, re-collecting");
                chainPlaceCell_ = {INT_MIN, 0, 0};
                return;
            }
        }
        int slot = findTableHotbarSlot();
        if (moveStage_ != MoveStage::None) {
            // Same orphaned-move guard as tickPlaceFurnace: drive any
            // in-flight hotbar move to completion before pulsing.
            if (!ensureHotbarTick("crafting_table")) return;
            slot = findTableHotbarSlot();
        } else if (slot < 0 && countAllItems("crafting_table") >= 1 && !moveFailed_) {
            // The crafted table sits in the main inventory (the
            // hotbar was full at deposit time): move it in first.
            if (!ensureHotbarTick("crafting_table")) return;
            slot = findTableHotbarSlot();
        }
        if (slot < 0) {
            // The memory hotbar read can flicker for a tick (a stack
            // mid-update reads as a different item): tolerate a
            // window, then force one fresh move re-plan before
            // giving up. A stale shadow can leave the move planner
            // convinced there is no source while the item count says
            // one exists, and the old 40-tick window died in 2 s
            // without ever re-planning (measured 2026-11-18: a smelt
            // chain failed here with a table in the inventory).
            if (++chainNoSlotTicks_ <= 100) return;
            if (chainNoSlotRetries_ < 1 && countAllItems("crafting_table") >= 1) {
                ++chainNoSlotRetries_;
                chainNoSlotTicks_ = 0;
                moveFailed_ = false;
                moveStage_ = MoveStage::None;
                note("craft chain: table counted but no hotbar slot; resetting the move and retrying");
                return;
            }
            moveFailed_ = false;
            chainFail("作業台がホットバーに無い");
            return;
        }
        chainNoSlotTicks_ = 0;
        if (!chainPlaceArmed_) {
            // The placement budget starts when the item is actually
            // in hand: the hotbar move that brought it here can eat
            // most of the step's 25 s on its own (measured
            // 2026-11-18: the furnace move completed and the place
            // step died on the shared budget seconds later).
            chainPlaceArmed_ = true;
            chainStepTicks_ = 0;
        }
        // The merged view can claim the table at a slot the server
        // disagrees with (see hotbarItems): rotate every claimed
        // slot, 40 ticks each, instead of holding use on one
        // possibly-empty slot for the whole budget.
        slot = placeSlotTick("crafting_table", chainPlaceSlotIdx_, chainPlaceSlotTicks_, slot);
        selectSlot(slot);
        const bedrock::Vec3 aim{chainPlaceCell_.x + .5f, static_cast<float>(chainPlaceCell_.y),
                                chainPlaceCell_.z + .5f};
        // Keep re-aiming until the block appears: a single use input
        // right after the inventory screen closes is often eaten —
        // at the mesa and at the shore alike, the first placement
        // attempt failed and the second succeeded.
        faceToward(eye, aim, 3.f);
        // If the target cell still overlaps the player's AABB (a dug
        // pocket the back-off could not fully clear), keep backing
        // away while placing: the game rejects intersecting
        // placements, but a held use lands the moment it clears.
        pressKey(kKeyS, chainPlaceCell_.x < feet.x + .31f && chainPlaceCell_.x + 1 > feet.x - .31f &&
                        chainPlaceCell_.z < feet.z + .31f && chainPlaceCell_.z + 1 > feet.z - .31f);
        // Hold the use button instead of pulsing it 2-in-12: the
        // same lesson as the surface-return climb — in marginal
        // geometry (a cave pocket with the ray grazing a wall) a
        // short pulse can be eaten for the whole 25 s budget, while
        // a held use places on the first tick the ray clears
        // (measured 2026-11-18: three smelt chains died here in
        // silence). chainAdvance releases the hold when the block
        // lands and the step moves on.
        setUse(true);
        if (++placeCellTicks_ > 30 && chainPlaceCells_.size() > 1) {
            placeCellTicks_ = 0;
            chainPlaceIdx_ = (chainPlaceIdx_ + 1) % chainPlaceCells_.size();
            chainPlaceCell_ = chainPlaceCells_[chainPlaceIdx_];
            note("craft chain: trying another table cell");
        }
        if ((chainStepTicks_ % 100) == 50) {
            note("place diag: table cell=" + std::to_string(chainPlaceCell_.x) + "," +
                 std::to_string(chainPlaceCell_.y) + "," + std::to_string(chainPlaceCell_.z) +
                 " at=" + localPart(blockAt(chainPlaceCell_)) +
                 " ground=" + localPart(blockAt({chainPlaceCell_.x, chainPlaceCell_.y - 1,
                                                 chainPlaceCell_.z})) +
                 " above=" + localPart(blockAt({chainPlaceCell_.x, chainPlaceCell_.y + 1,
                                                chainPlaceCell_.z})) +
                 " slot=" + std::to_string(slot) +
                 " top=" + (screenReady_ ? topScreenName() : std::string("?")));
        }
    }
    bool tableBanned(const BlockPos3& p) {
        return tickCount_ < chainTableBadUntil_ && p == chainTableBad_;
    }
    void tickOpenTable(bedrock::Vec3 feet) {
        if (!chainTablePlaced_) { chainFail("作業台が無い"); return; }
        if (chainStepTicks_ > 20 * 6) {
            // This table would not open (measured 2026-11-18: a table
            // adopted inside a tree canopy failed 20+ chain retries
            // in a row, each retry adopting the same table). Ban it
            // for a while so the retry places a fresh table elsewhere
            // instead of looping on the poisoned one.
            chainTableBad_ = chainTablePos_;
            chainTableBadUntil_ = tickCount_ + 20 * 600;
            chainTablePlaced_ = false;
            note("craft chain: table would not open; banning it and re-placing elsewhere");
            chainFail("作業台を開けなかった");
            return;
        }
        bedrock::Vec3 eye;
        if (!eyePos(eye)) return;
        const bedrock::Vec3 center{chainTablePos_.x + .5f, chainTablePos_.y + .5f,
                                   chainTablePos_.z + .5f};
        BlockPos3 occ;
        if (!lineOfSight(eye, chainTablePos_, occ)) {
            // Shadowed by another CRAFTING TABLE: that table is the
            // one the ray actually reaches — adopt it instead of
            // failing (measured 2026-11-17: two tables in a 1-wide
            // shaft, each shadowing the other, and the smelt chain
            // died on it). Any other occluder still fails the step:
            // the use pulse would open the WRONG screen and the craft
            // would fail 50 at phase 0 forever.
            if (contains(localPart(blockAt(occ)), "crafting_table") && !tableBanned(occ)) {
                note("craft chain: adopting the table in the aim path");
                chainTablePos_ = occ;
                chainAimedTick_ = -1;
                return;
            }
            note("craft chain: table shadowed by " + localPart(blockAt(occ)) +
                 "; re-placing");
            chainTablePlaced_ = false;
            chainFail("作業台が他のブロックに隠れている");
            return;
        }
        if (chainAimedTick_ < 0) {
            if (faceToward(eye, center, 3.f)) chainAimedTick_ = chainStepTicks_;
            return;
        }
        const int since = chainStepTicks_ - chainAimedTick_;
        setUse(since < 2);
        if (!game_.gameplayActive()) { setUse(false); chainAdvance(); return; }
        // The pulse missed (no screen after 15 ticks): re-aim and
        // pulse again instead of advancing blind — advancing here is
        // what sent table crafts into status-49 failures on fresh
        // worlds (measured 2026-11-17). The step timeout above fails
        // the chain if the table truly cannot be opened.
        if (since > 15) chainAimedTick_ = -1;
    }
    void tickOpenInventory() {
        // E toggles the inventory screen (and with it the personal 2x2
        // crafting grid the server validates grid places against).
        // gameplayActive() == false means a screen is open, so poll it
        // instead of assuming one pulse landed (the first feed after
        // joining can be eaten by device capture warm-up).
        if (!game_.gameplayActive()) { chainAdvance(); return; }
        if ((chainStepTicks_ % 45) == 1) pressKey(0x45, true);  // VK_E
        if ((chainStepTicks_ % 45) == 3) pressKey(0x45, false);
        // Never advance into a craft whose grid the server will reject
        // (status 49): if the screen never opened, fail the chain here
        // and let the retry loop try again from a clean state.
        if (chainStepTicks_ > 300) chainFail("インベントリを開けなかった");
    }
    void tickCloseUi() {
        if (game_.gameplayActive()) { chainAdvance(); return; }
        if (chainStepTicks_ == 1 || chainStepTicks_ == 30)
            pressKey(0x1B, true);  // VK_ESCAPE
        if (chainStepTicks_ == 3 || chainStepTicks_ == 32)
            pressKey(0x1B, false);
        if (chainStepTicks_ > 70) chainAdvance();
    }

    // -- expedition: gather / descend / hunt --------------------------------
    // The from-zero quest (empty hotbar, vanilla world) is driven by a
    // supervisor (tickExpedition) on top of these primitives: a gather is
    // the normal mining loop with a scan override and an inventory-count
    // finish line; descending digs a 1x1 shaft one safe block at a time;
    // a hunt leg tunnels a few cells so the next scan sweep sees new rock.
    void endScanOverride() {
        if (scanOverrideHeld_) { targetId_ = scanSavedId_; scanOverrideHeld_ = false; }
        scanIds_.clear();
    }
    void startGather(std::vector<std::string> blockIds, std::vector<std::string> countIds,
                     int want, bool hunt) {
        gatherActive_ = true; gatherDone_ = false; gatherFailed_ = false;
        gatherCountIds_ = std::move(countIds); gatherWant_ = want; gatherHunt_ = hunt;
        gatherDeadline_ = tickCount_ + 20 * 60 * 25;
        scanIds_ = std::move(blockIds);
        phase_ = Phase::Scan; scanCursor_ = 0; bestDist2_ = 1e18;
        hasTarget_ = false; emptyScans_ = 0;
        note("gather: start, want " + std::to_string(want));
    }
    bool lavaAdjacent(const BlockPos3& cell) {
        static constexpr BlockPos3 kDirs[6] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0},
                                               {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
        for (const auto& d : kDirs)
            if (contains(localPart(blockAt({cell.x + d.x, cell.y + d.y, cell.z + d.z})), "lava"))
                return true;
        return false;
    }
    // Dig one specific cell with the right tool. 0 = working, 1 = the
    // cell is air now, 2 = refused (lava next door, no tool, timeout).
    // toolOptional: dig by hand when no suitable tool is available.
    // For a placement POCKET the drop is irrelevant — only the open
    // cell matters — and requiring a pickaxe there deadlocks the
    // bootstrap: no pickaxe -> no pocket -> no table -> no pickaxe
    // (measured 2026-11-17: the chain failed twice in a 1-wide shaft
    // after the wooden pickaxe broke).
    int digCell(const BlockPos3& cell, bedrock::Vec3 feet, bool toolOptional = false) {
        const auto id = blockAt(cell);
        // Reset the dig state when the target cell changes OR the
        // block in it was replaced: the pocket dig and the furnace
        // break can target the same cell back to back (a table dug
        // out, a furnace placed into the same hole), and carrying the
        // tick budget over made the second dig "time out" instantly
        // (measured 2026-11-17: smelt failed at BreakFurnace).
        if (!(digPos_ == cell) || digBlockId_ != id) {
            digPos_ = cell; digBlockId_ = id; digTicks_ = 0; digLavaChecked_ = false;
        }
        if (id.empty() || isAir(id)) { setAttack(false); digTicks_ = 0; return 1; }
        // A passable decoration cell (leaf litter, grass, ...) is
        // already open space for movement purposes: never swing at it —
        // its hitbox sits at the cell floor and the dig would run out
        // the full timeout without breaking anything.
        if (isPassableCell(id)) { setAttack(false); digTicks_ = 0; return 1; }
        if (!digLavaChecked_) {
            digLavaChecked_ = true;
            if (lavaAdjacent(cell)) { setAttack(false); note("dig: lava-adjacent cell refused"); return 2; }
        }
        if (!ensureTool(localPart(id)) && !toolOptional) { setAttack(false); return 2; }
        bedrock::Vec3 eye;
        if (!eyePos(eye)) return 0;
        const bedrock::Vec3 center{cell.x + .5f, cell.y + .5f, cell.z + .5f};
        // Hold discipline (the tickMine pattern): aim BEFORE
        // the press, then feed nothing while the attack is
        // held, so nothing disturbs the destroy progress.
        // (Overhead digs additionally died in the ascend caller
        // from a held-slot flutter — see tickAscend.)
        if (!attackHeld_) {
            const bool facing = faceToward(eye, center, 5.f);
            if (facing) setAttack(true);
        }
        if (++digTicks_ > 20 * 30) { setAttack(false); digTicks_ = 0; return 2; }
        return 0;
    }
    // Pillar back up the shaft to the surface the expedition
    // descended from: jump, and while airborne place a spare block
    // into the feet cell (aimed at the top face of the block below),
    // land on it, repeat. The same jump-place rhythm as bridging.
    void tickAscend(bedrock::Vec3 feet) {
        ++ascendTicks_;
        const int fy = static_cast<int>(std::floor(feet.y));
        bool arrived = fy >= ascendTargetY_;
        if (arrived && ascendOpenSky_) {
            // Fallback target (the true surface was never recorded
            // this session): y=70 inside a hill is not the surface —
            // keep climbing until the column above is open.
            const int fx = static_cast<int>(std::floor(feet.x));
            const int fz = static_cast<int>(std::floor(feet.z));
            arrived = isAir(blockAt({fx, fy + 3, fz})) &&
                      isAir(blockAt({fx, fy + 8, fz}));
        }
        if (arrived) {
            releaseInputs();
            note("ascend: back at the surface (y=" + std::to_string(fy) + ")");
            reply("地上に戻ったよ。道具を作り直すね");
            hasTarget_ = false;
            if (expeditionActive()) {
                // Route by what can actually be done up here: with
                // no iron for a pickaxe the recovery is to go back
                // down and mine more, not to retry a chain that
                // cannot start. (Measured 2026-11-18: entering
                // IronPick with no materials left phase_ on Ascend
                // and this arrival fired 294 times in a loop.)
                if (expNeedTier_ >= 4 && !hasPickaxeTier(4) &&
                    countAllItems("iron_ingot") < 3 && countAllItems("raw_iron") < 3)
                    enterExpStage(ExpStage::ToIronLevel);
                else
                    enterExpStage(expNeedTier_ >= 4 ? ExpStage::IronPick : ExpStage::StonePick);
                if (phase_ == Phase::Ascend) { phase_ = Phase::Scan; scanCursor_ = 0; bestDist2_ = 1e18; }
            }
            else { phase_ = Phase::Scan; scanCursor_ = 0; bestDist2_ = 1e18; }
            return;
        }
        if (ascendLastY_ == INT_MAX || fy > ascendLastY_) { ascendLastY_ = fy; ascendStuck_ = 0; }
        if (fy >= ascendNoteY_ + 10) { ascendNoteY_ = fy; note("ascend: y=" + std::to_string(fy)); }
        if (++ascendStuck_ > 20 * 15 || ascendTicks_ > 20 * 1800) {
            note("ascend fail: stuck=" + std::to_string(ascendStuck_) +
                 " ticks=" + std::to_string(ascendTicks_) +
                 " fy=" + std::to_string(fy) +
                 " lastY=" + std::to_string(ascendLastY_));
            releaseInputs();
            expFail("地上へ戻れなかった");
            phase_ = Phase::Idle;
            return;
        }
        int slot = spareSlot();
        if (slot >= 0) ascendPullTicks_ = 0; // the pull budget is per shortage, not per climb
        if (slot < 0) {
            // Pull a spare stack up from the main inventory first.
            if (craftReady_ && ascendPullTicks_ < 600) {
                ++ascendPullTicks_;
                setUse(false); pressKey(kKeySpace, false);
                for (const auto& s : craft_.snapshot())
                    if (s.count && s.slot >= 9 && isSpareBlock(localPart(s.item))) {
                        (void)ensureHotbarTick(localPart(s.item));
                        break;
                    }
                return;
            }
            releaseInputs();
            note("ascend: out of spare blocks");
            expFail("足場ブロックが尽きた");
            phase_ = Phase::Idle;
            return;
        }
        void* p = player();
        bedrock::Vec3 eye;
        if (!eyePos(eye)) return;
        // Centre the player inside the column before jumping:
        // with the AABB straddling a wall column, that column's
        // block at fy+2 caps the jump at ~0.2 (measured
        // 2026-11-18: feet y oscillating -52.00/-51.72 for the
        // whole stuck budget, the place window never opened).
        // Burst-walk to the cell centre, view level so the
        // movement keys map to the world axes, sneak off (its
        // edge clamp is what parks the player at the edge).
        if (!ascendCenterGiveUp_) {
            // Sticky target: the climb never changes column, so
            // capture the starting cell once. Recomputing it from
            // the live feet position every tick flips the target
            // by a full block whenever the player straddles a
            // boundary, and the walk chases it back and forth
            // forever (measured 2026-11-18: 301 ticks pressed
            // into a corner, the place section never reached).
            if (!ascendCenterActive_) {
                ascendCenterActive_ = true;
                ascendCenterCell_ = {static_cast<int>(std::floor(feet.x)), fy,
                                     static_cast<int>(std::floor(feet.z))};
                ascendCenterTicks_ = 0;
            }
            const double ctx = ascendCenterCell_.x + 0.5;
            const double ctz = ascendCenterCell_.z + 0.5;
            const double cdx = ctx - feet.x, cdz = ctz - feet.z;
            if (feet.y - fy < 0.05 && cdx * cdx + cdz * cdz > 0.12 * 0.12) {
                if (++ascendCenterTicks_ > 40) {
                    // Cannot settle (pursuit orbit / wedged):
                    // climb anyway — the un-centred climb worked
                    // before centring existed; the wall-cap case
                    // is the rarer one.
                    ascendCenterGiveUp_ = true;
                    ascendCenterActive_ = false;
                    pressKey(kKeyW, false);
                    note("ascend: centering gave up, climbing as-is");
                } else {
                    setUse(false);
                    pressKey(kKeySpace, false);
                    pressKey(kKeySneak, false);
                    faceToward(eye, {static_cast<float>(ctx), eye.y,
                                     static_cast<float>(ctz)}, 5.f);
                    pressKey(kKeyW, true);
                    return;
                }
            } else {
                ascendCenterActive_ = false;
                pressKey(kKeyW, false);
            }
        }
        // A ceiling over the head cell blocks the jump: dig it out
        // first when a tool allows (a weaker spare pickaxe still
        // digs, just slower). Without any tool the jump-place below
        // simply makes no progress and the stuck timer gives up.
        // Standing ticks only: fy is sampled from the live feet
        // position, so at a jump's apex fy reads one higher and
        // the "head" cell is the block above the real ceiling —
        // digging it steals the cycle's placement and the player
        // falls back, forever (measured 2026-11-18: dig entries
        // at fy=-51 interleaved with standing notes at y=-52,
        // zero net rise).
        if (feet.y - fy < 0.05) {
            const int fx = static_cast<int>(std::floor(feet.x));
            const int fz = static_cast<int>(std::floor(feet.z));
            const auto headId = blockAt({fx, fy + 2, fz});
            if (isDiggableCell(headId) && !isPassableCell(headId)) {
                // Dig the ceiling out through tickMine, the
                // proven driver. digCell's own hold never broke
                // an overhead block in this geometry (repeated
                // 600-tick timeouts across three aim/selection
                // variants, 2026-11-18), while tickMine breaks
                // the same blocks in seconds and a real 4 s hold
                // from the same spot broke straight through.
                // tickMine hands control back via the ascendDig_
                // intercepts (completion / timeout / range).
                bool havePick = false;
                for (const auto& s : hotbarItems())
                    if (s.count > 0 && contains(s.id, "pickaxe")) { havePick = true; break; }
                if (havePick) {
                    note("ascend dig: via mine, head=" + headId +
                         " fy=" + std::to_string(fy));
                    ascendDig_ = true;
                    target_ = {fx, fy + 2, fz};
                    targetId_ = localPart(headId);
                    savedId_.clear();
                    hasTarget_ = true;
                    mineTicks_ = 0;
                    setUse(false); pressKey(kKeySpace, false);
                    phase_ = Phase::Mine;
                    return;
                }
                // No tool in the hotbar: the jump-place below
                // makes no progress and the stuck timer gives up.
            }
        }
        // Select the spare stack only now, for the placement
        // below, so the held item stays stable while any dig
        // is in progress.
        if (p && world_.selected(p) != slot) world_.select(p, static_cast<unsigned>(slot));
        // Aim at the centre of the top face of the block under
        // the feet CELL. The old aim (body centre + 0.3) crosses
        // into the neighbouring cell whenever the player stands
        // past the cell's middle — on a pillar at a cavern edge
        // the use then targets a face over the void, no block is
        // ever placed, and the climb dies on the stuck timer
        // (measured 2026-11-18: a climb rose -52 -> ~0 digging
        // every ceiling block, then placed nothing for 15 s).
        const int pfx = static_cast<int>(std::floor(feet.x));
        const int pfz = static_cast<int>(std::floor(feet.z));
        const bedrock::Vec3 aim{pfx + .5f, static_cast<float>(fy), pfz + .5f};
        faceToward(eye, aim, 3.f);
        // Sneak the whole climb: it clamps the player to the pillar
        // top's edge, so a placement that lands a hair early cannot
        // eject them sideways off the 1x1 column (measured
        // 2026-11-18: the bot repeatedly fell off its own pillar).
        pressKey(kKeySneak, true);
        // Hold jump and use continuously: the game places the
        // block the moment the jump clears the feet cell, and
        // auto-jumps again on landing. The earlier 12-tick pulse
        // cycle (1-tick jump, use on ticks 3-9) proved too
        // fragile under this client's variable tick rate —
        // measured 2026-11-18: whole climbs at a shaft bottom
        // placing nothing for minutes while every ceiling dig
        // succeeded.
        pressKey(kKeySpace, true);
        setUse(true);
        if ((ascendTicks_ % 20) == 0)
            note("ascend tick: y=" + std::to_string(feet.y) +
                 " below=" + blockAt({pfx, fy - 1, pfz}) +
                 " head=" + blockAt({pfx, fy + 2, pfz}) +
                 " slot=" + std::to_string(p ? world_.selected(p) : -1));
    }
    void tickDescend(bedrock::Vec3 feet) {
        if (feet.y <= descendTargetY_) {
            setAttack(false); descendDone_ = true; phase_ = Phase::Idle;
            note("descend: reached y=" + std::to_string(static_cast<int>(feet.y)));
            return;
        }
        // Drowning guard: water pockets chained by the drop-in rule
        // can turn the shaft into a dive far longer than one breath
        // (measured 2026-11-18: the descent drowned in an aquifer
        // system, 24 drop-ins in). Track continuous head submersion
        // and surface for air before it runs out.
        {
            const BlockPos3 headCell{static_cast<int>(std::floor(feet.x)),
                                     static_cast<int>(std::floor(feet.y + 1.62)),
                                     static_cast<int>(std::floor(feet.z))};
            const bool headWet = contains(localPart(blockAt(headCell)), "water");
            if (descendBreath_) {
                setAttack(false);
                pressKey(kKeySpace, true);
                if (!headWet || ++descendBreathTicks_ > 20 * 6) {
                    pressKey(kKeySpace, false);
                    descendBreath_ = false; descendBreathTicks_ = 0; descendWetTicks_ = 0;
                    note("descend: breath break over");
                }
                return;
            }
            if (headWet) {
                if (++descendWetTicks_ > 20 * 8) {
                    descendBreath_ = true; descendBreathTicks_ = 0;
                    digPos_ = {INT_MIN, 0, 0}; digBlockId_.clear(); digTicks_ = 0;
                    note("descend: surfacing for air");
                    return;
                }
            } else descendWetTicks_ = 0;
        }
        const int fy = static_cast<int>(std::floor(feet.y));
        if (fy < descendLastY_) { descendLastY_ = fy; descendStuck_ = 0; }
        if (descendEntryY_ == INT_MAX) {
            descendEntryY_ = fy;
            // Only the first descent of a session records the
            // surface: a cave-resume descent starts underground and
            // must not overwrite it (the surface-return rescue
            // climbs back to this height).
            if (lastSurfaceY_ == INT_MAX) lastSurfaceY_ = fy;
        }
        // Two blocks into the shaft the walls confine the player:
        // centring is only needed at the surface, and a wide hole
        // only until the player has dropped in (measured 2026-11-17:
        // on slopes the surface centring never settled and every
        // descent fell back to wide for its whole depth).
        const bool inShaft = fy <= descendEntryY_ - 2;
        const int cx = static_cast<int>(std::floor(feet.x + 0.3));
        const int cz = static_cast<int>(std::floor(feet.z + 0.3));
        // 1x1 shaft (user request 2026-11-17): centre the player fully
        // inside one column and dig only that column — half the blocks,
        // half the time of the footprint-wide dig. If centring does not
        // settle, fall back to the wide dig for the rest of this run:
        // digging only the AABB-min corner's column leaves the player
        // standing on the neighbouring column while the shaft deepens
        // beside them (measured 2026-11-16 in a swamp).
        const bool singleColumn =
            static_cast<int>(std::floor(feet.x)) == static_cast<int>(std::floor(feet.x + 0.599)) &&
            static_cast<int>(std::floor(feet.z)) == static_cast<int>(std::floor(feet.z + 0.599));
        if (!descendWide_ && !singleColumn && !inShaft) {
            setAttack(false);
            const double tx = cx + 0.2, tz = cz + 0.2;
            const double dx = tx - feet.x, dz = tz - feet.z;
            // Burst walking: holding W to a tight point tolerance
            // overshoots and circles the target forever (measured
            // 2026-11-17: centring never settled in 6 s and every
            // descent fell back to wide). Walk only while clearly
            // short of the centre point, then coast; any stop within
            // 0.15 of it is single-column by construction (the window
            // is 0.4 wide), which the loop guard above re-checks.
            if (dx * dx + dz * dz > 0.15 * 0.15) {
                bedrock::Vec3 eye;
                if (eyePos(eye)) faceToward(eye, {static_cast<float>(tx), feet.y,
                                                  static_cast<float>(tz)}, 0.05f);
                pressKey(kKeyW, true);
            } else {
                pressKey(kKeyW, false);
            }
            if (++descendCenterTicks_ > 20 * 3) {
                pressKey(kKeyW, false);
                descendWide_ = true;
                note("descend: centering failed, digging wide");
            }
            return;
        }
        pressKey(kKeyW, false);
        descendCenterTicks_ = 0;
        std::vector<BlockPos3> cols;
        cols.push_back({cx, fy - 1, cz});
        if ((descendWide_ || !singleColumn) && !inShaft) {
            const int x0 = static_cast<int>(std::floor(feet.x));
            const int x1 = static_cast<int>(std::floor(feet.x + 0.599));
            const int z0 = static_cast<int>(std::floor(feet.z));
            const int z1 = static_cast<int>(std::floor(feet.z + 0.599));
            for (int x = x0; x <= x1; ++x)
                for (int z = z0; z <= z1; ++z)
                    if (x != cx || z != cz) cols.push_back({x, fy - 1, z});
        }
        for (const auto& c : cols) {
            if (contains(localPart(blockAt(c)), "lava")) {
                descendDone_ = true; descendFailed_ = true; phase_ = Phase::Idle;
                note("descend: lava directly below");
                return;
            }
        }
        // Cave guard: after the supporting blocks break the player
        // falls one cell; if the void under the centre column then
        // continues 3+ cells, the fall becomes a cavern drop — stop
        // and let the hunt take over here. A SHALLOW opening is the
        // exception: when a floor is visible inside the probe
        // window (drop of at most 7, a few hearts at worst), or the
        // void is water (a safe landing at any depth), drop in and
        // keep shafting from the cave floor instead of abandoning
        // the descent far above the target band (measured
        // 2026-11-18: the diamond descent stopped at a cave at y=52
        // and the hunt scanned that height forever).
        int voidDepth = 0;
        std::string voidFloor;
        bool voidWater = false;
        for (int y = fy - 2; y > fy - 9; --y) {
            const auto id = blockAt({cx, y, cz});
            if (id.empty() || isAir(id)) ++voidDepth;
            else if (contains(localPart(id), "water")) { ++voidDepth; voidWater = true; }
            else { voidFloor = localPart(id); break; }
        }
        // Water in the void changes the call: a drop into water is
        // safe at any depth, so an aquifer under the shaft is a
        // drop-in too, not a cavern stop (measured 2026-11-18: the
        // probe read a water column as a floorless void and the
        // descent surrendered above an underground lake).
        if (voidDepth >= 3 && voidFloor != "lava" && (!voidFloor.empty() || voidWater)) {
            if (descendDropNoteY_ != fy) {
                descendDropNoteY_ = fy;
                note("descend: opening below (" +
                     (voidWater && voidFloor.empty()
                          ? std::string("water")
                          : "floor " + std::to_string(voidDepth + 1) + " down") +
                     "), dropping in");
            }
            // Fall through to the dig loop: breaking the centre
            // column drops the player onto the floor.
        } else if (voidDepth >= 3) {
            setAttack(false);
            descendDone_ = true; descendCave_ = true; phase_ = Phase::Idle;
            note("descend: cave opening below, stopping the shaft");
            return;
        }
        bool refused = false;
        for (const auto& c : cols) {
            const auto id = blockAt(c);
            if (id.empty() || isAir(id) || contains(localPart(id), "water")) continue;
            if (digCell(c, feet) == 2) { refused = true; continue; }
            descendStuck_ = 0;
            return; // working on this column
        }
        if (refused) {
            descendDone_ = true; descendFailed_ = true; phase_ = Phase::Idle;
            note("descend: dig refused below");
            return;
        }
        // Nothing solid under the footprint: falling (or swimming).
        setAttack(false);
        if (++descendStuck_ > 20 * 20) {
            descendDone_ = true; descendFailed_ = true; phase_ = Phase::Idle;
            note("descend: no progress, giving up");
        }
    }
    void tickHunt(bedrock::Vec3 feet) {
        const int fx = static_cast<int>(std::floor(feet.x));
        const int fy = static_cast<int>(std::floor(feet.y));
        const int fz = static_cast<int>(std::floor(feet.z));
        const BlockPos3 ahead{fx + huntDirX_, fy, fz + huntDirZ_};
        if (huntStage_ == 0) {
            // No floor ahead means a cave edge or water: end the leg and
            // let the supervisor pick a new direction.
            const auto floorId = blockAt({ahead.x, fy - 1, ahead.z});
            if (floorId.empty() || isAir(floorId) || contains(localPart(floorId), "water") ||
                contains(localPart(floorId), "lava")) {
                releaseInputs(); huntLegDone_ = true; huntBlocked_ = true; phase_ = Phase::Idle;
                note("hunt: no floor ahead, leg over");
                return;
            }
            const BlockPos3 head{ahead.x, fy + 1, ahead.z};
            const auto headId = blockAt(head);
            if (headId.empty() || isAir(headId)) { huntStage_ = 1; return; }
            if (digCell(head, feet) != 0) huntStage_ = 1;
            return;
        }
        if (huntStage_ == 1) {
            const auto footId = blockAt(ahead);
            if (footId.empty() || isAir(footId)) { huntStage_ = 2; huntTicks_ = 0; return; }
            if (digCell(ahead, feet) != 0) { huntStage_ = 2; huntTicks_ = 0; }
            return;
        }
        // Advance one cell into the cleared space.
        bedrock::Vec3 eye;
        if (!eyePos(eye)) return;
        const bedrock::Vec3 want{ahead.x + .5f, feet.y + 1.0f, ahead.z + .5f};
        faceToward(eye, want, 0.05f);
        pressKey(kKeyW, true);
        const int nx = static_cast<int>(std::floor(feet.x));
        const int nz = static_cast<int>(std::floor(feet.z));
        if ((nx == ahead.x && nz == ahead.z) || ++huntTicks_ > 70) {
            pressKey(kKeyW, false);
            huntStage_ = 0;
            if (huntTicks_ > 70) { // could not advance: treat as blocked
                huntLegDone_ = true; huntBlocked_ = true; phase_ = Phase::Idle;
                note("hunt: advance blocked, leg over");
                return;
            }
            if (++huntCells_ >= 6) {
                huntLegDone_ = true; phase_ = Phase::Idle;
                note("hunt: leg complete");
            }
        }
    }

    // -- smelting -------------------------------------------------------------
    // Craft a furnace, place it, load it through adapter transfers
    // (input container 25 slot 0, fuel 24 slot 1 — response-verified),
    // wait out the burn with the screen closed, then break the furnace
    // and step onto the drops: the result stack's network id is never
    // revealed to the adapter, so a forged take from the result slot
    // (26/2) is not possible, but a broken furnace drops everything.
    enum class SmeltStage { CraftFurnace, PlaceFurnace, OpenFurnace, LoadInput,
                            LoadFuel, CloseUi, WaitSmelt, BreakFurnace, CollectDrops,
                            Done, Failed };
    static std::string smeltYieldOf(const std::string& item) {
        if (item == "raw_iron") return "iron_ingot";
        if (item == "raw_gold") return "gold_ingot";
        if (item == "raw_copper") return "copper_ingot";
        const std::string cooked = cookedMeatOf(item);
        if (!cooked.empty()) return cooked;
        return item;
    }
    int findHotbarSlot(const std::string& localId) {
        const auto slots = hotbarItems();
        for (int i = 0; i < 9 && i < static_cast<int>(slots.size()); ++i)
            if (slots[static_cast<std::size_t>(i)].count > 0 &&
                localPart(slots[static_cast<std::size_t>(i)].id) == localId)
                return i;
        return -1;
    }
    std::optional<bedrock::CraftInventorySlot> findInvItem(const std::string& localId) {
        for (const auto& s : craft_.snapshot())
            if (s.count > 0 && localPart(s.item) == localId) return s;
        return std::nullopt;
    }
    void startSmelt(const std::string& item, int count) {
        smeltItem_ = item; smeltCount_ = count;
        smeltYieldItem_ = smeltYieldOf(item);
        smeltYieldBase_ = countAllItems(smeltYieldItem_);
        smeltStage_ = SmeltStage::CraftFurnace; smeltTicks_ = 0;
        smeltPlaced_ = false; smeltPlaceCell_ = {INT_MIN, 0, 0}; smeltAimedTick_ = -1;
        smeltPocketing_ = false; smeltPocketDug_ = false; smeltPocketBad_ = 0;
        smeltPocket_ = {INT_MIN, 0, 0};
        smeltPlaceCells_.clear(); smeltPlaceIdx_ = 0; smeltCellTicks_ = 0;
        smeltTransferStarted_ = false; smeltUseAltCont_ = false; smeltMoved_ = 0;
        smeltFuelBurnTicks_ = 0; smeltFuelPendingBurn_ = 0; smeltFuelBurnPer_ = 0;
        smeltExpect_ = count;
        smeltFurnaceGhost_ = false; smeltFailReason_.clear();
        furnaceChainTried_ = false; smeltCollectTries_ = 0; smeltBreakRetries_ = 0;
        smeltDone_ = false; smeltFailed_ = false;
        phase_ = Phase::Smelt;
        note("smelt: start " + item + " x" + std::to_string(count));
    }
    void smeltAdvance() {
        smeltStage_ = static_cast<SmeltStage>(static_cast<int>(smeltStage_) + 1);
        smeltTicks_ = 0; smeltAimedTick_ = -1; smeltTransferStarted_ = false;
        smeltUseAltCont_ = false;
        smeltPlaceArmed_ = false; smeltNoSlotRetries_ = 0; smeltNoSlotTicks_ = 0;
        smeltPlaceSlotIdx_ = 0; smeltPlaceSlotTicks_ = 0;
        setUse(false); pressKey(kKeyS, false);
    }
    void smeltFail(const std::string& why) {
        smeltFailReason_ = why;
        note("smelt failed: " + why);
        reply("精錬に失敗したよ（" + why + "）");
        setUse(false); setAttack(false); releaseInputs();
        smeltFailed_ = true; smeltStage_ = SmeltStage::Failed; phase_ = Phase::Idle;
    }
    // -- forage: hunt animals, cook the meat, eat ----------------------
    // /mine food [N]: hunt N pieces of meat from passive animals,
    // cook them with the smelt machinery, then eat. The profile has
    // no hunger read, so the meal is a fixed size (up to 2 pieces);
    // if a held piece is not consumed the player was already full,
    // which still counts as success.
    void startForage(int want) {
        resetRun();
        forageActive_ = true;
        forageWant_ = std::clamp(want, 1, 16);
        forageStage_ = ForageStage::Hunt;
        forageEid_ = 0; forageType_ = 0; forageEngaged_ = false;
        forageTicks_ = 0; forageKills_ = 0; forageCollectTicks_ = 0;
        forageNoAnimalTicks_ = 0; forageLastHitTick_ = 0; forageIgnore_.clear();
        forageSmeltStarted_ = false;
        forageTriedRaw_.clear();
        forageRawId_.clear(); forageCookedId_.clear();
        forageEatStep_ = 0; forageEatTicks_ = 0; forageEatBase_ = 0; forageEaten_ = 0;
        phase_ = Phase::Forage;
        reply("食料を調達するね（肉" + std::to_string(forageWant_) + "個が目標）");
        note("forage: start want=" + std::to_string(forageWant_));
    }
    void forageFail(const std::string& why) {
        note("forage failed: " + why);
        reply("食料調達はここまで（" + why + "）");
        forageActive_ = false;
        releaseInputs(); setUse(false); setAttack(false);
        if (phase_ == Phase::Forage) phase_ = Phase::Idle;
    }
    void forageDone() {
        note("forage: done kills=" + std::to_string(forageKills_) +
             " eaten=" + std::to_string(forageEaten_));
        reply("狩って焼いて食べたよ（" + std::to_string(forageKills_) + "頭狩って、" +
              std::to_string(forageEaten_) + "個食べた）");
        forageActive_ = false;
        releaseInputs(); setUse(false); setAttack(false);
        if (phase_ == Phase::Forage) phase_ = Phase::Idle;
    }
    int forageRawTotal() {
        return countAllItems("beef") + countAllItems("porkchop") +
               countAllItems("chicken") + countAllItems("mutton");
    }
    void tickForage(const bedrock::Vec3& feet) {
        if (!forageActive_) return;
        ++forageTicks_;
        if (forageTicks_ > 20 * 60 * 10) { forageFail("時間切れ"); return; }
        const bedrock::Vec3 eye{feet.x, feet.y + 1.62f, feet.z};
        if (forageStage_ == ForageStage::Hunt) {
            if (attackPulse_ > 0 && --attackPulse_ == 0) setAttack(false);
            if (attackCd_ > 0) --attackCd_;
            if (forageRawTotal() >= forageWant_) {
                releaseInputs();
                forageStage_ = ForageStage::Cook; forageSmeltStarted_ = false;
                note("forage: enough raw meat, cooking");
                return;
            }
            if (forageEid_) {
                // No landed hit for 20 s: the animal is unreachable
                // (measured 2026-11-18: a pig fled into a tree canopy
                // and the straight-line chase pressed into the leaves
                // forever). Drop the lock and ignore it for a minute.
                if (forageTicks_ - forageLastHitTick_ > 20 * 20) {
                    note("forage: animal unreachable, moving on");
                    forageIgnore_[forageEid_] = forageTicks_ + 20 * 60;
                    forageEid_ = 0; forageEngaged_ = false;
                    releaseInputs();
                    return;
                }
                auto mob = scanPassive(feet, 48.0, forageEid_);
                if (mob) {
                    forageCentre_ = mob->center;
                    BlockPos3 hitCell{};
                    const BlockPos3 mobCell{static_cast<int>(std::floor(mob->center.x)),
                                            static_cast<int>(std::floor(mob->center.y)),
                                            static_cast<int>(std::floor(mob->center.z))};
                    const bool los = lineOfSight(eye, mobCell, hitCell);
                    const double reach = distToMob(eye, *mob);
                    const bool wet = inWaterNow(feet);
                    if (reach > 2.9 || !los) {
                        faceAim(eye, mob->center, 5.f, .35f);
                        pressKey(kKeyW, true);
                        pressKey(kKeySprint, mob->dist > 6.0);
                        pressKey(kKeySpace, wet || mob->center.y > feet.y + .6f);
                    } else {
                        pressKey(kKeyW, false); pressKey(kKeySprint, false);
                        pressKey(kKeySpace, wet);
                        faceToward(eye, mob->center, 3.f);
                        if (attackCd_ == 0) {
                            const Weapon w = bestMelee();
                            if (w.slot >= 0) selectSlot(w.slot);
                            setAttack(true);
                            attackPulse_ = 2; attackCd_ = 11;
                            forageEngaged_ = true;
                            forageLastHitTick_ = forageTicks_;
                        }
                    }
                    return;
                }
                // The animal is gone (killed, or fled out of range):
                // walk onto its last spot so the drops get picked up.
                forageEid_ = 0;
                if (forageEngaged_) ++forageKills_;
                forageEngaged_ = false;
                forageCollectTicks_ = 50;
                note("forage: animal down, collecting drops");
                return;
            }
            if (forageCollectTicks_ > 0) {
                --forageCollectTicks_;
                const double dx = forageCentre_.x - feet.x;
                const double dz = forageCentre_.z - feet.z;
                if (dx * dx + dz * dz > 1.2) {
                    faceAim(eye, forageCentre_, 5.f, .35f);
                    pressKey(kKeyW, true);
                    pressKey(kKeySpace, inWaterNow(feet));
                } else {
                    pressKey(kKeyW, false);
                }
                if (forageCollectTicks_ == 0) pressKey(kKeyW, false);
                return;
            }
            for (auto it = forageIgnore_.begin(); it != forageIgnore_.end();) {
                if (it->second <= forageTicks_) it = forageIgnore_.erase(it);
                else ++it;
            }
            if (auto mob = scanPassive(feet, 48.0)) {
                if (forageIgnore_.count(mob->eid)) {
                    // Only the ignored animal is around: walkabout
                    // away instead of re-locking it (fall through to
                    // the walkabout below by pretending none was seen).
                } else {
                forageEid_ = mob->eid; forageType_ = mob->type;
                forageCentre_ = mob->center; forageEngaged_ = false;
                forageLastHitTick_ = forageTicks_;
                forageNoAnimalTicks_ = 0;
                note("forage: hunting type=" + std::to_string(mob->type) +
                     " (" + rawMeatOf(mob->type) + ")");
                return;
                }
            }
            // No animal in range: if some meat is already in hand and
            // a full minute of searching found nothing more, cook
            // what there is instead of walking forever.
            if (++forageNoAnimalTicks_ > 20 * 60 && forageRawTotal() > 0) {
                releaseInputs();
                forageStage_ = ForageStage::Cook; forageSmeltStarted_ = false;
                note("forage: no more animals nearby, cooking what we have");
                return;
            }
            // Walkabout: keep moving in the hunt direction (turning
            // every ~8 s), hopping onto single blocks in the way.
            if ((forageTicks_ % 160) == 0) {
                const int ndx = huntDirZ_, ndz = -huntDirX_;
                huntDirX_ = ndx; huntDirZ_ = ndz;
            }
            const bedrock::Vec3 ahead{feet.x + huntDirX_ * 8.f, feet.y,
                                      feet.z + huntDirZ_ * 8.f};
            faceAim(eye, ahead, 5.f, .3f);
            pressKey(kKeyW, true);
            const BlockPos3 fwdCell{static_cast<int>(std::floor(feet.x)) + huntDirX_,
                                    static_cast<int>(std::floor(feet.y)),
                                    static_cast<int>(std::floor(feet.z)) + huntDirZ_};
            const bool blocked = isSolidCell(localPart(blockAt(fwdCell)));
            const bool headroom =
                !isSolidCell(localPart(blockAt({fwdCell.x, fwdCell.y + 1, fwdCell.z})));
            pressKey(kKeySpace, blocked && headroom);
            return;
        }
        if (forageStage_ == ForageStage::Cook) {
            // The smelt machinery places a furnace and stands beside
            // it; that fails in open water (measured 2026-11-18: a
            // hunt that ended in a pond cycled every placement cell
            // and gave up). Swim ashore first, turning every ~5 s
            // until the feet are dry.
            if (inWaterNow(feet)) {
                if ((forageTicks_ % 100) == 0) {
                    const int ndx = huntDirZ_, ndz = -huntDirX_;
                    huntDirX_ = ndx; huntDirZ_ = ndz;
                }
                const bedrock::Vec3 ahead{feet.x + huntDirX_ * 8.f, feet.y + 1.f,
                                          feet.z + huntDirZ_ * 8.f};
                faceAim(eye, ahead, 5.f, .3f);
                pressKey(kKeyW, true);
                pressKey(kKeySpace, true);
                return;
            }
            pressKey(kKeyW, false);
            pressKey(kKeySpace, false);
            if (!forageSmeltStarted_) {
                // Cook the largest raw stack (one kind per smelt run),
                // skipping kinds already exposed as inventory ghosts.
                static const char* kRaws[4] = {"beef", "porkchop", "chicken", "mutton"};
                int bestCount = 0;
                for (const char* r : kRaws) {
                    if (forageTriedRaw_.count(r)) continue;
                    const int c = countAllItems(r);
                    if (c > bestCount) { bestCount = c; forageRawId_ = r; }
                }
                if (bestCount <= 0) { forageFail("肉が無かった"); return; }
                forageCookedId_ = cookedMeatOf(forageRawId_);
                startSmelt(forageRawId_, std::min(bestCount, forageWant_));
                forageSmeltStarted_ = true;
                return;
            }
            if (smeltDone_) {
                smeltDone_ = false;
                forageStage_ = ForageStage::Eat;
                forageEatStep_ = 0; forageEatTicks_ = 0;
                phase_ = Phase::Forage;
                note("forage: cooked " + forageCookedId_);
                return;
            }
            if (smeltFailed_) {
                smeltFailed_ = false;
                if (smeltFailReason_.find("材料") != std::string::npos &&
                    !forageRawId_.empty()) {
                    // The chosen meat was a ghost in the inventory
                    // view (counted, but the furnace load found
                    // nothing): try the next kind instead of giving
                    // up on the whole forage.
                    forageTriedRaw_.insert(forageRawId_);
                    forageSmeltStarted_ = false;
                    note("forage: " + forageRawId_ +
                         " was a ghost; trying another meat");
                    return;
                }
                forageFail("焼けなかった"); return;
            }
            return; // the smelt machinery owns the phase until it lands in Idle
        }
        // Eat.
        if (moveStage_ != MoveStage::None) { (void)ensureHotbarTick(moveItem_); return; }
        const int cooked = countAllItems(forageCookedId_);
        if (cooked <= 0 || forageEaten_ >= 2) { forageDone(); return; }
        int slot = -1;
        const auto bar = hotbarItems();
        for (int i = 0; i < 9; ++i) {
            const auto& it = bar[static_cast<std::size_t>(i)];
            if (it.count && localPart(it.id) == forageCookedId_) { slot = i; break; }
        }
        if (slot < 0) {
            if (!moveFailed_) (void)ensureHotbarTick(forageCookedId_);
            else forageFail("焼けた肉をホットバーに移せなかった");
            return;
        }
        selectSlot(slot);
        if (forageEatStep_ == 0) {
            forageEatBase_ = cooked; forageEatStep_ = 1; forageEatTicks_ = 0;
            setUse(true);
            return;
        }
        if (++forageEatTicks_ >= 45) {
            setUse(false);
            const int now = countAllItems(forageCookedId_);
            forageEatStep_ = 0;
            if (now < forageEatBase_) {
                ++forageEaten_;
                note("forage: ate " + forageCookedId_ +
                     " (" + std::to_string(forageEaten_) + ")");
            } else {
                // Nothing consumed in 45 ticks of holding use: the
                // hunger bar is full. That is a successful meal state.
                note("forage: not hungry, keeping the rest");
                forageDone();
            }
        }
    }
    bool tryStartFurnaceChain() {
        if (!craftReady_ || !craft_.recipesReady() || !craft_.ready() || chainActive_) return false;
        if (craft_.status() == bedrock::CraftJobStatus::Running) return false;
        craft_.clearStatus();
        const auto have = chainCounts();
        if (countIn(have, "cobblestone") < 8 && countIn(have, "cobbled_deepslate") < 8 &&
            countIn(have, "blackstone") < 8)
            return false;
        std::vector<ChainStep> steps;
        // A placed table nearby is reused; otherwise craft a fresh one
        // from planks on the spot (the iron hunt ends far from the
        // surface camp, so the original table is out of reach).
        // placedTableExists() also gates on reach: a table left on the
        // surface still exists as a block, but its screen cannot be
        // opened from the bottom of the iron shaft, and the craft then
        // dies with status 49 on the first grid placement (measured
        // 2026-11-17 on a fresh world). This chain once open-coded the
        // existence check without the distance gate.
        const bool localTable = placedTableExists();
        if (!localTable) {
            if (countIn(have, "crafting_table") >= 1) {
                // A table item is already carried (e.g. an earlier
                // chain crafted it and the hotbar move delivered it):
                // just place it, no new craft needed.
                steps.push_back(ChainStep{ChainStep::Kind::PlaceTable});
            } else {
                const int planks = countIn(have, "planks");
                steps.push_back(ChainStep{ChainStep::Kind::OpenInventory});
                if (planks < 4) {
                    // No planks for a table: mill them from logs on
                    // the spot first (same pattern as the pickaxe
                    // chain) instead of refusing to start.
                    int logCount = 0;
                    const std::string wood = chainWood(have, logCount);
                    const int logCrafts = (4 - planks + 3) / 4;
                    if (wood.empty() || logCrafts > logCount) return false;
                    steps.push_back(ChainStep{ChainStep::Kind::Craft,
                                              "minecraft:" + wood + "_planks",
                                              "minecraft:" + wood + "_planks", logCrafts, false});
                }
                steps.push_back(ChainStep{ChainStep::Kind::Craft, "minecraft:WorkBench_recipeId",
                                          "minecraft:crafting_table", 1, false});
                steps.push_back(ChainStep{ChainStep::Kind::CloseUi});
                steps.push_back(ChainStep{ChainStep::Kind::PlaceTable});
            }
        }
        steps.push_back(ChainStep{ChainStep::Kind::OpenTable});
        steps.push_back(ChainStep{ChainStep::Kind::Craft, "minecraft:furnace",
                                  "minecraft:furnace", 1, true});
        steps.push_back(ChainStep{ChainStep::Kind::CloseUi});
        chainSteps_ = std::move(steps);
        chainStep_ = 0; chainActive_ = true; chainJobStarted_ = false;
        chainPlaceCell_ = {INT_MIN, 0, 0};
        chainPlaceCells_.clear(); chainPlaceIdx_ = 0; placeCellTicks_ = 0;
        chainPocketing_ = false; chainPocketDug_ = false; chainPocketBad_ = 0; chainPocket_ = {INT_MIN, 0, 0};
        chainStepTicks_ = 0; chainAimedTick_ = -1; chainRetriedOpen_ = false;
        chainRecoverTicks_ = -1; chainPendingFail_.clear();
        chainWaitingCobble_ = false;
        chainForPickaxe_ = false; chainForFurnace_ = true; chainReturn_ = Phase::Smelt;
        phase_ = Phase::Craft;
        note("smelt: furnace craft chain (" + std::to_string(chainSteps_.size()) + " steps)");
        return true;
    }
    // One transfer stage (input or fuel). Returns true when the stage is
    // finished (advanced or failed) so the caller stops ticking it.
    bool tickSmeltTransfer(std::uint32_t dstContainer, int dstSlot, bool isFuel) {
        if (!smeltTransferStarted_) {
            std::optional<bedrock::CraftInventorySlot> src;
            int want = 0;
            if (isFuel) {
                // Fuel is topped up until the accumulated burn
                // covers the input (coal 1600 ticks per item,
                // planks/logs 300 — 1.5 items each). The old code
                // moved a fixed count from the FIRST matching stack
                // only: a split stack under-fuelled the furnace, the
                // cook stopped early, and the post-break yield check
                // failed on ingots that were never made (measured
                // 2026-11-18, twice in a row).
                const int needTicks = smeltMoved_ * 200 - smeltFuelBurnTicks_;
                if (needTicks <= 0) {
                    note("smelt: fuel loaded");
                    smeltAdvance();
                    return true;
                }
                src = findInvItem("coal");
                smeltFuelBurnPer_ = 1600;
                if (!src) {
                    src = findInvItemPred([](const std::string& id) {
                        return id.size() > 7 && id.compare(id.size() - 7, 7, "_planks") == 0;
                    });
                    smeltFuelBurnPer_ = 300;
                }
                if (!src) {
                    src = findInvItemPred([](const std::string& id) {
                        return id.size() > 4 && id.compare(id.size() - 4, 4, "_log") == 0;
                    });
                    smeltFuelBurnPer_ = 300;
                }
                if (!src) {
                    if (smeltFuelBurnTicks_ > 0) {
                        smeltExpect_ = std::min(smeltMoved_, smeltFuelBurnTicks_ / 200);
                        note("smelt: fuel short, cooking " + std::to_string(smeltExpect_) +
                             " of " + std::to_string(smeltMoved_));
                        smeltAdvance();
                        return true;
                    }
                    smeltFail("燃料が無い"); return true;
                }
                want = (needTicks + smeltFuelBurnPer_ - 1) / smeltFuelBurnPer_;
            } else {
                src = findInvItem(smeltItem_);
                if (!src) { smeltFail("精錬する材料が無い"); return true; }
                want = smeltCount_;
            }
            const int n = std::min(want, src->count);
            if (!isFuel) smeltMoved_ = n;
            else smeltFuelPendingBurn_ = n * smeltFuelBurnPer_;
            const std::uint32_t cont = smeltUseAltCont_ ? 12u
                                       : (src->slot < 9 ? 28u : 29u);
            if (!craft_.startTransfer(cont, src->slot, src->netId, dstContainer, dstSlot, n,
                                      src->item))
                return false; // adapter busy; retry next tick
            smeltTransferStarted_ = true;
            return false;
        }
        const auto st = craft_.transferStatus();
        if (st == bedrock::CraftJobStatus::Running) return false;
        if (st == bedrock::CraftJobStatus::Done) {
            craft_.clearTransferStatus();
            if (isFuel) {
                smeltFuelBurnTicks_ += smeltFuelPendingBurn_;
                smeltFuelPendingBurn_ = 0;
                if (smeltFuelBurnTicks_ < smeltMoved_ * 200) {
                    // More burn needed: loop the stage for another
                    // stack instead of advancing.
                    smeltTransferStarted_ = false;
                    note("smelt: fuel topped up (burn " +
                         std::to_string(smeltFuelBurnTicks_) + " ticks)");
                    return false;
                }
            } else {
                // The yield target follows what was really loaded.
                smeltExpect_ = smeltMoved_;
            }
            note(std::string("smelt: ") + (isFuel ? "fuel" : "input") + " loaded");
            smeltAdvance();
            return true;
        }
        // Failed: the screen-open inventory may be addressed as the plain
        // container 12 instead of 28/29 — retry once with the other form.
        craft_.clearTransferStatus();
        if (!smeltUseAltCont_) {
            smeltUseAltCont_ = true; smeltTransferStarted_ = false;
            note("smelt: transfer failed, retrying with container 12");
            return false;
        }
        smeltFail("アイテム移動に失敗");
        return true;
    }
    std::optional<bedrock::CraftInventorySlot> findInvItemPred(
        const std::function<bool(const std::string&)>& pred) {
        for (const auto& s : craft_.snapshot())
            if (s.count > 0 && pred(localPart(s.item))) return s;
        return std::nullopt;
    }
    void tickSmelt(bedrock::Vec3 feet) {
        ++smeltTicks_;
        switch (smeltStage_) {
        case SmeltStage::CraftFurnace: {
            if (smeltFurnaceGhost_) {
                // A counted furnace turned out to be a ghost in the
                // inventory view (tickPlaceFurnace could not find it
                // in the hotbar after a completed move): craft a real
                // one even though the count still claims one exists.
                if (chainActive_) return;
                if (!furnaceChainTried_) {
                    furnaceChainTried_ = true;
                    if (tryStartFurnaceChain()) return;
                    smeltFail("かまどを作れなかった");
                    return;
                }
                if (countAllItems("furnace") >= 1) {
                    smeltFurnaceGhost_ = false;
                    smeltAdvance();
                } else if (smeltTicks_ > 20 * 30) smeltFail("かまどを作れなかった");
                return;
            }
            if (countAllItems("furnace") >= 1) { smeltAdvance(); return; }
            if (chainActive_) return; // the furnace chain is running (phase Craft)
            if (!furnaceChainTried_) {
                furnaceChainTried_ = true;
                if (tryStartFurnaceChain()) return;
            }
            if (smeltTicks_ > 20 * 30) smeltFail("かまどを作れなかった");
            return; }
        case SmeltStage::PlaceFurnace: tickPlaceFurnace(feet); return;
        case SmeltStage::OpenFurnace: tickOpenFurnace(feet); return;
        case SmeltStage::LoadInput: (void)tickSmeltTransfer(25, 0, false); return;
        case SmeltStage::LoadFuel: (void)tickSmeltTransfer(24, 1, true); return;
        case SmeltStage::CloseUi:
            if (game_.gameplayActive()) { smeltAdvance(); return; }
            if (smeltTicks_ == 1 || smeltTicks_ == 30) pressKey(0x1B, true);
            if (smeltTicks_ == 3 || smeltTicks_ == 32) pressKey(0x1B, false);
            if (smeltTicks_ > 70) smeltAdvance();
            return;
        case SmeltStage::WaitSmelt:
            if (smeltTicks_ > smeltMoved_ * 200 + 150) smeltAdvance();
            return;
        case SmeltStage::BreakFurnace: {
            const int r = digCell(smeltFurnacePos_, feet);
            if (r == 1) { note("smelt: furnace broken, collecting"); smeltAdvance(); }
            else if (r == 2) {
                // A refused dig here is usually stale dig state or a
                // transient tool-read failure, not a truly unbreakable
                // furnace: reset and retry a few times before failing.
                if (++smeltBreakRetries_ <= 3) {
                    digPos_ = {INT_MIN, 0, 0}; digBlockId_.clear(); digTicks_ = 0;
                    note("smelt: break refused, retrying");
                } else smeltFail("かまどを壊せなかった");
            }
            return; }
        case SmeltStage::CollectDrops: {
            // The yield target is what was actually loaded and
            // cookable (smeltExpect_), not the stage's request: a
            // partial input load or a short fuel load otherwise
            // makes success unreachable by construction.
            const int gained = countAllItems(smeltYieldItem_) - smeltYieldBase_;
            if (gained >= smeltExpect_) {
                pressKey(kKeyW, false);
                reply("精錬完了！" + smeltYieldItem_ + " が " +
                      std::to_string(gained) + "個できたよ");
                note("smelt: done, " + std::to_string(gained) + "x " + smeltYieldItem_);
                smeltDone_ = true; smeltStage_ = SmeltStage::Done; phase_ = Phase::Idle;
                return;
            }
            // The drops from a broken furnace scatter around its
            // cell and the pickup can lag the walk: sweep the cell
            // with walk/pause cycles for a generous window instead
            // of three fixed 25-tick lunges (measured 2026-11-18:
            // the ingots existed but the old window closed before
            // the count caught up, twice in a row).
            const bedrock::Vec3 eye{feet.x, feet.y + 1.62f, feet.z};
            const bedrock::Vec3 center{smeltFurnacePos_.x + .5f, feet.y + .5f,
                                       smeltFurnacePos_.z + .5f};
            faceAim(eye, center, 12.f, 0.3f);
            pressKey(kKeyW, (smeltTicks_ % 40) < 25);
            if (smeltTicks_ > 240) {
                pressKey(kKeyW, false);
                note("smelt diag: collect shortfall, gained=" + std::to_string(gained) +
                     " expect=" + std::to_string(smeltExpect_) +
                     " base=" + std::to_string(smeltYieldBase_));
                smeltFail("精錬物を取りこぼした");
            }
            return; }
        case SmeltStage::Done: case SmeltStage::Failed:
            phase_ = Phase::Idle; return;
        }
    }
    void tickPlaceFurnace(bedrock::Vec3 feet) {
        // Same neighbour re-check as tickPlaceTable: a furnace placed
        // by an earlier pulse may sit in a cell this step no longer
        // watches; adopt any reachable furnace next to the player.
        {
            bedrock::Vec3 eye0;
            if (eyePos(eye0)) {
                const int fx = static_cast<int>(std::floor(feet.x));
                const int fy = static_cast<int>(std::floor(feet.y));
                const int fz = static_cast<int>(std::floor(feet.z));
                for (int dy = 1; dy >= -1; --dy) {
                const BlockPos3 nbs[8] = {{fx + 1, fy + dy, fz}, {fx - 1, fy + dy, fz},
                                          {fx, fy + dy, fz + 1}, {fx, fy + dy, fz - 1},
                                          {fx + 1, fy + dy, fz + 1}, {fx + 1, fy + dy, fz - 1},
                                          {fx - 1, fy + dy, fz + 1}, {fx - 1, fy + dy, fz - 1}};
                for (const auto& nb : nbs) {
                    if (!contains(localPart(blockAt(nb)), "furnace")) continue;
                    BlockPos3 occ;
                    if (!lineOfSight(eye0, nb, occ)) continue;
                    smeltFurnacePos_ = nb; smeltPlaced_ = true;
                    note("smelt: adopting nearby furnace");
                    smeltAdvance();
                    return;
                }
                }
            }
        }
        if (smeltTicks_ > 20 * 25) { smeltFail("かまどを置けなかった"); return; }
        if (anyScreenUp() && moveStage_ == MoveStage::None) {
            // A leftover inventory screen eats every use pulse, so
            // placement "fails" for all candidates (measured 2026-11-17
            // on a fresh world: the hotbar move's CloseUi gave up with
            // the screen still open and all 12 candidates cycled).
            // Phase::Smelt is exempt from tick()'s screen recovery, so
            // close it here before touching candidates. Skipped while
            // a hotbar move runs — its inventory screen is not a
            // leftover (see tickPlaceTable's guard).
            if (tickCount_ % 30 == 1) pressKey(0x1B, true);
            if (tickCount_ % 30 == 3) pressKey(0x1B, false);
            return;
        }
        if (smeltPocketing_) {
            const int r = digPocketTick(feet, smeltPocket_, smeltPocketBad_, smeltPocketStage_);
            if (r == 2) { smeltPocketing_ = false; smeltFail("かまどを置く場所が無い"); return; }
            if (r == 1) { smeltPocketing_ = false; smeltPocketDug_ = true; smeltPocketCell_ = smeltPocket_; }
            else return;
        }
        bedrock::Vec3 eye;
        if (!eyePos(eye)) return;
        if (smeltPlaceCell_.x == INT_MIN) {
            const int fx = static_cast<int>(std::floor(feet.x));
            const int fy = static_cast<int>(std::floor(feet.y));
            const int fz = static_cast<int>(std::floor(feet.z));
            std::array<BlockPos3, 4> dirs{{{1, 0, 0}, {-1, 0, 0}, {0, 0, 1}, {0, 0, -1}}};
            smeltPlaceCells_.clear();
            for (const auto& d : dirs) {
                const BlockPos3 cell{fx + d.x, fy, fz + d.z};
                if (cell.x < feet.x + .31 && cell.x + 1 > feet.x - .31 &&
                    cell.z < feet.z + .31 && cell.z + 1 > feet.z - .31 &&
                    !(smeltPocketDug_ && cell.x == smeltPocketCell_.x &&
                      cell.y == smeltPocketCell_.y && cell.z == smeltPocketCell_.z))
                    continue;
                const auto at = blockAt(cell);
                if (contains(localPart(at), "furnace")) {
                    // Same shadowing rule as the table adoption: a
                    // furnace the use ray cannot reach opens the
                    // wrong screen and bricks the smelt.
                    BlockPos3 occ;
                    if (!lineOfSight(eye, cell, occ)) continue;
                    smeltFurnacePos_ = cell; smeltPlaced_ = true;
                    note("smelt: adopting existing furnace");
                    smeltAdvance();
                    return;
                }
                const auto ground = blockAt({cell.x, cell.y - 1, cell.z});
                if (ground.empty() || isAir(ground) || contains(localPart(ground), "water") ||
                    (!isAir(at) && !isPassableCell(at) && !contains(localPart(at), "water")))
                    continue;
                // Same gravity refill as the table cells (measured
                // 2026-11-17: the smelt failed right after the pocket
                // note when the furnace cell sat under gravel).
                if (isGravityBlock(localPart(blockAt({cell.x, cell.y + 1, cell.z}))))
                    continue;
                BlockPos3 occluder;
                const bool isPocketCell = smeltPocketDug_ && cell.x == smeltPocketCell_.x &&
                                          cell.y == smeltPocketCell_.y &&
                                          cell.z == smeltPocketCell_.z;
                if (!isPocketCell &&
                    !lineOfSight(eye, {cell.x, cell.y - 1, cell.z}, occluder))
                    continue;
                smeltPlaceCells_.push_back(cell);
            }
            if (smeltPlaceCells_.empty()) {
                if (smeltPocketDug_) { smeltFail("かまどを置く場所が無い"); return; }
                smeltPocketing_ = true;
                smeltPocket_ = {INT_MIN, 0, 0};
                smeltPocketStage_ = 0;
                note("smelt: digging a pocket for the furnace");
                return;
            }
            smeltPlaceIdx_ = 0;
            smeltCellTicks_ = 0;
            smeltPlaceCell_ = smeltPlaceCells_[0];
        }
        // (furnace placement shares the table placement pattern)
        if (contains(localPart(blockAt(smeltPlaceCell_)), "furnace")) {
            smeltFurnacePos_ = smeltPlaceCell_; smeltPlaced_ = true;
            note("smelt: furnace placed");
            smeltAdvance();
            return;
        }
        {
            // Same refill guard as the table placement.
            const auto atNow = blockAt(smeltPlaceCell_);
            if (!isAir(atNow) && !isPassableCell(atNow) && !contains(localPart(atNow), "water")) {
                note("smelt: place cell refilled, re-collecting");
                smeltPlaceCell_ = {INT_MIN, 0, 0};
                return;
            }
        }
        int slot = findHotbarSlot("furnace");
        // A hotbar move in flight must be driven to completion first:
        // its Bring transfer already updates the shadow, so
        // findHotbarSlot() succeeds while the move still sits in
        // CloseUi with the inventory screen open — and pulses sent
        // with a screen open are eaten (measured 2026-11-17, fresh
        // world run 2: the move was orphaned mid-CloseUi and every
        // candidate cycled). Only a fully idle move may be skipped.
        if (moveStage_ != MoveStage::None) {
            if (!ensureHotbarTick("furnace")) return;
            slot = findHotbarSlot("furnace");
        } else if (slot < 0 && countAllItems("furnace") >= 1 && !moveFailed_) {
            if (!ensureHotbarTick("furnace")) return;
            slot = findHotbarSlot("furnace");
        }
        if (slot < 0) {
            // Same flicker tolerance + one forced move re-plan as
            // tickPlaceTable (see there for the measured failure).
            if (++smeltNoSlotTicks_ <= 100) return;
            if (smeltNoSlotRetries_ < 1 && countAllItems("furnace") >= 1) {
                ++smeltNoSlotRetries_;
                smeltNoSlotTicks_ = 0;
                moveFailed_ = false;
                moveStage_ = MoveStage::None;
                note("smelt: furnace counted but no hotbar slot; resetting the move and retrying");
                return;
            }
            if (!smeltFurnaceGhost_) {
                // The count claims a furnace exists but no hotbar
                // slot ever showed it: the counted stack is a ghost
                // (measured 2026-11-18 after a death/respawn desync).
                // Go back and craft a real one instead of failing.
                smeltFurnaceGhost_ = true;
                furnaceChainTried_ = false;
                smeltStage_ = SmeltStage::CraftFurnace; smeltTicks_ = 0;
                smeltNoSlotTicks_ = 0; smeltNoSlotRetries_ = 0;
                moveFailed_ = false;
                note("smelt: counted furnace not in hotbar (ghost?); crafting one");
                return;
            }
            moveFailed_ = false; smeltFail("かまどがホットバーに無い"); return;
        }
        smeltNoSlotTicks_ = 0;
        if (!smeltPlaceArmed_) {
            // Same budget decoupling as tickPlaceTable: the clock
            // starts when the furnace is actually in hand.
            smeltPlaceArmed_ = true;
            smeltTicks_ = 0;
        }
        // Same claimed-slot rotation as tickPlaceTable.
        slot = placeSlotTick("furnace", smeltPlaceSlotIdx_, smeltPlaceSlotTicks_, slot);
        selectSlot(slot);
        const bedrock::Vec3 aim{smeltPlaceCell_.x + .5f,
                                static_cast<float>(smeltPlaceCell_.y),
                                smeltPlaceCell_.z + .5f};
        faceToward(eye, aim, 3.f);
        pressKey(kKeyS, smeltPlaceCell_.x < feet.x + .31f && smeltPlaceCell_.x + 1 > feet.x - .31f &&
                        smeltPlaceCell_.z < feet.z + .31f && smeltPlaceCell_.z + 1 > feet.z - .31f);
        // Held use, not a 2-in-12 pulse — same fix as the table
        // placement (see tickPlaceTable).
        setUse(true);
        if (++smeltCellTicks_ > 30 && smeltPlaceCells_.size() > 1) {
            smeltCellTicks_ = 0;
            smeltPlaceIdx_ = (smeltPlaceIdx_ + 1) % smeltPlaceCells_.size();
            smeltPlaceCell_ = smeltPlaceCells_[smeltPlaceIdx_];
            note("smelt: trying another furnace cell");
        }
        if ((smeltTicks_ % 100) == 50) {
            note("place diag: furnace cell=" + std::to_string(smeltPlaceCell_.x) + "," +
                 std::to_string(smeltPlaceCell_.y) + "," + std::to_string(smeltPlaceCell_.z) +
                 " at=" + localPart(blockAt(smeltPlaceCell_)) +
                 " ground=" + localPart(blockAt({smeltPlaceCell_.x, smeltPlaceCell_.y - 1,
                                                 smeltPlaceCell_.z})) +
                 " above=" + localPart(blockAt({smeltPlaceCell_.x, smeltPlaceCell_.y + 1,
                                                smeltPlaceCell_.z})) +
                 " slot=" + std::to_string(slot) +
                 " top=" + (screenReady_ ? topScreenName() : std::string("?")));
        }
    }
    void tickOpenFurnace(bedrock::Vec3 feet) {
        if (!smeltPlaced_) { smeltFail("かまどが無い"); return; }
        if (smeltTicks_ > 20 * 6) { smeltFail("かまどを開けなかった"); return; }
        bedrock::Vec3 eye;
        if (!eyePos(eye)) return;
        const bedrock::Vec3 center{smeltFurnacePos_.x + .5f, smeltFurnacePos_.y + .5f,
                                   smeltFurnacePos_.z + .5f};
        BlockPos3 occ;
        if (!lineOfSight(eye, smeltFurnacePos_, occ)) {
            if (contains(localPart(blockAt(occ)), "furnace")) {
                note("smelt: adopting the furnace in the aim path");
                smeltFurnacePos_ = occ;
                smeltAimedTick_ = -1;
                return;
            }
            note("smelt: furnace shadowed by " + localPart(blockAt(occ)) +
                 "; re-placing");
            smeltPlaced_ = false;
            smeltFail("かまどが他のブロックに隠れている");
            return;
        }
        if (smeltAimedTick_ < 0) {
            if (faceToward(eye, center, 3.f)) smeltAimedTick_ = smeltTicks_;
            return;
        }
        const int since = smeltTicks_ - smeltAimedTick_;
        setUse(since < 2);
        if (!game_.gameplayActive()) { setUse(false); smeltAdvance(); return; }
        // Same blind-advance trap as tickOpenTable: re-aim and pulse
        // again; the step timeout fails the smelt if it never opens.
        if (since > 15) smeltAimedTick_ = -1;
    }

    // -- expedition supervisor --------------------------------------------------
    // From-zero quest: when the normal flow stalls in NeedTool because a
    // material is missing, the supervisor takes over and produces it —
    // gather wood, chain-craft up to a stone pickaxe, descend and hunt
    // for iron ore, smelt it, chain-craft the iron pickaxe, then descend
    // to diamond depth and hunt the goal ore. Each stage hands control
    // back to the ordinary machinery (chains, gathers, mining) and just
    // watches for its finish line.
    enum class ExpStage { None, Wood, StonePick, ToIronLevel, HuntIron, Cobble,
                          Smelt, IronPick, ToDiamondLevel, HuntOre };
    bool expeditionActive() const { return expStage_ != ExpStage::None; }
    // Start the from-zero expedition when the ordinary flow is stuck on
    // a missing material or tool. Returns true when it took over.
    bool maybeStartExpedition() {
        if (expeditionActive() || chainActive_) return false;
        if (classForBlock(targetId_) != ToolClass::Pickaxe) return false;
        needTier_ = requiredTier(targetId_);
        const auto have = chainCounts();
        if (needTier_ >= 4 && countIn(have, "iron_ingot") < 3 && !hasPickaxeTier(4)) {
            startExpedition(hasPickaxeTier(2) ? ExpStage::ToIronLevel : ExpStage::Wood);
            return true;
        }
        if (needTier_ >= 4 && !hasPickaxeTier(4) && countIn(have, "iron_ingot") >= 3) {
            // Already holding smelted iron (a previous run's smelt
            // yield, collected since): go straight to the iron
            // pickaxe chain. Without this branch the starter fell
            // through to false and the quest deadlocked in Scan
            // (measured 2026-11-17 on fresh2: iron_ingot=3, no
            // tier-4 pickaxe, expedition never started).
            startExpedition(ExpStage::IronPick);
            return true;
        }
        if (!hasPickaxeTier(1)) {
            const int wood = countIn(have, "_log") + countIn(have, "_wood") +
                             countIn(have, "planks") / 4;
            if (wood < 3) { startExpedition(ExpStage::Wood); return true; }
        }
        return false;
    }
    bool hasPickaxeTier(int minTier) {
        for (const auto& s : craft_.snapshot()) {
            if (!s.count) continue;
            int t{};
            if (classForItem(localPart(s.item), t) == ToolClass::Pickaxe && t >= minTier)
                return true;
        }
        return false;
    }
    void startExpedition(ExpStage stage) {
        expStage_ = stage;
        expNeedTier_ = needTier_;
        expGoalItem_ = goalItem_; expGoalCount_ = goalCount_;
        huntDirX_ = 1; huntDirZ_ = 0; huntBlocked_ = false;
        smeltRetries_ = 0;
        reply("材料を自分で集めるところからやるね");
        note("expedition: start (needTier " + std::to_string(expNeedTier_) + ")");
        enterExpStage(stage);
    }
    void expFail(const std::string& why) {
        note("expedition failed: " + why);
        reply("自律調達はここまで（" + why + "）");
        expStage_ = ExpStage::None;
        gatherActive_ = false;
        endScanOverride();
        releaseInputs();
        if (phase_ != Phase::Craft) phase_ = Phase::Idle;
    }
    void enterExpStage(ExpStage stage) {
        expStage_ = stage;
        note("expedition: stage " + std::to_string(static_cast<int>(stage)));
        switch (stage) {
        case ExpStage::Wood: {
            descendDivertUsed_ = false; descendPending_ = false;
            static const std::vector<std::string> kLogs = {
                "oak_log", "birch_log", "spruce_log", "jungle_log", "acacia_log",
                "dark_oak_log", "mangrove_log", "cherry_log", "pale_oak_log"};
            reply("まず木を集めるね");
            startGather(kLogs, kLogs, 20, true);
            return; }
        case ExpStage::StonePick:
            needTier_ = 2; wantPickaxeChain_ = true; chainRetries_ = 0;
            chainNextRetryTick_ = 0;
            (void)tryStartPickaxeChain();
            return;
        case ExpStage::ToIronLevel:
        case ExpStage::ToDiamondLevel: {
            descendCaveRetries_ = 0; descendResumePending_ = false;
            // Advance judgment before a long descent: if the best
            // pickaxe is nearly worn out and no fresh spare exists,
            // craft one first (the descend waits in tickExpedition);
            // and make sure a weapon is in the hotbar before cave
            // stages. Both are best-effort — if preparation cannot
            // run, the descent proceeds as before.
            const int needTier = stage == ExpStage::ToIronLevel ? 2 : 4;
            const int threshold = stage == ExpStage::ToIronLevel ? 60 : 120;
            if (!descendDivertUsed_) {
                const int best = bestPickaxeRemaining(needTier);
                if (best >= 0 && best < threshold && !hasSparePickaxe(needTier)) {
                    descendDivertUsed_ = true;
                    needTier_ = needTier; wantPickaxeChain_ = true;
                    if (tryStartPickaxeChain(true)) {
                        descendPending_ = true;
                        note("expedition: pickaxe durability low (~" +
                             std::to_string(best) + " left); crafting a fresh one first");
                        reply("ツルハシがボロボロだから、先に新しいのを作るね");
                        return;
                    }
                }
            }
            ensureSwordPrep();
            if (chainActive_) { descendPending_ = true; return; } // sword chain first
            beginDescend(stage);
            return; }
        case ExpStage::HuntIron:
            reply("鉄鉱石を探すね");
            startGather({"iron_ore", "deepslate_iron_ore"}, {"raw_iron"}, 3, true);
            return;
        case ExpStage::Cobble:
            startGather({"stone"}, {"cobblestone"}, 8, true);
            return;
        case ExpStage::Smelt:
            reply("鉄を精錬するね");
            startSmelt("raw_iron", 3);
            return;
        case ExpStage::IronPick:
            needTier_ = 4; wantPickaxeChain_ = true; chainRetries_ = 0;
            chainNextRetryTick_ = 0;
            (void)tryStartPickaxeChain();
            return;
        case ExpStage::HuntOre: {
            reply("ダイヤを探すね");
            const int want = expGoalCount_ > 0 ? expGoalCount_ : 10;
            startGather({"diamond_ore", "deepslate_diamond_ore"}, {"diamond"}, want, true);
            return; }
        case ExpStage::None:
            return;
        }
    }
    void beginDescend(ExpStage stage) {
        const int target = stage == ExpStage::ToIronLevel ? 16 : -53;
        descendTargetY_ = target;
        descendDone_ = false; descendFailed_ = false; descendCave_ = false;
        descendLastY_ = INT_MAX; descendStuck_ = 0; digPos_ = {INT_MIN, 0, 0};
        descendWide_ = false; descendCenterTicks_ = 0;
        descendWetTicks_ = 0; descendBreathTicks_ = 0; descendBreath_ = false;
        pressKey(kKeySpace, false);
        descendEntryY_ = INT_MAX; // captured on the first descend tick
        phase_ = Phase::Descend;
        reply(stage == ExpStage::ToIronLevel ? "鉄を探しに降りるね" : "ダイヤ層まで降りるね");
    }
    // A cavern stop far above the target band is not arrival: the
    // hunt would scan a depth where the ore never spawns (measured
    // 2026-11-18: the diamond descent stopped at a cavern at y=52
    // and stage 9 hunted horizontally at that height forever).
    // Relocate with a hunt leg and shaft again from there; the
    // retry cap hands over to the hunt as before.
    bool descendCaveResume(ExpStage stage, bedrock::Vec3 feet) {
        if (!descendCave_) return false;
        const int fy = static_cast<int>(std::floor(feet.y));
        if (fy <= descendTargetY_ + 16) return false;
        if (descendCaveRetries_ >= 6) return false;
        ++descendCaveRetries_;
        descendDone_ = false; descendCave_ = false;
        descendResumePending_ = true;
        huntStage_ = 0; huntCells_ = 0; huntLegDone_ = false; huntBlocked_ = false;
        digPos_ = {INT_MIN, 0, 0};
        phase_ = Phase::Hunt;
        note("expedition: cavern stop at y=" + std::to_string(fy) + ", still above the y=" +
             std::to_string(descendTargetY_) + " band; relocating and descending again");
        return true;
    }
    // While a cavern relocation leg runs, watch for its end and
    // restart the descent from the new spot. Returns true when the
    // stage case is fully handled for this tick.
    bool descendResumeTick(ExpStage stage) {
        if (!descendResumePending_) return false;
        if (huntLegDone_ && phase_ == Phase::Idle) {
            huntLegDone_ = false;
            if (huntBlocked_) {
                huntBlocked_ = false;
                const int ndx = huntDirZ_, ndz = -huntDirX_;
                huntDirX_ = ndx; huntDirZ_ = ndz;
            }
            descendResumePending_ = false;
            beginDescend(stage);
        }
        return true;
    }
    void tickExpedition(bedrock::Vec3 feet) {
        if (!expeditionActive()) return;
        // Gather outcomes are stage transitions wherever they land.
        if (gatherDone_ || gatherFailed_) {
            const bool ok = gatherDone_;
            gatherDone_ = gatherFailed_ = false;
            if (!ok) { expFail("素材が集まらなかった"); return; }
            switch (expStage_) {
            case ExpStage::Wood: enterExpStage(ExpStage::StonePick); return;
            case ExpStage::HuntIron:
                if (countAllItems("cobblestone") >= 8) enterExpStage(ExpStage::Smelt);
                else enterExpStage(ExpStage::Cobble);
                return;
            case ExpStage::Cobble: enterExpStage(ExpStage::Smelt); return;
            case ExpStage::HuntOre: {
                // The goal check in tick() normally celebrates first;
                // if the gather line was crossed between its polls, do
                // it here and disarm the goal so it cannot double-fire.
                if (expGoalCount_ > 0) {
                    reply("目標達成！ " + expGoalItem_ + " を " +
                          std::to_string(expGoalCount_) + " 個集めたよ");
                    note("expedition: goal reached");
                    goalCount_ = 0;
                }
                expStage_ = ExpStage::None;
                phase_ = Phase::Idle; hasTarget_ = false;
                return; }
            default: return;
            }
        }
        // Hopeless-underground rescue, independent of which
        // sub-state notices the missing tool first (the hunt's
        // tunnel digs fail silently without entering NeedTool, and
        // a gather with no reachable ore never tries to mine).
        if (tickCount_ % 20 == 0 && !chainActive_ && !ascendDig_ &&
            (expStage_ == ExpStage::HuntIron || expStage_ == ExpStage::HuntOre) &&
            phase_ != Phase::Ascend && phase_ != Phase::Craft &&
            phase_ != Phase::Smelt && phase_ != Phase::Descend) {
            const int need = expStage_ == ExpStage::HuntIron ? 2 : 4;
            const int fy = static_cast<int>(std::floor(feet.y));
            // The surface this session descended from; when the run
            // was resumed at depth after a restart it was never
            // recorded — fall back to y=70 + an open-sky check.
            const int surfY = lastSurfaceY_ == INT_MAX ? 70 : lastSurfaceY_;
            if (fy < surfY - 8 && !hasPickaxeTier(need) &&
                !hasSparePickaxe(need)) {
                needTier_ = need; wantPickaxeChain_ = true;
                if (!tryStartPickaxeChain() && totalSpareCount() >= surfY - fy) {
                    releaseInputs();
                    gatherActive_ = false;
                    phase_ = Phase::Ascend; ascendTicks_ = 0;
                    ascendLastY_ = INT_MAX; ascendStuck_ = 0; ascendPullTicks_ = 0;
                    ascendTargetY_ = surfY;
                    ascendOpenSky_ = lastSurfaceY_ == INT_MAX;
                    ascendNoteY_ = INT_MIN;
                    ascendDig_ = false;
                    ascendCenterActive_ = false; ascendCenterGiveUp_ = false;
                    reply("道具が作れないから、地上に戻って仕切り直すね");
                    note("ascend: returning to surface y=" + std::to_string(surfY));
                    return;
                }
            }
        }
        if (gatherActive_ || chainActive_) return;
        switch (expStage_) {
        case ExpStage::StonePick:
            if (hasPickaxeTier(2)) {
                enterExpStage(expNeedTier_ >= 4 ? ExpStage::ToIronLevel : ExpStage::None);
                if (expStage_ == ExpStage::None) { phase_ = Phase::Scan; }
            } else if (tickCount_ % 20 == 0) {
                needTier_ = 2; wantPickaxeChain_ = true;
                (void)tryStartPickaxeChain();
            }
            return;
        case ExpStage::ToIronLevel:
            if (descendResumeTick(ExpStage::ToIronLevel)) return;
            if (descendPending_) {
                if (!chainActive_) { descendPending_ = false; beginDescend(ExpStage::ToIronLevel); }
                return;
            }
            if (descendDone_) {
                if (descendCaveResume(ExpStage::ToIronLevel, feet)) return;
                enterExpStage(ExpStage::HuntIron);
            }
            return;
        case ExpStage::Smelt:
            if (smeltDone_) { smeltDone_ = false; enterExpStage(ExpStage::IronPick); }
            else if (smeltFailed_) {
                smeltFailed_ = false;
                if (++smeltRetries_ <= 1) enterExpStage(ExpStage::Smelt);
                else expFail("精錬できなかった");
            }
            return;
        case ExpStage::IronPick:
            if (hasPickaxeTier(4)) enterExpStage(ExpStage::ToDiamondLevel);
            else if (tickCount_ % 20 == 0) {
                needTier_ = 4; wantPickaxeChain_ = true;
                (void)tryStartPickaxeChain();
            }
            return;
        case ExpStage::ToDiamondLevel:
            if (descendResumeTick(ExpStage::ToDiamondLevel)) return;
            if (descendPending_) {
                if (!chainActive_) { descendPending_ = false; beginDescend(ExpStage::ToDiamondLevel); }
                return;
            }
            if (descendDone_) {
                if (descendCaveResume(ExpStage::ToDiamondLevel, feet)) return;
                enterExpStage(ExpStage::HuntOre);
            }
            return;
        default:
            return;
        }
    }


    // -- members -----------------------------------------------------------------
    bedrock::NativeItemStackRequests craft_;
    bool craftReady_{};
    std::string craftProfileJson_;
    bedrock::NativeGameAccess access_;
    bedrock::GameApi game_{access_};
    bedrock::NativeWorldAccess world_;
    bedrock::NativeRuntimeActors runtimeActors_;
    bedrock::NativeActorRegistry actorRegistry_;
    bedrock::NativeActorLayout actorLayout_{};
    std::size_t actorTypeOffset_{0};
    bool actorsReady_{false};
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
    std::atomic<void*> level_{};
    std::atomic<std::uint64_t> epoch_{1};
    std::atomic<bool> stopped_{};
    bool sehStopped_{};
    std::atomic<bool> crashTestPending_{};
    bool enabled_{true};
    int radius_{24};

    std::mutex logMutex_;
    std::deque<std::string> logs_;
    std::set<unsigned> heldKeys_;
    bool attackHeld_{};
    bool useHeld_{};
    // combat state
    Phase resumePhase_{Phase::Scan};
    CombatMode combatMode_{CombatMode::Melee};
    std::uint32_t combatEid_{}; std::uint32_t combatType_{};
    int combatTicks_{}; int attackCd_{}; int attackPulse_{};
    int retreatTicks_{}; bool retreatAfterHit_{};
    int bowDraw_{}; int bowCd_{}; int landedHits_{};
    int threatScanTick_{};
    int lastEngageTick_{};
    long long tickCount_{};
    std::unordered_map<std::uint32_t, long long> combatIgnore_;
    // boat state
    BoatStep boatStep_{BoatStep::Deploy};
    int boatTicks_{}; int boatStepTicks_{}; int boatPulse_{}; int boatCd_{}; int boatLockTicks_{};
    std::uint32_t boatEid_{};
    std::vector<std::uint32_t> preBoatEids_;
    int boatGlueTicks_{}; int boatProbeTicks_{};
    bedrock::Vec3 boatProbeStart_{};
    bedrock::Vec3 boatCenter_{};
    BlockPos3 boatTriedFor_{INT_MIN, 0, 0};
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
    std::vector<bedrock::Vec3> pathWalk_, pathDigLine_, pathBridgeLine_;
    bool pathBox_{};
    BlockPos3 pathBoxPos_{};
    bool showPath_{true};
    bool probePending_{false};
    bool skipUnderwater_{true};
    int submergedTicks_{0};
    int joinGuardTicks_{}, joinDryTicks_{};
    bool joinGuardNoted_{};
    bool joinArmPending_{true};
    bool joinSurfaced_{};
    std::string lastIdleScreen_;
    bedrock::Vec3 lastAlivePos_{};
    bool hasLastAlivePos_{};
    int deathSigTicks_{};
    bool deathPending_{}, hasDeathPos_{};
    bedrock::Vec3 deathPos_{};
    bool recoverActive_{};
    int recoverTicks_{}, recoverSweepTicks_{}, recoverStuckTicks_{};
    bedrock::Vec3 recoverLastPos_{};
    int escapeTicks_{0};
    int dryTicks_{0};
    struct HazardZone { int x, y, z, r; };
    std::vector<HazardZone> hazardZones_;
    std::vector<BlockPos3> path_;
    std::vector<bool> pathDig_;
    std::vector<bool> pathBridge_;
    int bridgeTicks_{};
    int bridgeFailStreak_{};
    BlockPos3 bridgeFailFor_{INT_MIN, 0, 0};
    int bridgeJumpTicks_{};
    double bridgeLastX_{1e18};
    double bridgeLastZ_{};
    int bridgeStuckTicks_{};
    int planSpares_{};
    std::set<BlockPos3> bridgeBlocked_;
    std::size_t pathIdx_{0};
    bool pathDigResume_{};
    bool pathPartial_{};
    int jumpHoldTicks_{};
    int jumpBaseY_{INT_MIN};
    bool sprintWasOn_{};
    std::unordered_set<CellKey, CellKeyHash> planBlocked_;
    BlockPos3 planBlockedFor_{INT_MIN, 0, 0};
    BlockPos3 pathFor_{INT_MIN, 0, 0};
    BlockPos3 planFailFor_{INT_MIN, 0, 0};
    int planFailTick_ = -100000;
    int replans_{0};
    int swimSubmerged_{0};
    bool surfacing_{false};
    bool wasInWater_{false};
    Phase phase_{Phase::Idle};
    std::string targetId_;
    BlockPos3 target_{};
    bool hasTarget_{};
    std::set<BlockPos3> blacklist_;
    int mined_{};
    int goalCount_{};
    int goalBase_{};
    int goalProgress_{};
    std::string goalItem_;
    int scanCursor_{};
    double bestDist2_{1e18};
    int emptyScans_{};
    int escapeStreak_{};
    int scanWait_{};
    int navTicks_{}, stuckTicks_{};
    float lastX_{}, lastZ_{};
    int mineTicks_{}, collectTicks_{}, needTicks_{};
    BlockPos3 subTarget_{};
    std::string subId_;
    BlockPos3 savedTarget_{};
    std::string savedId_;
    // Scan override (gather / cobble wait)
    std::vector<std::string> scanIds_;
    std::string scanFoundId_;
    std::string scanSavedId_;
    bool scanOverrideHeld_{};
    // Gather state
    bool gatherActive_{}, gatherDone_{}, gatherFailed_{}, gatherHunt_{};
    std::vector<std::string> gatherCountIds_;
    int gatherWant_{};
    long long gatherDeadline_{};
    // digCell state
    BlockPos3 digPos_{INT_MIN, 0, 0};
    std::string digBlockId_;
    int digTicks_{};
    bool digLavaChecked_{};
    int smeltBreakRetries_{};
    // Descend state
    int descendTargetY_{};
    bool descendDone_{}, descendFailed_{}, descendCave_{};
    int descendCaveRetries_{};
    bool descendResumePending_{};
    int descendDropNoteY_{INT_MAX};
    int descendWetTicks_{}, descendBreathTicks_{};
    bool descendBreath_{};
    int cobbleNavFails_{};
    bool cobbleDescend_{};
    int descendLastY_{INT_MAX}, descendStuck_{};
    bool descendWide_{};
    int descendCenterTicks_{};
    int descendEntryY_{INT_MAX};
    // Resource management state
    int lastSurfaceY_{INT_MAX};
    bool descendPending_{};
    bool descendDivertUsed_{};
    int ascendTicks_{};
    int ascendLastY_{INT_MAX};
    int ascendStuck_{};
    int ascendPullTicks_{};
    int ascendTargetY_{INT_MAX};
    bool ascendOpenSky_{};
    int ascendNoteY_{INT_MIN};
    bool ascendDig_{};
    bool ascendCenterActive_{};
    bool ascendCenterGiveUp_{};
    BlockPos3 ascendCenterCell_{};
    int ascendCenterTicks_{};
    // Hunt state
    int huntDirX_{1}, huntDirZ_{};
    int huntStage_{}, huntCells_{}, huntTicks_{};
    bool huntLegDone_{}, huntBlocked_{};
    // Smelt state
    SmeltStage smeltStage_{SmeltStage::CraftFurnace};
    std::string smeltItem_, smeltYieldItem_;
    int smeltCount_{}, smeltMoved_{}, smeltYieldBase_{}, smeltTicks_{};
    int smeltFuelBurnTicks_{}, smeltFuelPendingBurn_{}, smeltFuelBurnPer_{}, smeltExpect_{};
    BlockPos3 smeltPlaceCell_{INT_MIN, 0, 0}, smeltFurnacePos_{};
    BlockPos3 smeltPocket_{INT_MIN, 0, 0};
    int smeltPocketBad_{};
    int smeltPocketStage_{};
    bool smeltPocketing_{};
    bool smeltPocketDug_{};
    BlockPos3 smeltPocketCell_{INT_MIN, 0, 0};
    int smeltNoSlotTicks_{};
    bool smeltPlaceArmed_{};
    std::size_t smeltPlaceSlotIdx_{};
    int smeltPlaceSlotTicks_{};
    int smeltNoSlotRetries_{};
    std::vector<BlockPos3> smeltPlaceCells_;
    std::size_t smeltPlaceIdx_{};
    int smeltCellTicks_{};
    bool smeltPlaced_{}, smeltTransferStarted_{}, smeltUseAltCont_{};
    int smeltAimedTick_{-1}, smeltCollectTries_{};
    bool furnaceChainTried_{}, smeltDone_{}, smeltFailed_{};
    bool smeltFurnaceGhost_{};
    std::string smeltFailReason_;
    // Forage (hunt -> cook -> eat) state.
    bool forageActive_{};
    int forageWant_{3};
    ForageStage forageStage_{ForageStage::Hunt};
    std::uint32_t forageEid_{}, forageType_{};
    bedrock::Vec3 forageCentre_{};
    bool forageEngaged_{};
    int forageTicks_{}, forageKills_{}, forageCollectTicks_{}, forageNoAnimalTicks_{};
    int forageLastHitTick_{};
    std::map<std::uint32_t, int> forageIgnore_;
    bool forageSmeltStarted_{};
    std::set<std::string> forageTriedRaw_;
    std::string forageRawId_, forageCookedId_;
    int forageEatStep_{}, forageEatTicks_{}, forageEatBase_{}, forageEaten_{};
    // Expedition state
    ExpStage expStage_{ExpStage::None};
    int expNeedTier_{}, expGoalCount_{};
    std::string expGoalItem_;
    int smeltRetries_{};
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
