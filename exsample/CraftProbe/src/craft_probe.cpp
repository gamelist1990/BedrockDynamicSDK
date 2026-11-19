// CraftProbe — throwaway verification probe for AutoCraft (Thread 37).
//
// Verifies the hand-built native ItemStackRequestPacket layout that was
// reverse engineered on 2026-11-16 (packet 0x50 bytes, RequestData 0x48,
// action variant box 0x60 with the type index at +0x58):
//   - captures CraftingData (52) payloads to CraftingData.bin (PacketAPI)
//   - logs ItemStackResponse (148) payloads and outgoing 147 wire bytes
//   - when PEXData/CraftProbe/go.txt appears, builds one fake packet with a
//     single craft_recipe (mode.txt: "recipe <netId> <times>") or
//     craft_recipe_auto ("auto <netId> <times>") action and dispatches it
//     through BDS_HOOK_PACKET_SEND_TO_SERVER, exactly like
//     NativeCommandRequests does for CommandRequest packets.
#include <bedrock/runtime/game_hooks.hpp>
#include <bedrock/profiles/profile_manager.hpp>
#include <bedrock/profiles/profile_json.hpp>
#include <bedrock/native/packet_sender.hpp>
#include <bedrock/native/world_access.hpp>
#include <bedrock/packets/packet_api.hpp>
#include <bedrock/detail/native_profile_functions.hpp>
#include <bedrock/runtime/hook_runtime.h>
#include <knownfolders.h>
#include <shlobj.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace {
constexpr std::uintptr_t kPacketVtableRva = 0xF15DD30;
constexpr std::uintptr_t kRequestDataVtableRva = 0xF15FA90;

std::string hexBytes(const void* data, std::size_t size) {
    static constexpr char digits[] = "0123456789abcdef";
    const auto* p = static_cast<const unsigned char*>(data);
    std::string out;
    for (std::size_t i = 0; i < size; ++i) {
        out += digits[p[i] >> 4];
        out += digits[p[i] & 15];
        out += ' ';
    }
    return out;
}

class CraftProbeMod {
  public:
    std::atomic<unsigned> status{};
    void signalStop() noexcept { stopped_ = true; }
    void run() noexcept {
        status = 1;
        try {
            PWSTR path{};
            if (FAILED(SHGetKnownFolderPath(FOLDERID_Documents, KF_FLAG_CREATE, nullptr, &path)))
                throw std::runtime_error("Documents directory unavailable");
            directory_ = std::filesystem::path(path) / L"PEXData" / L"CraftProbe";
            CoTaskMemFree(path);
            std::filesystem::create_directories(directory_);
            std::ofstream log(directory_ / L"CraftProbe.log", std::ios::trunc);
            if (!log) throw std::runtime_error("could not open CraftProbe.log");
            initialize();
            while (!stopped_) {
                flush(log);
                std::this_thread::sleep_for(std::chrono::milliseconds{100});
            }
            if (sendToken_) { BDS_HookUnsubscribe(sendToken_); sendToken_ = nullptr; }
            packets_.stop();
            hooks_.reset();
            flush(log);
            status = 3;
        } catch (const std::exception& error) {
            note(std::string("startup failed: ") + error.what());
            status = 4;
        } catch (...) { status = 4; }
    }

