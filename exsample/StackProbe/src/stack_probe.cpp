// StackProbe — throwaway RE probe for the AutoCraft work.
//
// Subscribes to the shared PacketSender::sendToServer hook and, for every
// packet the game sends, reads its id through the packet vtable (slot 1,
// the same slot NativeCommandRequests verifies for CommandRequest). When an
// ItemStackRequest (id 147) goes out, the native packet object is dumped to
// the log (raw bytes + plausible std::vector walks + one level of pointer
// chasing) so the in-memory layout can be reconstructed offline.
#include <bedrock/runtime/game_hooks.hpp>
#include <bedrock/profiles/profile_manager.hpp>
#include <bedrock/profiles/profile_json.hpp>
#include <bedrock/native/packet_sender.hpp>
#include <bedrock/detail/native_profile_functions.hpp>
#include <bedrock/runtime/hook_runtime.h>
#include <knownfolders.h>
#include <shlobj.h>
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <set>
#include <string>
#include <thread>

namespace {
constexpr std::uint32_t kItemStackRequestId = 147;

bool readMem(const void* address, void* out, std::size_t size) {
    SIZE_T copied{};
    return ReadProcessMemory(GetCurrentProcess(), address, out, size, &copied) && copied == size;
}
template <typename T> bool readVal(const void* base, std::size_t offset, T& out) {
    return readMem(static_cast<const std::byte*>(base) + offset, &out, sizeof(T));
}
bool heapPtr(std::uintptr_t p) {
    return p > 0x10000 && p < 0x7FFFFFFFFFFFull && (p & 7) == 0;
}
std::string hexDump(const std::byte* data, std::size_t size, std::uintptr_t baseAddr) {
    static constexpr char digits[] = "0123456789abcdef";
    std::string out;
    for (std::size_t row = 0; row < size; row += 16) {
        char addr[32];
        std::snprintf(addr, sizeof(addr), "  +0x%04zx  ", row);
        out += addr;
        for (std::size_t i = 0; i < 16; ++i) {
            if (row + i < size) {
                const auto b = static_cast<unsigned char>(data[row + i]);
                out += digits[b >> 4]; out += digits[b & 15]; out += ' ';
            } else out += "   ";
        }
        out += " |";
        for (std::size_t i = 0; i < 16 && row + i < size; ++i) {
            const auto b = static_cast<unsigned char>(data[row + i]);
            out += (b >= 0x20 && b < 0x7f) ? static_cast<char>(b) : '.';
        }
        out += "|\n";
    }
    (void)baseAddr;
    return out;
}

class StackProbeMod {
  public:
    std::atomic<unsigned> status{};
    void signalStop() noexcept { stopped_ = true; }
    void run() noexcept {
        status = 1;
        try {
            PWSTR path{};
            if (FAILED(SHGetKnownFolderPath(FOLDERID_Documents, KF_FLAG_CREATE, nullptr, &path)))
                throw std::runtime_error("Documents directory unavailable");
            const auto directory = std::filesystem::path(path) / L"PEXData" / L"StackProbe";
            CoTaskMemFree(path);
            std::filesystem::create_directories(directory);
            std::ofstream log(directory / L"StackProbe.log", std::ios::trunc);
            if (!log) throw std::runtime_error("could not open StackProbe.log");
            initialize();
            while (!stopped_) {
                flush(log);
                std::this_thread::sleep_for(std::chrono::milliseconds{100});
            }
            if (sendToken_) { BDS_HookUnsubscribe(sendToken_); sendToken_ = nullptr; }
            hooks_.reset();
            flush(log);
            status = 3;
        } catch (const std::exception& error) {
            note(std::string("startup failed: ") + error.what());
            status = 4;
            try {
                const auto* home = _wgetenv(L"USERPROFILE");
                if (home) {
                    auto directory = std::filesystem::path(home) / L"Documents" / L"PEXData" / L"StackProbe";
                    std::filesystem::create_directories(directory);
                    std::ofstream log(directory / L"StackProbe.log", std::ios::app);
                    flush(log);
                }
            } catch (...) {}
        } catch (...) { status = 4; }
    }

  private:
    void note(std::string value) {
        OutputDebugStringA(("[StackProbe] " + value + "\n").c_str());
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
                {bedrock::Feature::Gameplay, bedrock::Feature::LevelTick}, options,
                [this](const bedrock::RuntimeProfile& profile, std::string& failure) {
                    try {
                        const auto root = bedrock::parseNativeProfile(profile.json());
                        senderLayout_.load(root);
                        base_ = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
                        return true;
                    } catch (const std::exception& e) { failure = e.what(); return false; }
                },
                error))
            throw std::runtime_error(error);
        game_.Event.Enum.Gameplay.then([this](const bedrock::GameplayEvent& event) {
            if (event.clientInstance) client_.store(event.clientInstance);
        });
        game_.Event.Enum.LevelTick.then([this](const bedrock::LevelTickEvent& event) {
            if (stopped_) return;
            level_.store(event.level);
            observeSender();
        });
        const auto profile = profiles.snapshot();
        hooks_ = std::make_unique<bedrock::GameHooks>(game_);
        if (!hooks_->installGameplay(profile->resolver(), error) ||
            !hooks_->installLevelTick(profile->resolver(), error))
            throw std::runtime_error(error);
        note("StackProbe attached; waiting for sender resolution");
        note(BDS_HookRuntimeDiagnostic());
    }
    void observeSender() {
        void* client = client_.load();
        if (!client || !level_.load()) return;
        void* sender{}; void* target{};
        if (!senderLayout_.resolve(client, sender, target)) {
            if (!resolveNote_) { resolveNote_ = true; note("sender not resolvable yet"); }
            return;
        }
        if (sendToken_ && target == sendTarget_) { sender_.store(sender); return; }
        if (sendToken_) { note("sender target changed; keeping first subscription"); sender_.store(sender); return; }
        if (!BDS_HookSubscribe(BDS_HOOK_PACKET_SEND_TO_SERVER, target, &onSend, this, &sendToken_)) {
            note("send hook subscribe failed");
            return;
        }
        sendTarget_ = target;
        sender_.store(sender);
        note("send hook subscribed; target RVA 0x" + toHex(reinterpret_cast<std::uintptr_t>(target) - base_));
    }
    static std::string toHex(std::uintptr_t v) {
        char buf[32]; std::snprintf(buf, sizeof(buf), "%llx", static_cast<unsigned long long>(v));
        return buf;
    }
    static int callGetId(void* packet, void* vtable) noexcept {
        void* fn{};
        if (!readVal(vtable, 8, fn) || !fn) return -1;
        __try {
            return reinterpret_cast<int(__fastcall*)(void*)>(fn)(packet);
        } __except (EXCEPTION_EXECUTE_HANDLER) { return -1; }
    }
    static void BDS_HOOK_CALL onSend(void* user, void* raw, BDS_HookNext next, void* frame) noexcept {
        next(frame, raw); // never disturb the packet
        auto& self = *static_cast<StackProbeMod*>(user);
        if (self.stopped_) return;
        const auto& args = *static_cast<BDS_PacketSendArgs*>(raw);
        if (!args.packet || args.sender != self.sender_.load()) return;
        void* vtable{};
        if (!readVal(args.packet, 0, vtable) || !vtable) return;
        const int id = callGetId(args.packet, vtable);
        if (id < 0 || id > 400) return;
        {
            std::lock_guard lock(self.seenMutex_);
            if (self.seenIds_.insert(id).second)
                self.note("packet id " + std::to_string(id) + " vtable RVA 0x" +
                          toHex(reinterpret_cast<std::uintptr_t>(vtable) - self.base_));
        }
        if (static_cast<std::uint32_t>(id) == kItemStackRequestId &&
            self.dumpCount_.fetch_add(1) < 16)
            self.dumpPacket(args.packet, vtable);
    }
    // Scan a captured buffer for std::vector triples and heap pointers and
    // dump what they point at, recursing twice so packet -> requests ->
    // actions are all captured.
    void walkBuffer(const std::byte* data, std::size_t size, std::uintptr_t selfAddr, int depth) {
        for (std::size_t off = 0; off + 24 <= size; off += 8) {
            std::uintptr_t begin{}, end{}, cap{};
            std::memcpy(&begin, data + off, 8);
            std::memcpy(&end, data + off + 8, 8);
            std::memcpy(&cap, data + off + 16, 8);
            if (heapPtr(begin) && end >= begin && cap >= end && (end - begin) <= 0x40000 &&
                (cap - begin) <= 0x80000 && begin != selfAddr) {
                const auto bytes = static_cast<std::size_t>(end - begin);
                std::string line = "vector? d" + std::to_string(depth) + " @+0x" + toHex(off) +
                                   " bytes=" + std::to_string(bytes);
                for (std::size_t stride : {0x18u, 0x20u, 0x28u, 0x30u, 0x38u, 0x40u, 0x48u, 0x50u, 0x58u, 0x60u, 0x70u, 0x80u})
                    if (bytes && bytes % stride == 0) line += " /" + std::to_string(stride) + "=" + std::to_string(bytes / stride);
                note(line);
                if (bytes) {
                    const std::size_t take = std::min<std::size_t>(bytes, 0x480);
                    std::vector<std::byte> inner(take);
                    if (readMem(reinterpret_cast<void*>(begin), inner.data(), take)) {
                        note("vector data d" + std::to_string(depth) + " @+0x" + toHex(off) + ":\n" +
                             hexDump(inner.data(), take, begin));
                        if (depth < 2) walkBuffer(inner.data(), take, begin, depth + 1);
                    }
                }
                continue;
            }
            if (heapPtr(begin) && begin != selfAddr && depth < 2) {
                std::array<std::byte, 0x80> target{};
                if (readMem(reinterpret_cast<void*>(begin), target.data(), target.size()))
                    note("ptr d" + std::to_string(depth) + " @+0x" + toHex(off) + " -> 0x" + toHex(begin) + ":\n" +
                         hexDump(target.data(), target.size(), begin));
            }
        }
    }
    void dumpPacket(void* packet, void* vtable) {
        note("=== ItemStackRequest dump #" + std::to_string(dumpCount_.load()) +
             " packet=0x" + toHex(reinterpret_cast<std::uintptr_t>(packet)) +
             " vtable RVA 0x" + toHex(reinterpret_cast<std::uintptr_t>(vtable) - base_) + " ===");
        // vtable slots (first 10) as RVAs
        {
            std::string slots = "vtable slots:";
            for (int i = 0; i < 10; ++i) {
                void* fn{};
                if (readVal(vtable, static_cast<std::size_t>(i) * 8, fn) && fn)
                    slots += " [" + std::to_string(i) + "]=0x" + toHex(reinterpret_cast<std::uintptr_t>(fn) - base_);
            }
            note(slots);
        }
        std::array<std::byte, 0x200> obj{};
        if (!readMem(packet, obj.data(), obj.size())) { note("object read failed"); return; }
        note("object 0x200 bytes:\n" + hexDump(obj.data(), obj.size(), reinterpret_cast<std::uintptr_t>(packet)));
        walkBuffer(obj.data(), obj.size(), reinterpret_cast<std::uintptr_t>(packet), 0);
        note("=== end dump ===");
    }
    bedrock::GameApi game_;
    bedrock::NativePacketSender senderLayout_;
    std::unique_ptr<bedrock::GameHooks> hooks_;
    std::atomic<void*> client_{}, level_{}, sender_{};
    std::atomic<bool> stopped_{};
    std::atomic<unsigned> dumpCount_{};
    std::uintptr_t base_{};
    void* sendTarget_{};
    BDS_HookToken sendToken_{};
    bool resolveNote_{};
    std::mutex seenMutex_;
    std::set<int> seenIds_;
    std::mutex logMutex_;
    std::deque<std::string> logs_;
};
std::atomic<StackProbeMod*> mod{};
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
        auto* instance = new StackProbeMod;
        mod = instance;
        if (stopRequested) instance->signalStop();
        instance->run();
    } catch (...) { initialStatus = 4; }
    return 0;
}
} // namespace
extern "C" __declspec(dllexport) unsigned StackProbe_GetStatus() {
    auto* instance = mod.load();
    return instance ? instance->status.load() : initialStatus.load();
}
extern "C" __declspec(dllexport) void StackProbe_Stop() {
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