  private:
    void note(std::string value) {
        OutputDebugStringA(("[CraftProbe] " + value + "\n").c_str());
        std::lock_guard lock(logMutex_);
        if (logs_.size() < 512) logs_.push_back(std::move(value));
    }
    void flush(std::ostream& log) {
        std::deque<std::string> batch;
        { std::lock_guard lock(logMutex_); batch.swap(logs_); }
        for (const auto& value : batch) log << value << '\n';
        if (!batch.empty()) log.flush();
    }
    void initialize() {
        bedrock::ProfileManager profiles;
        std::string error;
        const bedrock::ProfileOptions options{bedrock::UpdatePolicy::RefreshIfNeeded,
                                              bedrock::CandidatePolicy::AllowCandidates};
        if (!profiles.prepare(
                {bedrock::Feature::Gameplay, bedrock::Feature::LevelTick, bedrock::Feature::World,
                 bedrock::Feature::Actors}, options,
                [this](const bedrock::RuntimeProfile& profile, std::string& failure) {
                    try {
                        profileJson_ = profile.json();
                        const auto root = bedrock::parseNativeProfile(profileJson_);
                        senderLayout_.load(root);
                        base_ = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
                        try {
                            world_.load(profileJson_);
                            worldReady_ = true;
                        } catch (const std::exception& e) {
                            note(std::string("world access unavailable: ") + e.what());
                        }
                        loadItems(root);
                        return true;
                    } catch (const std::exception& e) { failure = e.what(); return false; }
                },
                error))
            throw std::runtime_error(error);
        // PacketAPI handlers must be registered before install (Thread 32).
        packets_.onReceive(bedrock::PacketId::CraftingData,
                           [this](const bedrock::PacketReadEvent& event) {
                               auto bytes = event.ownedPayload();
                               {
                                   std::lock_guard lock(dataMutex_);
                                   craftingData_ = bytes;
                               }
                               // A fresh CraftingData means a new world join:
                               // the response-tracked inventory overlay from a
                               // previous session is invalid (stacks restack).
                               shadowInv_.clear();
                               note("CraftingData received: " +
                                    std::to_string(bytes->size()) + " bytes");
                           });
        packets_.onReceive(bedrock::PacketId::ItemStackResponse,
                           [this](const bedrock::PacketReadEvent& event) {
                               const auto payload = event.payload();
                               const std::size_t take = std::min<std::size_t>(payload.size(), 96);
                               note("ItemStackResponse (" + std::to_string(payload.size()) +
                                    " bytes): " + hexBytes(payload.data(), take));
                               auto owned = event.ownedPayload();
                               if (owned) parseResponse(*owned);
                           });
        packets_.onSend(bedrock::PacketId::ItemStackRequest,
                        [this](bedrock::PacketWireSendEvent& event) {
                            const auto payload = event.packet().payload();
                            const std::size_t take = std::min<std::size_t>(payload.size(), 128);
                            note("wire 147 (" + std::to_string(payload.size()) +
                                 " bytes): " + hexBytes(payload.data(), take));
                        });
        if (!packets_.installReceive(profileJson_, error))
            note("installReceive failed: " + error);
        else
            note("PacketAPI receive installed");
        if (!packets_.installSend(profileJson_, error))
            note("installSend failed: " + error);
        else
            note("PacketAPI send installed");
        game_.Event.Enum.Gameplay.then([this](const bedrock::GameplayEvent& event) {
            if (event.clientInstance) client_.store(event.clientInstance);
        });
        (void)0; // send-hook subscription happens in observeSender()
        game_.Event.Enum.LevelTick.then([this](const bedrock::LevelTickEvent& event) {
            if (stopped_) return;
            level_.store(event.level);
            observeSender();
            persistCraftingData();
            checkTrigger();
            flowTick();
        });
        const auto profile = profiles.snapshot();
        hooks_ = std::make_unique<bedrock::GameHooks>(game_);
        if (!hooks_->installGameplay(profile->resolver(), error) ||
            !hooks_->installLevelTick(profile->resolver(), error))
            throw std::runtime_error(error);
        note("CraftProbe attached");
        note(BDS_HookRuntimeDiagnostic());
    }
    // Inventory reading copied from AutoMine: supplies -> inventory getItem
    // -> ItemStack. Used to hunt the stack net id field inside ItemStack.
    struct ItemLayout {
        std::size_t suppliesOffset{}, inventoryOffset{}, getItemSlot{};
        std::size_t itemRefOffset{}, countOffset{}, idOffset{}, strSizeOffset{}, strCapOffset{};
        bool loaded{};
    } items_;
    void loadItems(const nlohmann::json& root) {
        try {
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
    void dumpInventory() {
        if (!worldReady_ || !items_.loaded) { note("dumpinv: world/items not ready"); return; }
        void* player = world_.localPlayer(client_.load());
        if (!player) { note("dumpinv: no local player"); return; }
        note("dumpinv: player=0x" + hexBytes(&player, sizeof(player)));
        for (int slot = 0; slot < 36; ++slot) {
            void* stack = itemStack(player, slot);
            if (!stack) continue;
            std::uint8_t count{};
            if (!world_.readField(stack, items_.countOffset, count) || !count) continue;
            const std::string id = stackItemId(stack);
            std::array<std::byte, 0xA0> raw{};
            readMem(stack, raw.data(), raw.size());
            note("inv slot " + std::to_string(slot) + " count=" + std::to_string(count) +
                 " id=" + id + " stack=0x" + hexBytes(&stack, sizeof(stack)) +
                 " bytes: " + hexBytes(raw.data(), raw.size()));
        }
        note("dumpinv: done");
    }
    void persistCraftingData() {
        std::shared_ptr<const std::string> bytes;
        {
            std::lock_guard lock(dataMutex_);
            if (craftingData_ && craftingData_ != savedCraftingData_) {
                bytes = craftingData_;
                savedCraftingData_ = craftingData_;
            }
        }
        if (!bytes) return;
        std::ofstream out(directory_ / L"CraftingData.bin", std::ios::binary | std::ios::trunc);
        out.write(bytes->data(), static_cast<std::streamsize>(bytes->size()));
        note("CraftingData.bin written");
    }
    void observeSender() {
        void* client = client_.load();
        if (!client || !level_.load()) return;
        void* sender{}; void* target{};
        if (!senderLayout_.resolve(client, sender, target)) {
            if (!resolveNote_) { resolveNote_ = true; note("sender not resolvable yet"); }
            return;
        }
        sender_.store(sender);
        if (!sendToken_ && target) {
            if (BDS_HookSubscribe(BDS_HOOK_PACKET_SEND_TO_SERVER, target, &onSend, this, &sendToken_)) {
                sendTarget_ = target;
                note("object send hook subscribed");
            } else {
                note("object send hook subscribe failed");
            }
        }
    }
    static bool readMem(const void* address, void* out, std::size_t size) {
        SIZE_T copied{};
        return ReadProcessMemory(GetCurrentProcess(), address, out, size, &copied) && copied == size;
    }
    // Observe real outgoing packets at the object level: for ItemStackRequest
    // packets, capture the sender identity and the packet's +0x10 pointer so
    // the fake packet can be compared against the real thing.
    static void BDS_HOOK_CALL onSend(void* user, void* raw, BDS_HookNext next, void* frame) noexcept {
        next(frame, raw);
        auto& self = *static_cast<CraftProbeMod*>(user);
        if (self.stopped_) return;
        const auto& args = *static_cast<BDS_PacketSendArgs*>(raw);
        if (!args.packet) return;
        void* vtable{};
        if (!readMem(args.packet, &vtable, sizeof(vtable)) || !vtable) return;
        if (reinterpret_cast<std::uintptr_t>(vtable) - self.base_ != kPacketVtableRva) return;
        void* plus10{};
        readMem(static_cast<const std::byte*>(args.packet) + 0x10, &plus10, sizeof(plus10));
        self.realSender_.store(args.sender);
        self.capturedPlus10_.store(plus10);
        if (self.realSeen_.fetch_add(1) < 64) {
            std::array<std::byte, 0x50> obj{};
            readMem(args.packet, obj.data(), obj.size());
            self.note("real 147 object: sender=0x" +
                      hexBytes(&args.sender, sizeof(args.sender)) + " +0x10=0x" +
                      hexBytes(&plus10, sizeof(plus10)) + " obj: " + hexBytes(obj.data(), obj.size()));
            self.dumpRequest(args.packet);
        }
    }
    // Walk packet -> requests vector -> actions vector and dump each action
    // box (0x60) plus nested vectors for results (18) / auto (11) actions.
    void dumpRequest(const void* packet) {
        struct Vec { const std::byte* begin; const std::byte* end; const std::byte* cap; };
        Vec requests{};
        if (!readMem(static_cast<const std::byte*>(packet) + 0x30, &requests, sizeof(requests)) ||
            !requests.begin || requests.end < requests.begin)
            return;
        const std::size_t reqBytes = static_cast<std::size_t>(requests.end - requests.begin);
        if (reqBytes % 0x48 != 0 || reqBytes / 0x48 > 8) { note("dump: bad requests vec"); return; }
        for (std::size_t r = 0; r < reqBytes / 0x48; ++r) {
            const auto* req = requests.begin + r * 0x48;
            std::int32_t requestId{};
            readMem(req + 0x08, &requestId, sizeof(requestId));
            Vec actions{};
            if (!readMem(req + 0x10, &actions, sizeof(actions)) || !actions.begin ||
                actions.end < actions.begin)
                continue;
            const std::size_t actBytes = static_cast<std::size_t>(actions.end - actions.begin);
            if (actBytes % 0x60 != 0 || actBytes / 0x60 > 16) {
                note("dump: bad actions vec");
                continue;
            }
            note("dump request #" + std::to_string(r) + " id=" + std::to_string(requestId) +
                 " actions=" + std::to_string(actBytes / 0x60));
            for (std::size_t a = 0; a < actBytes / 0x60; ++a) {
                const auto* act = actions.begin + a * 0x60;
                std::array<std::byte, 0x60> box{};
                if (!readMem(act, box.data(), box.size())) continue;
                const auto index = static_cast<unsigned>(box[0x58]);
                note("dump action #" + std::to_string(a) + " index=" + std::to_string(index) +
                     ": " + hexBytes(box.data(), box.size()));
                if (index == 18) { // results_deprecated: vector<descriptor 0x58> @+0x00
                    Vec elems{};
                    if (readMem(act, &elems, sizeof(elems)) && elems.begin &&
                        elems.end >= elems.begin) {
                        const std::size_t eb =
                            static_cast<std::size_t>(elems.end - elems.begin);
                        if (eb % 0x58 == 0 && eb / 0x58 <= 8)
                            for (std::size_t e = 0; e < eb / 0x58; ++e) {
                                std::array<std::byte, 0x58> el{};
                                if (readMem(elems.begin + e * 0x58, el.data(), el.size()))
                                    note("dump result elem #" + std::to_string(e) + ": " +
                                         hexBytes(el.data(), el.size()));
                            }
                    }
                }
                if (index == 11) { // craft_recipe_auto: vector<ingredient 0x38> @+0x08
                    Vec elems{};
                    if (readMem(act + 0x08, &elems, sizeof(elems)) && elems.begin &&
                        elems.end >= elems.begin) {
                        const std::size_t eb =
                            static_cast<std::size_t>(elems.end - elems.begin);
                        if (eb % 0x38 == 0 && eb / 0x38 <= 8)
                            for (std::size_t e = 0; e < eb / 0x38; ++e) {
                                std::array<std::byte, 0x38> el{};
                                if (readMem(elems.begin + e * 0x38, el.data(), el.size()))
                                    note("dump ingredient elem #" + std::to_string(e) + ": " +
                                         hexBytes(el.data(), el.size()));
                            }
                    }
                }
            }
        }
    }
    // Replicates NativePacketSender::resolve but also captures the chain's
    // owner object, then scans the chain objects for a field pointing at an
    // object whose +0x10 == sender (the ItemStackNetManager signature: sender
    // at +0x10, pending-requests holder at +0x70).
    void scanForManager() {
        void* client = client_.load();
        void* sender = sender_.load();
        if (!client || !sender) { note("scan: client/sender unresolved"); return; }
        auto readPtr = [](const void* base, std::size_t off, void*& out) -> bool {
            return readMem(static_cast<const std::byte*>(base) + off, &out, sizeof(out)) && out != nullptr;
        };
        void *minecraft{}, *commands{}, *provider{}, *owner{};
        if (!readPtr(client, 0x1d0, minecraft) || !readPtr(minecraft, 0xc8, commands) ||
            !readPtr(commands, 8, provider)) {
            note("scan: chain broken before provider");
            return;
        }
        // owner = provider->vtable[1](provider); sender = owner->vtable[0x148](owner)
        auto callGetter = [](void* object, std::size_t vtableByteOff, void*& value) -> bool {
            void* table{}; void* fn{};
            if (!readMem(object, &table, sizeof(table)) || !table) return false;
            if (!readMem(static_cast<std::byte*>(table) + vtableByteOff, &fn, sizeof(fn)) || !fn)
                return false;
            __try {
                value = reinterpret_cast<void*(__fastcall*)(void*)>(fn)(object);
                return value != nullptr;
            } __except (EXCEPTION_EXECUTE_HANDLER) { value = nullptr; return false; }
        };
        if (!callGetter(provider, 8, owner)) { note("scan: owner getter failed"); return; }
        note("scan: owner=0x" + hexBytes(&owner, sizeof(owner)) + " sender=0x" +
             hexBytes(&sender, sizeof(sender)));
        struct Root { const char* name; void* obj; };
        const Root roots[] = {{"client", client}, {"minecraft", minecraft},
                              {"commands", commands}, {"owner", owner}, {"sender", sender}};
        for (const auto& root : roots) {
            for (std::size_t off = 0; off < 0x2000; off += 8) {
                void* field{};
                if (!readPtr(root.obj, off, field)) continue;
                void* plus10{};
                if (!readPtr(field, 0x10, plus10) || plus10 != sender) continue;
                void* holder{};
                readPtr(field, 0x70, holder);
                std::string extra = " holder=0x" + hexBytes(&holder, sizeof(holder));
                if (holder) {
                    void *b{}, *e{};
                    if (readPtr(holder, 0, b) && readPtr(holder, 8, e))
                        extra += " queueBegin=0x" + hexBytes(&b, sizeof(b)) + " queueEnd=0x" +
                                 hexBytes(&e, sizeof(e));
                }
                note(std::string("scan: CANDIDATE via ") + root.name + "+0x" +
                     std::to_string(off) + " -> manager=0x" + hexBytes(&field, sizeof(field)) + extra);
            }
        }
        note("scan: done");
        // Second level: dump each candidate and scan its fields too.
        for (const auto& root : roots) {
            for (std::size_t off = 0; off < 0x2000; off += 8) {
                void* field{};
                if (!readPtr(root.obj, off, field)) continue;
                void* plus10{};
                if (!readPtr(field, 0x10, plus10) || plus10 != sender) continue;
                std::array<std::byte, 0x100> raw{};
                if (readMem(field, raw.data(), raw.size()))
                    note(std::string("scan: dump of candidate via ") + root.name + ":\n" +
                         hexBytes(raw.data(), raw.size()));
                for (std::size_t off2 = 0; off2 < 0x400; off2 += 8) {
                    void* inner{};
                    if (!readPtr(field, off2, inner)) continue;
                    void* innerPlus10{};
                    if (!readPtr(inner, 0x10, innerPlus10) || innerPlus10 != sender) continue;
                    void* holder2{};
                    readPtr(inner, 0x70, holder2);
                    note(std::string("scan: LEVEL2 candidate via ") + root.name + " manager+0x" +
                         std::to_string(off2) + " -> 0x" + hexBytes(&inner, sizeof(inner)) +
                         " holder=0x" + hexBytes(&holder2, sizeof(holder2)));
                }
            }
        }
        note("scan: level2 done");
    }
    void checkTrigger() {
        std::error_code ec;
        const auto dumpPath = directory_ / L"dumpinv.txt";
        if (std::filesystem::exists(dumpPath, ec)) {
            std::filesystem::remove(dumpPath, ec);
            dumpInventory();
        }
        const auto scanPath = directory_ / L"scan.txt";
        if (std::filesystem::exists(scanPath, ec)) {
            std::filesystem::remove(scanPath, ec);
            scanForManager();
        }
        const auto flowPath = directory_ / L"flow.txt";
        if (std::filesystem::exists(flowPath, ec)) {
            std::string key, grid, third;
            int dep = -1;
            bool zeroExtra = false;
            {
                // Close the stream before removing: an open ifstream makes
                // remove() fail on Windows and the trigger refires every tick.
                std::ifstream in(flowPath);
                if (in && (in >> key)) {
                    if (!(in >> grid)) grid = "p";
                    if (in >> third) {
                        if (third == "z") zeroExtra = true;
                        else dep = std::atoi(third.c_str());
                    }
                }
            }
            std::filesystem::remove(flowPath, ec);
            if (!key.empty()) startFlow(key, grid != "t", dep, zeroExtra);
        }
        const auto goPath = directory_ / L"go.txt";
        if (!std::filesystem::exists(goPath, ec)) return;
        void* sender = sender_.load();
        if (!sender) { note("go.txt present but sender unresolved"); return; }
        // mode.txt: "recipe|auto|recipe2|auto2 <netId> <times>"
        //           "auto3 <netId> <times> <ingredientItemName> <ingredientCount>"
        // (the "2" variants reuse the +0x10 pointer captured from a real packet;
        //  "auto3" sends craft_recipe_auto with one name ingredient)
        bool autoMode = true;
        bool useCaptured = false;
        bool withIngredient = false;
        std::uint32_t netId = 3587;
        unsigned times = 1;
        std::string ingredientName;
        unsigned ingredientCount = 1;
        std::ifstream mode(directory_ / L"mode.txt");
        std::string kind;
        if (mode && (mode >> kind)) {
            autoMode = (kind.rfind("auto", 0) == 0);
            useCaptured = (kind.size() > 0 && kind.back() == '2');
            withIngredient = (kind == "auto3");
            if (withIngredient) useCaptured = true;
            unsigned parsedId{}, parsedTimes{};
            if (mode >> parsedId) netId = parsedId;
            if (mode >> parsedTimes) times = parsedTimes;
            if (withIngredient) {
                mode >> ingredientName;
                if (!(mode >> ingredientCount)) ingredientCount = 1;
            }
        }
        std::filesystem::rename(goPath, directory_ / L"done.txt", ec);
        sendCraft(sender, autoMode, netId, times, useCaptured, withIngredient, ingredientName,
                  ingredientCount);
    }
    std::int32_t nextRequestId() {
        return requestId_.fetch_sub(2) - 2; // first call returns -99999-... see init
    }
    void sendCraft(void* sender, bool autoMode, std::uint32_t recipeNetId, unsigned times,
                   bool useCaptured, bool withIngredient, const std::string& ingredientName,
                   unsigned ingredientCount) {
        auto* actions = new std::byte[0x60]{};
        std::memcpy(actions + 0x00, &recipeNetId, sizeof(recipeNetId));
        actions[0x04] = static_cast<std::byte>(times & 0xFF);
        actions[0x58] = static_cast<std::byte>(autoMode ? 11 : 10);
        std::byte* ingredient = nullptr;
        if (withIngredient) {
            // One RecipeIngredient2 element (0x38): name descriptor variant.
            ingredient = new std::byte[0x38]{};
            new (ingredient + 0x00) std::string(ingredientName);
            const std::int32_t metadata = -1;
            std::memcpy(ingredient + 0x20, &metadata, sizeof(metadata));
            ingredient[0x28] = static_cast<std::byte>(1); // variant: name
            const std::uint16_t count = static_cast<std::uint16_t>(ingredientCount);
            std::memcpy(ingredient + 0x30, &count, sizeof(count));
            struct Vec { void* begin; void* end; void* cap; };
            const Vec ingredientsVec{ingredient, ingredient + 0x38, ingredient + 0x38};
            std::memcpy(actions + 0x08, &ingredientsVec, sizeof(ingredientsVec));
        }
        auto* request = new std::byte[0x48]{};
        void* requestVtable = reinterpret_cast<void*>(base_ + kRequestDataVtableRva);
        std::memcpy(request + 0x00, &requestVtable, sizeof(requestVtable));
        const std::int32_t requestId = nextRequestId();
        std::memcpy(request + 0x08, &requestId, sizeof(requestId));
        struct Vec { void* begin; void* end; void* cap; };
        const Vec actionsVec{actions, actions + 0x60, actions + 0x60};
        std::memcpy(request + 0x10, &actionsVec, sizeof(actionsVec));
        const std::int32_t minusOne = -1;
        std::memcpy(request + 0x40, &minusOne, sizeof(minusOne));

        alignas(8) std::array<std::byte, 0x50> packet{};
        void* packetVtable = reinterpret_cast<void*>(base_ + kPacketVtableRva);
        std::memcpy(packet.data() + 0x00, &packetVtable, sizeof(packetVtable));
        const std::uint64_t priorityReliability = 0x100000002ull;
        std::memcpy(packet.data() + 0x08, &priorityReliability, sizeof(priorityReliability));
        void* plus10 = useCaptured ? capturedPlus10_.load() : nullptr;
        if (plus10) std::memcpy(packet.data() + 0x10, &plus10, sizeof(plus10));
        const Vec requestsVec{request, request + 0x48, request + 0x48};
        std::memcpy(packet.data() + 0x30, &requestsVec, sizeof(requestsVec));
        const std::uint32_t serializationVersion = 5;
        std::memcpy(packet.data() + 0x48, &serializationVersion, sizeof(serializationVersion));

        void* mySender = sender;
        void* realSender = realSender_.load();
        note(std::string("dispatching fake ItemStackRequest: mode=") +
             (autoMode ? "auto" : "recipe") + " recipeNetId=" + std::to_string(recipeNetId) +
             " times=" + std::to_string(times) + " requestId=" + std::to_string(requestId) +
             " useCaptured=" + (useCaptured ? "1" : "0") +
             (withIngredient ? " ingredient=" + ingredientName + "x" + std::to_string(ingredientCount) : "") +
             " mySender=0x" +
             hexBytes(&mySender, sizeof(mySender)) + " realSender=0x" +
             hexBytes(&realSender, sizeof(realSender)));
        BDS_PacketSendArgs args{sender, packet.data()};
        BDS_HookDispatch(BDS_HOOK_PACKET_SEND_TO_SERVER, &args);
        note("dispatch returned");
        if (ingredient) {
            reinterpret_cast<std::string*>(ingredient + 0x00)->~basic_string();
            delete[] ingredient;
        }
        delete[] actions;
        delete[] request;
    }
    // ---- ItemStackResponse parsing --------------------------------------
    // Format derived from captured traffic (2026-11-16):
    //   varuint responseCount
    //   per response: u8 status, zigzag requestId, u8 (always 1),
    //     varuint containerCount
    //     per container: u8 containerId, u8 dynamicOption (1 -> varuint id),
    //       varuint slotCount
    //       per slot: u8 slot, u8 hotbarSlot, u8 count, u8 (always 1),
    //         zigzag stackId, string customName, string filteredName,
    //         zigzag durabilityCorrection
    struct RespSlot { unsigned container, slot, count; std::int32_t stackId; };
    struct RespInfo { unsigned status{}; std::vector<RespSlot> slots; };
    std::mutex respMutex_;
    std::map<std::int32_t, RespInfo> responses_;
    struct RespReader {
        const std::uint8_t* p; std::size_t n, pos{};
        bool u8(unsigned& v) { if (pos >= n) return false; v = p[pos++]; return true; }
        bool varuint(std::uint64_t& v) {
            v = 0; int s = 0;
            while (true) {
                unsigned b; if (!u8(b)) return false;
                v |= (std::uint64_t)(b & 0x7f) << s;
                if (!(b & 0x80)) return true;
                s += 7; if (s > 63) return false;
            }
        }
        bool zigzag(std::int32_t& v) {
            std::uint64_t u; if (!varuint(u)) return false;
            v = static_cast<std::int32_t>((u >> 1) ^ static_cast<std::uint64_t>(
                -static_cast<std::int64_t>(u & 1)));
            return true;
        }
        bool str() {
            std::uint64_t len; if (!varuint(len)) return false;
            if (pos + len > n) return false; pos += static_cast<std::size_t>(len); return true;
        }
    };
    void parseResponse(const std::string& payload) {
        RespReader r{reinterpret_cast<const std::uint8_t*>(payload.data()), payload.size()};
        std::uint64_t count{};
        if (!r.varuint(count) || count > 64) return;
        for (std::uint64_t i = 0; i < count; ++i) {
            unsigned status{}; std::int32_t reqId{};
            if (!r.u8(status) || !r.zigzag(reqId)) return;
            RespInfo info; info.status = status;
            if (status != 0) {
                // Error responses carry no flag byte and no container data
                // (6-byte payloads observed: count, status, reqId, 0x00).
                std::lock_guard lock(respMutex_);
                responses_[reqId] = std::move(info);
                return;
            }
            unsigned flag{};
            if (!r.u8(flag)) return;
            std::uint64_t nc{};
            if (!r.varuint(nc) || nc > 16) return;
            bool ok = true;
            for (std::uint64_t c = 0; c < nc && ok; ++c) {
                unsigned cont{}, dyn{};
                if (!r.u8(cont) || !r.u8(dyn)) { ok = false; break; }
                if (dyn == 1) { std::uint64_t d; if (!r.varuint(d)) { ok = false; break; } }
                std::uint64_t ns{};
                if (!r.varuint(ns) || ns > 64) { ok = false; break; }
                for (std::uint64_t s = 0; s < ns; ++s) {
                    unsigned slot{}, hot{}, cnt{}, one{};
                    std::int32_t stackId{}, dur{};
                    if (!r.u8(slot) || !r.u8(hot) || !r.u8(cnt) || !r.u8(one) ||
                        !r.zigzag(stackId) || !r.str() || !r.str()) {
                        ok = false; break;
                    }
                    // The durability correction is only written for slots
                    // holding a real stack; emptied slots (stackId 0) omit it
                    // (verified against captured traffic, 2026-11-16).
                    if (stackId != 0 && !r.zigzag(dur)) { ok = false; break; }
                    info.slots.push_back(RespSlot{cont, slot, cnt, stackId});
                }
            }
            if (!ok) return;
            std::lock_guard lock(respMutex_);
            responses_[reqId] = std::move(info);
        }
    }
    bool takeResponse(std::int32_t reqId, RespInfo& out) {
        std::lock_guard lock(respMutex_);
        auto it = responses_.find(reqId);
        if (it == responses_.end()) return false;
        out = std::move(it->second);
        responses_.erase(it);
        return true;
    }
    // ---- inventory snapshot (id, count, stack net id at ItemStack+0x80) --
    struct InvItem { std::string id; int count{}; std::int32_t netId{}; };
    bool snapshotInventory(std::map<int, InvItem>& out) {
        if (!worldReady_ || !items_.loaded) return false;
        void* player = world_.localPlayer(client_.load());
        if (!player) return false;
        out.clear();
        for (int slot = 0; slot < 36; ++slot) {
            void* stack = itemStack(player, slot);
            if (!stack) continue;
            std::uint8_t count{};
            if (!world_.readField(stack, items_.countOffset, count) || !count) continue;
            std::int32_t netId{};
            readMem(static_cast<const std::byte*>(stack) + 0x80, &netId, sizeof(netId));
            out[slot] = InvItem{stackItemId(stack), static_cast<int>(count), netId};
        }
        return true;
    }
    // ---- native packet builders -----------------------------------------
    struct RawVec { void* begin; void* end; void* cap; };
    static void putSlotInfo(std::byte* box, std::size_t off, std::uint32_t container,
                            std::uint8_t slot, std::int32_t netId) {
        std::memcpy(box + off + 0x00, &container, sizeof(container));
        box[off + 0x0C] = static_cast<std::byte>(slot);
        std::memcpy(box + off + 0x10, &netId, sizeof(netId));
        // discriminant at +0x20 stays 0 (case0: plain stack reference)
    }
    void putSlotInfoResult(std::byte* box, std::size_t off, std::int32_t requestId) {
        // case1 (result take): +0x10 holds the RequestData vtable address
        // value itself, +0x18 the owning request id, discriminant 1.
        const std::uint32_t container = 60;
        std::memcpy(box + off + 0x00, &container, sizeof(container));
        box[off + 0x0C] = static_cast<std::byte>(50);
        void* vtableValue = reinterpret_cast<void*>(base_ + kRequestDataVtableRva);
        std::memcpy(box + off + 0x10, &vtableValue, sizeof(vtableValue));
        std::memcpy(box + off + 0x18, &requestId, sizeof(requestId));
        box[off + 0x20] = static_cast<std::byte>(1);
    }
    std::byte* makeRequest(std::int32_t requestId, std::byte* actions, std::size_t actionCount) {
        auto* request = new std::byte[0x48]{};
        void* vtable = reinterpret_cast<void*>(base_ + kRequestDataVtableRva);
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
        alignas(8) std::array<std::byte, 0x50> packet{};
        void* packetVtable = reinterpret_cast<void*>(base_ + kPacketVtableRva);
        std::memcpy(packet.data() + 0x00, &packetVtable, sizeof(packetVtable));
        const std::uint64_t pr = 0x100000002ull;
        std::memcpy(packet.data() + 0x08, &pr, sizeof(pr));
        void* plus10 = capturedPlus10_.load();
        if (plus10) std::memcpy(packet.data() + 0x10, &plus10, sizeof(plus10));
        const RawVec vec{requests, requests + count * 0x48, requests + count * 0x48};
        std::memcpy(packet.data() + 0x30, &vec, sizeof(vec));
        const std::uint32_t version = 5;
        std::memcpy(packet.data() + 0x48, &version, sizeof(version));
        BDS_PacketSendArgs args{sender_.load(), packet.data()};
        BDS_HookDispatch(BDS_HOOK_PACKET_SEND_TO_SERVER, &args);
    }
    // ---- manual craft flow ----------------------------------------------
    // Replicates the real client's hand flow: place each ingredient into the
    // grid (one request per cell, waiting for each response so stack ids are
    // always the server-confirmed ones), then one request with
    // [craft_recipe, results_deprecated, consume x cells, take result].
    struct FlowCell { int row, col; const char* ingredient; };
    struct FlowRecipe {
        const char* key{};
        std::uint32_t netId{};
        const char* descName{};
        std::uint16_t descCount{};
        std::uint32_t descBrid{};
        bool descDamage{};
        std::vector<FlowCell> cells;
    };
    static const FlowRecipe& flowRecipe(const std::string& key) {
        static const char* P = "minecraft:oak_planks";
        static const char* S = "minecraft:stick";
        static const char* C = "minecraft:cobblestone";
        static const char* L = "minecraft:oak_log";
        static const std::vector<FlowRecipe> recipes = {
            {"planks", 3587, "minecraft:oak_planks", 4, 0x728B1AB6u, false, {{0, 0, L}}},
            {"stick", 4453, "minecraft:stick", 4, 0, false, {{0, 0, P}, {1, 0, P}}},
            {"table", 4090, "minecraft:crafting_table", 1, 0x68702CC0u, false,
             {{0, 0, P}, {0, 1, P}, {1, 0, P}, {1, 1, P}}},
            {"wpick", 4601, "minecraft:wooden_pickaxe", 1, 0, true,
             {{0, 0, P}, {0, 1, P}, {0, 2, P}, {1, 1, S}, {2, 1, S}}},
            {"spick", 4458, "minecraft:stone_pickaxe", 1, 0, true,
             {{0, 0, C}, {0, 1, C}, {0, 2, C}, {1, 1, S}, {2, 1, S}}},
            {"boat", 3500, "minecraft:oak_boat", 1, 0, false,
             {{0, 0, P}, {0, 2, P}, {1, 0, P}, {1, 1, P}, {1, 2, P}}},
        };
        for (const auto& r : recipes) if (key == r.key) return r;
        static const FlowRecipe none{};
        return none;
    }
    struct Flow {
        bool active{};
        const FlowRecipe* recipe{};
        int width{3}, base{32};
        std::map<int, InvItem> inv;
        std::size_t cell{};
        std::vector<std::pair<int, std::int32_t>> grid; // gridSlot -> stackId
        int srcSlot{-1};
        std::int32_t pending{};
        std::chrono::steady_clock::time_point sentAt{};
        int phase{}; // 0 place, 1 craft, 2 deposit
        std::int32_t resultStack{};
        unsigned resultCount{};
        int depSlot{-1};
        bool zeroExtra{};
    } flow_;
    // Response-tracked inventory overlay. The client's own inventory model
    // does NOT apply responses to requests it did not originate, so after a
    // fake craft the memory view is stale (ingredients look unconsumed, the
    // product is invisible) until a relog resyncs it. The flow therefore
    // tracks every slot it touches from ItemStackResponses and overlays that
    // state onto the next flow's snapshot.
    std::map<int, InvItem> shadowInv_;
    void flowFail(const std::string& why) {
        note("FLOW FAIL: " + why);
        flow_.active = false;
        flow_.pending = 0;
    }
    int flowGridSlot(std::size_t i) const {
        const auto& c = flow_.recipe->cells[i];
        return flow_.base + c.row * flow_.width + c.col;
    }
    void flowSendPlace() {
        const auto& cellDef = flow_.recipe->cells[flow_.cell];
        int src = -1;
        for (const auto& [slot, item] : flow_.inv)
            if (item.id == cellDef.ingredient && item.count > 0) { src = slot; break; }
        if (src < 0) { flowFail(std::string("no ingredient ") + cellDef.ingredient); return; }
        flow_.srcSlot = src;
        const int gridSlot = flowGridSlot(flow_.cell);
        const std::int32_t srcNet = flow_.inv[src].netId;
        auto* actions = new std::byte[0x60]{};
        actions[0x00] = static_cast<std::byte>(1);
        putSlotInfo(actions, 0x08, 12, static_cast<std::uint8_t>(src), srcNet);
        putSlotInfo(actions, 0x30, 13, static_cast<std::uint8_t>(gridSlot), 0);
        actions[0x58] = static_cast<std::byte>(1); // place
        const std::int32_t reqId = nextRequestId();
        auto* request = makeRequest(reqId, actions, 1);
        note("flow: place " + std::string(cellDef.ingredient) + " inv" + std::to_string(src) +
             " (net " + std::to_string(srcNet) + ") -> grid " + std::to_string(gridSlot) +
             " req=" + std::to_string(reqId));
        dispatchRequests(request, 1);
        delete[] actions;
        delete[] request;
        flow_.inv[src].count -= 1; // provisional; the response overwrites it
        flow_.pending = reqId;
        flow_.sentAt = std::chrono::steady_clock::now();
        flow_.phase = 0;
    }
    void flowSendCraft() {
        const FlowRecipe& r = *flow_.recipe;
        const std::size_t nActions = 2 + flow_.grid.size() + 1;
        auto* actions = new std::byte[0x60 * nActions]{};
        const std::int32_t reqId = nextRequestId();
        std::byte* a0 = actions; // craft_recipe
        std::memcpy(a0 + 0x00, &r.netId, sizeof(r.netId));
        a0[0x04] = static_cast<std::byte>(1);
        a0[0x58] = static_cast<std::byte>(10);
        std::byte* a1 = actions + 0x60; // results_deprecated
        auto* desc = new std::byte[0x58]{};
        new (desc + 0x00) std::string(r.descName);
        // metadata @+0x20 = 0; +0x24 = 0; +0x32 = 0 (uninterpreted; test)
        desc[0x28] = static_cast<std::byte>(1); // variant: name
        std::memcpy(desc + 0x30, &r.descCount, sizeof(r.descCount));
        std::memcpy(desc + 0x34, &r.descBrid, sizeof(r.descBrid));
        if (r.descDamage && !flow_.zeroExtra) {
            static constexpr std::array<std::uint8_t, 28> kDamage = {
                0xff, 0xff, 0x01, 0x0a, 0x00, 0x00, 0x03, 0x06, 0x00, 0x44, 0x61, 0x6d,
                0x61, 0x67, 0x65, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x01};
            new (desc + 0x38) std::string(reinterpret_cast<const char*>(kDamage.data()),
                                          kDamage.size());
        } else {
            new (desc + 0x38) std::string(10, '\0');
        }
        const RawVec descVec{desc, desc + 0x58, desc + 0x58};
        std::memcpy(a1 + 0x00, &descVec, sizeof(descVec));
        a1[0x18] = static_cast<std::byte>(1); // times_crafted
        a1[0x58] = static_cast<std::byte>(18);
        std::size_t ai = 2;
        for (const auto& [gridSlot, stackId] : flow_.grid) { // consume x cells
            std::byte* a = actions + ai++ * 0x60;
            a[0x00] = static_cast<std::byte>(1);
            putSlotInfo(a, 0x08, 13, static_cast<std::uint8_t>(gridSlot), stackId);
            a[0x58] = static_cast<std::byte>(5);
        }
        std::byte* at = actions + ai * 0x60; // take result -> cursor
        // Take the whole result stack: a count of 1 would leave the rest of
        // the result behind in the crafting output (measured: planks x4
        // yielded only 1 on the cursor with take count 1).
        at[0x00] = static_cast<std::byte>(r.descCount & 0xFF);
        putSlotInfoResult(at, 0x08, reqId);
        putSlotInfo(at, 0x30, 59, 0, 0);
        at[0x58] = static_cast<std::byte>(0);
        auto* request = makeRequest(reqId, actions, nActions);
        note("flow: craft " + std::string(r.key) + " req=" + std::to_string(reqId) +
             " actions=" + std::to_string(nActions));
        dispatchRequests(request, 1);
        reinterpret_cast<std::string*>(desc + 0x00)->~basic_string();
        reinterpret_cast<std::string*>(desc + 0x38)->~basic_string();
        delete[] desc;
        delete[] actions;
        delete[] request;
        flow_.pending = reqId;
        flow_.sentAt = std::chrono::steady_clock::now();
        flow_.phase = 1;
    }
    void flowSendDeposit() {
        auto* actions = new std::byte[0x60]{};
        // Move the WHOLE result stack off the cursor: a partial move leaves
        // items on the cursor, and a loaded cursor makes every later craft's
        // result take fail with status 50 (root cause of the "second craft
        // always fails" cascade, confirmed 2026-11-16).
        actions[0x00] = static_cast<std::byte>(flow_.resultCount & 0xFF);
        putSlotInfo(actions, 0x08, 59, 0, flow_.resultStack);
        putSlotInfo(actions, 0x30, 12, static_cast<std::uint8_t>(flow_.depSlot), 0);
        actions[0x58] = static_cast<std::byte>(1); // place
        const std::int32_t reqId = nextRequestId();
        auto* request = makeRequest(reqId, actions, 1);
        note("flow: deposit result (net " + std::to_string(flow_.resultStack) + ") -> inv " +
             std::to_string(flow_.depSlot) + " req=" + std::to_string(reqId));
        dispatchRequests(request, 1);
        delete[] actions;
        delete[] request;
        flow_.pending = reqId;
        flow_.sentAt = std::chrono::steady_clock::now();
        flow_.phase = 2;
    }
    void flowAdvance(const RespInfo& resp) {
        if (resp.status != 0) {
            flowFail("response status=" + std::to_string(resp.status) +
                     " phase=" + std::to_string(flow_.phase));
            return;
        }
        if (flow_.phase == 0) {
            const int gridSlot = flowGridSlot(flow_.cell);
            bool foundGrid = false;
            for (const auto& s : resp.slots) {
                if (s.container == 13 && static_cast<int>(s.slot) == gridSlot) {
                    flow_.grid.emplace_back(gridSlot, s.stackId);
                    foundGrid = true;
                    note("flow: grid " + std::to_string(gridSlot) + " stack=" +
                         std::to_string(s.stackId) + " count=" + std::to_string(s.count));
                }
                if (s.container == 12 && static_cast<int>(s.slot) == flow_.srcSlot) {
                    auto& item = flow_.inv[flow_.srcSlot];
                    item.count = static_cast<int>(s.count);
                    item.netId = s.stackId;
                    shadowInv_[flow_.srcSlot] = item;
                    note("flow: source inv " + std::to_string(flow_.srcSlot) + " now count=" +
                         std::to_string(s.count) + " net=" + std::to_string(s.stackId));
                }
            }
            if (!foundGrid) { flowFail("place response missing grid entry"); return; }
            flow_.cell++;
            if (flow_.cell < flow_.recipe->cells.size()) flowSendPlace();
            else flowSendCraft();
        } else if (flow_.phase == 1) {
            bool found = false;
            unsigned resultCount = 0;
            for (const auto& s : resp.slots)
                if (s.container == 59) {
                    flow_.resultStack = s.stackId; resultCount = s.count; found = true;
                }
            if (!found) { flowFail("craft response missing cursor entry"); return; }
            flow_.resultCount = resultCount;
            note("flow: crafted " + std::string(flow_.recipe->descName) + " x" +
                 std::to_string(resultCount) + " result net=" +
                 std::to_string(flow_.resultStack));
            if (flow_.depSlot < 0) {
                note("FLOW OK: " + std::string(flow_.recipe->key) +
                     " (WARNING: result left on cursor; later crafts will fail)");
                flow_.active = false;
            } else flowSendDeposit();
        } else {
            InvItem deposited{flow_.recipe->descName,
                              static_cast<int>(flow_.recipe->descCount), flow_.resultStack};
            for (const auto& s : resp.slots)
                if (s.container == 12 && static_cast<int>(s.slot) == flow_.depSlot) {
                    deposited.count = static_cast<int>(s.count);
                    deposited.netId = s.stackId;
                }
            shadowInv_[flow_.depSlot] = deposited;
            note("FLOW OK: " + std::string(flow_.recipe->key));
            flow_.active = false;
        }
    }
    void flowTick() {
        if (!flow_.active || !flow_.pending) return;
        RespInfo resp;
        if (takeResponse(flow_.pending, resp)) {
            const auto waited = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - flow_.sentAt).count();
            if (waited > 3000)
                note("flow: response waited " + std::to_string(waited) + " ms (phase " +
                     std::to_string(flow_.phase) + ")");
            flow_.pending = 0;
            flowAdvance(resp);
        } else if (std::chrono::steady_clock::now() - flow_.sentAt > std::chrono::seconds(180)) {
            // Responses to table crafts lagged past 30s on 2026-11-16 (the
            // pickaxe craft answered correctly but after the 30s timeout
            // fired; personal-grid crafts answer in <1s). 180s keeps those
            // late successes usable while the cause is unidentified.
            flowFail("timeout waiting for response");
        }
    }
    void startFlow(const std::string& key, bool personal, int depSlot, bool zeroExtra = false) {
        const FlowRecipe& r = flowRecipe(key);
        if (!r.key) { note("flow: unknown recipe " + key); return; }
        if (flow_.active) { note("flow: busy, ignoring start"); return; }
        if (!sender_.load()) { note("flow: sender unresolved"); return; }
        if (!capturedPlus10_.load()) { note("flow: no real 147 captured yet"); return; }
        std::map<int, InvItem> inv;
        if (!snapshotInventory(inv)) { note("flow: inventory snapshot failed"); return; }
        for (const auto& [slot, item] : shadowInv_) inv[slot] = item; // response-tracked wins
        flow_ = Flow{};
        flow_.active = true;
        flow_.recipe = &r;
        flow_.width = personal ? 2 : 3;
        flow_.base = personal ? 28 : 32;
        flow_.inv = std::move(inv);
        if (depSlot == -1) {
            depSlot = -2;
            for (int s = 0; s < 36 && depSlot == -2; ++s)
                if (!flow_.inv.count(s) || flow_.inv[s].count == 0) depSlot = s;
        }
        flow_.depSlot = depSlot;
        flow_.zeroExtra = zeroExtra;
        note("flow: start " + key + (personal ? " personal" : " table") +
             " dep=" + std::to_string(depSlot) + (zeroExtra ? " zeroExtra" : ""));
        flowSendPlace();
    }
    bedrock::GameApi game_;
    bedrock::NativePacketSender senderLayout_;
    bedrock::NativeWorldAccess world_;
    bool worldReady_{};
    bedrock::PacketAPI packets_;
    std::unique_ptr<bedrock::GameHooks> hooks_;
    std::filesystem::path directory_;
    std::string profileJson_;
    std::atomic<void*> client_{}, level_{}, sender_{};
    std::atomic<void*> realSender_{}, capturedPlus10_{};
    std::atomic<unsigned> realSeen_{};
    std::atomic<bool> stopped_{};
    std::atomic<std::int32_t> requestId_{-99};
    std::uintptr_t base_{};
    BDS_HookToken sendToken_{};
    void* sendTarget_{};
    bool resolveNote_{};
    std::mutex dataMutex_;
    std::shared_ptr<const std::string> craftingData_, savedCraftingData_;
    std::mutex logMutex_;
    std::deque<std::string> logs_;
};
std::atomic<CraftProbeMod*> mod{};
std::atomic<unsigned> initialStatus{};
std::atomic<bool> stopRequested{};
DWORD WINAPI start(void*) {
    HMODULE pinned{};
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                            reinterpret_cast<LPCWSTR>(&start), &pinned)) {
        initialStatus = 4;
        return 1;
    }
    try {
        auto* instance = new CraftProbeMod;
        mod = instance;
        if (stopRequested) instance->signalStop();
        instance->run();
    } catch (...) { initialStatus = 4; }
    return 0;
}
} // namespace
extern "C" __declspec(dllexport) unsigned CraftProbe_GetStatus() {
    auto* instance = mod.load();
    return instance ? instance->status.load() : initialStatus.load();
}
extern "C" __declspec(dllexport) void CraftProbe_Stop() {
    stopRequested = true;
    if (auto* instance = mod.load()) instance->signalStop();
}
BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(instance);
        if (HANDLE thread = CreateThread(nullptr, 0, start, nullptr, 0, nullptr)) CloseHandle(thread);
        else initialStatus = 4;
    } else if (reason == DLL_PROCESS_DETACH) stopRequested = true;
    return TRUE;
}
