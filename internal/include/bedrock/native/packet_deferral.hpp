#pragma once
#include <bedrock/detail/native_profile_functions.hpp>
#include <bedrock/profiles/profile_json.hpp>
#include <bedrock/runtime/hook_runtime.h>
#include <intrin.h>
#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <mutex>
#include <stdexcept>

namespace bedrock {
// Delay-tolerant packet deferral at the concrete client-network handler entry.
//
// The profiled `LegacyClientNetworkHandler::handle(NetworkIdentifier const&,
// std::shared_ptr<PacketT>)` overloads are ordinary
// virtual functions, so the profile pins each entry by RVA + entry guard and
// additionally proves the identity by finding the `lea` of the std::function
// callable table of that overload's queued lambda inside the handler body.
//
// The ABI is proven by the handler body itself and re-checked live on every
// call:
//   * RCX = handler `this`, whose first qword must equal the profiled vtable
//     address, and whose vtable slot must hold this very entry;
//   * RDX = pointer to the NetworkIdentifier (0xB0 bytes; the handler copies it
//     with a single memcpy, so it is trivially copyable and safe to snapshot);
//   * R8  = pointer to the by-value std::shared_ptr<Packet> argument (packet at
//     +0, control block at +8); the handler increments the use count at
//     control+8 and releases it in its epilogue, so ownership follows MSVC's
//     shared_ptr convention exactly.
//
// Deferral holds that shared_ptr (one use count of our own) and replays the
// handler later on the game thread. Nothing is written to game memory, no game
// container is touched, and every entry that is not deferred goes straight to
// the original.
class NativePacketDeferral {
  public:
    static constexpr unsigned kMaxHandlers = 5;
    static constexpr unsigned kQueueCapacity = 512;
    static constexpr unsigned kMaxIdentifierBytes = 256;
    static constexpr unsigned kMaxAgeMs = 400;
    using Handler = void(__fastcall *)(void *self, const void *identifier, void *shared);
    using Dispatcher = void(__fastcall *)(void *dispatcher, const void *identifier,
                                          void *self, const void *shared);
    using Policy = bool (*)(int packetId) noexcept;

    struct HandlerStatistics {
        int packetId{};
        unsigned calls{}, deferred{}, replayed{};
    };

    struct Statistics {
        unsigned long long calls{}, deferred{}, replayed{}, overflow{}, otherObject{},
            otherClass{}, worldDrops{}, agedOut{};
        unsigned queue{};
        std::array<HandlerStatistics, kMaxHandlers> byHandler{};
    };

    bool install(const RuntimeProfile &profile, std::string_view embedded, Policy policy,
                 std::string &error) {
        try {
            std::lock_guard lock(state().mutex);
            auto &s = state();
            if (s.installed)
                throw std::runtime_error("packet deferral already installed in this module");
            auto root = parseNativeProfile(profile.json());
            const auto deferralPath = [](const nlohmann::json &node) -> const nlohmann::json & {
                return node.at("layouts").at("Packet").at("packet_deferral");
            };
            const auto usable = [&](const nlohmann::json &node) {
                return node.contains("layouts") && node.at("layouts").contains("Packet") &&
                       node.at("layouts").at("Packet").contains("packet_deferral") &&
                       deferralPath(node).value("schema", 0) == 2 &&
                       deferralPath(node).contains("handlers");
            };
            if (!usable(root)) {
                const auto fallback = parseNativeProfile(embedded);
                for (const auto *field :
                     {"sha256", "text_sha256", "size_of_image", "text_rva", "text_size"})
                    if (root.at("target").at(field) != fallback.at("target").at(field))
                        throw std::runtime_error(
                            "packet deferral embedded definition belongs to another executable");
                if (!usable(fallback))
                    throw std::runtime_error("packet deferral layout is missing from the profile");
                root = fallback;
            }
            const auto &deferral = deferralPath(root);
            const auto &handlers = deferral.at("handlers");
            if (!handlers.is_array() || handlers.empty() || handlers.size() > kMaxHandlers)
                throw std::runtime_error("unsupported packet deferral handler count");
            s.identifierSize = native_profile::number(deferral.at("network_identifier_size"));
            if (s.identifierSize == 0 || s.identifierSize > kMaxIdentifierBytes)
                throw std::runtime_error("invalid NetworkIdentifier size");
            const auto &shared = deferral.at("shared_ptr");
            s.useOffset = native_profile::number(shared.at("use_offset"));
            s.weakOffset = native_profile::number(shared.at("weak_offset"));
            if (s.useOffset != 8 || s.weakOffset != 12)
                throw std::runtime_error("unsupported shared_ptr control block layout");

            const auto base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
            const auto extent = native_profile::number(root.at("target").at("size_of_image"));
            std::size_t count{};
            for (const auto &handler : handlers) {
                const auto reference = nlohmann::json{{"symbol", handler.at("symbol")}};
                auto *target = native_profile::checked(root, reference);
                const auto vtable = native_profile::number(handler.at("vtable_rva"));
                const auto slot = native_profile::number(handler.at("vtable_slot"));
                if (vtable >= extent || slot >= 512 ||
                    std::uint64_t(vtable) + (slot + 1) * 8 > extent)
                    throw std::runtime_error("packet deferral vtable outside module");
                // The vtable slot must name this entry; that is what makes the
                // handler a (this-)virtual of the profiled class.
                void *slotTarget{};
                if (!native_profile::read(reinterpret_cast<void *>(base + vtable), slot * 8,
                                          slotTarget) ||
                    slotTarget != target)
                    throw std::runtime_error("packet deferral vtable slot mismatch");
                // The handler body must materialise the callable table of its
                // own queued lambda; that pairs the RVA with the overload.
                const auto callable = native_profile::number(handler.at("callable_vtable_rva"));
                const auto referenceOffset = handler.contains("callable_reference_offset")
                    ? native_profile::number(handler.at("callable_reference_offset")) : 0u;
                if (callable >= extent || !referencesAddress(target, callable, referenceOffset))
                    throw std::runtime_error("packet deferral callable identity mismatch");
                auto &binding = s.handlers[count];
                binding = {};
                binding.target = target;
                binding.vtable = base + vtable;
                if (handler.contains("dispatcher")) {
                    const auto &dispatch = handler.at("dispatcher");
                    const auto instance = native_profile::number(dispatch.at("instance_rva"));
                    const auto table = native_profile::number(dispatch.at("table_rva"));
                    const auto dispatchSlot = native_profile::number(dispatch.at("slot"));
                    const auto packetVtable = native_profile::number(handler.at("packet_vtable_rva"));
                    void *observedTable{}, *dispatchEntry{};
                    auto *dispatchTarget = native_profile::checked(root, dispatch.at("target"));
                    if (count + 1 != handlers.size() || count != kMaxHandlers - 1 ||
                        dispatchSlot > 16 || std::uint64_t(instance) + 8 > extent ||
                        std::uint64_t(table) + (dispatchSlot + 1) * 8 > extent ||
                        !packetVtable || packetVtable + 16 > extent ||
                        !native_profile::read(reinterpret_cast<void *>(base + instance), 0,
                                              observedTable) ||
                        reinterpret_cast<std::uintptr_t>(observedTable) != base + table ||
                        !native_profile::read(observedTable, dispatchSlot * 8, dispatchEntry) ||
                        dispatchEntry != dispatchTarget)
                        throw std::runtime_error("packet deferral typed dispatcher mismatch");
                    binding.target = dispatchTarget;
                    binding.dispatcherInstance = reinterpret_cast<void *>(base + instance);
                    binding.packetVtable = base + packetVtable;
                }
                const auto packetId = native_profile::number(handler.at("protocol_id"));
                if (!packetId || packetId > 0xFFFF)
                    throw std::runtime_error("invalid packet deferral class id");
                s.handlers[count].packetId = static_cast<int>(packetId);
                s.handlers[count].delayTolerant = policy && policy(static_cast<int>(packetId));
                const auto label = handler.value("packet_class", std::string("unknown"));
                std::strncpy(s.handlers[count].label, label.c_str(),
                             sizeof(s.handlers[count].label) - 1);
                ++count;
            }
            s.count = count;
            s.base = base;
            s.extent = extent;
            s.enabled.store(false);
            for (std::size_t i = 0; i < count; ++i) {
                auto &binding = s.handlers[i];
                const auto detour = binding.dispatcherInstance
                    ? reinterpret_cast<void *>(&detourDispatch) : reinterpret_cast<void *>(detours[i]);
                auto **original = binding.dispatcherInstance
                    ? reinterpret_cast<void **>(&binding.dispatchOriginal)
                    : reinterpret_cast<void **>(&binding.original);
                if (BDS_HookDirectInstall(binding.target, detour, original,
                                          &s.handlers[i].token))
                    continue;
                for (std::size_t done = 0; done < i; ++done) {
                    BDS_HookDirectRemove(s.handlers[done].token);
                    s.handlers[done].token = nullptr;
                    s.handlers[done].original = nullptr;
                }
                s.count = 0;
                throw std::runtime_error("packet deferral hook conflict");
            }
            s.installed = true;
            // The detour outlives this object (and may outlive an unload), so
            // pin the module exactly like the other native adapters do.
            HMODULE pinned{};
            GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                   GET_MODULE_HANDLE_EX_FLAG_PIN,
                               reinterpret_cast<LPCWSTR>(&detours[0]), &pinned);
            error.clear();
            return true;
        } catch (const std::exception &e) {
            error = e.what();
            return false;
        }
    }

    bool ready() const noexcept {
        return state().installed;
    }
    void setEnabled(bool value) noexcept {
        state().enabled.store(value, std::memory_order_release);
    }
    bool enabled() const noexcept {
        return state().enabled.load(std::memory_order_acquire);
    }

    // Game thread: replay at most `budget` held packets in FIFO order. Entries
    // older than kMaxAgeMs are always drained (up to a hard cap) so a stalled
    // frame cannot keep a block update pending indefinitely.
    unsigned pump(unsigned budget, unsigned hardCap = 256) {
        auto &s = state();
        unsigned done = 0;
        for (;;) {
            Entry entry;
            bool aged = false;
            {
                std::lock_guard lock(s.mutex);
                if (!s.queueCount) break;
                if (done >= budget) {
                    if (done >= hardCap) break;
                    const auto now = stamp();
                    const auto &head = s.queue[s.head];
                    if (now - head.stamp < kMaxAgeMs) break;
                    aged = true;
                }
                entry = s.queue[s.head];
                s.head = (s.head + 1) % kQueueCapacity;
                --s.queueCount;
            }
            if (aged) s.agedOut.fetch_add(1);
            replay(entry);
            ++done;
        }
        return done;
    }

    // Game thread: drop everything (world change). The packets belong to the
    // old level, so replaying them would apply stale block updates.
    unsigned clear() {
        auto &s = state();
        unsigned dropped = 0;
        for (;;) {
            Entry entry;
            {
                std::lock_guard lock(s.mutex);
                if (!s.queueCount) break;
                entry = s.queue[s.head];
                s.head = (s.head + 1) % kQueueCapacity;
                --s.queueCount;
            }
            releaseControl(entry.control);
            ++dropped;
        }
        if (dropped) s.worldDrops.fetch_add(dropped);
        return dropped;
    }

    Statistics takeStatistics() noexcept {
        auto &s = state();
        unsigned queue{};
        {
            std::lock_guard lock(s.mutex);
            queue = s.queueCount;
        }
        Statistics result{s.calls.exchange(0), s.deferred.exchange(0), s.replayed.exchange(0),
                          s.overflow.exchange(0), s.otherObject.exchange(0),
                          s.otherClass.exchange(0), s.worldDrops.exchange(0),
                          s.agedOut.exchange(0), queue};
        for (std::size_t i = 0; i < s.count; ++i)
            result.byHandler[i] = {s.handlers[i].packetId, s.handlerCalls[i].exchange(0),
                                   s.handlerDeferred[i].exchange(0), s.handlerReplayed[i].exchange(0)};
        return result;
    }
    unsigned queueDepth() const noexcept {
        auto &s = state();
        std::lock_guard lock(s.mutex);
        return s.queueCount;
    }
    unsigned handlerCount() const noexcept {
        return static_cast<unsigned>(state().count);
    }
    // Packet-class label of handler `index` (from the profile), for logs.
    const char *handlerLabel(unsigned index) const noexcept {
        auto &s = state();
        return index < s.count ? s.handlers[index].label : "";
    }
    // Diagnostics: the packet-class label of handler index i, for logs.
    std::uintptr_t handlerRva(unsigned index) const noexcept {
        auto &s = state();
        if (index >= s.count || !s.base) return 0;
        return reinterpret_cast<std::uintptr_t>(s.handlers[index].target) - s.base;
    }

  private:
    struct Entry {
        void *self{};
        void *dispatcher{};
        void *object{};
        void *control{};
        long long stamp{};
        unsigned handler{};
        alignas(16) unsigned char identifier[kMaxIdentifierBytes]{};
    };
    struct HandlerSlot {
        void *target{};
        std::uintptr_t vtable{};
        Handler original{};
        void *dispatcherInstance{};
        Dispatcher dispatchOriginal{};
        BDS_HookToken token{};
        char label[32]{};
        int packetId{};
        bool delayTolerant{};
        std::uintptr_t packetVtable{};
    };
    struct State {
        std::mutex mutex;
        std::array<Entry, kQueueCapacity> queue{};
        std::array<HandlerSlot, kMaxHandlers> handlers{};
        std::size_t count{};
        unsigned head{}, queueCount{};
        unsigned identifierSize{};
        unsigned useOffset{8}, weakOffset{12};
        std::uintptr_t base{}, extent{};
        bool installed{};
        std::atomic<bool> enabled{false};
        std::atomic<unsigned long long> calls{}, deferred{}, replayed{}, overflow{}, otherObject{},
            otherClass{}, worldDrops{}, agedOut{};
        std::array<std::atomic<unsigned>, kMaxHandlers> handlerCalls{}, handlerDeferred{},
            handlerReplayed{};
    };
    static State &state() {
        static State *s = new State;
        return *s;
    }
    static long long stamp() noexcept {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::steady_clock::now().time_since_epoch())
            .count();
    }
    static bool referencesAddress(void *function, std::uint32_t target,
                                  unsigned referenceOffset) noexcept {
        if (referenceOffset > 0x2000) return false;
        const auto start = referenceOffset;
        const auto stop = referenceOffset ? referenceOffset + 1 : 0x200 - 6;
        for (unsigned offset = start; offset < stop; ++offset) {
            unsigned char bytes[7]{};
            const auto *at = static_cast<const unsigned char *>(function) + offset;
            if (!native_profile::read(at, 0, bytes) ||
                (bytes[0] != 0x48 && bytes[0] != 0x4C) || bytes[1] != 0x8D ||
                (bytes[2] & 0xC7) != 0x05)
                continue;
            std::int32_t displacement{};
            std::memcpy(&displacement, bytes + 3, 4);
            const auto instruction = reinterpret_cast<std::uintptr_t>(at);
            const auto address = instruction + 7 + static_cast<std::intptr_t>(displacement);
            if (address - reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr)) == target)
                return true;
        }
        return false;
    }
    // Release one use count with the same control-block protocol the handler's
    // own epilogue uses (uses at +8, weaks at +0xC, _Destroy then _Delete_this).
    static void releaseControl(void *control) noexcept {
        if (!control) return;
        auto *uses = reinterpret_cast<volatile long *>(static_cast<char *>(control) + 8);
        if (_InterlockedDecrement(uses) != 0) return;
        auto **vtable = *reinterpret_cast<void ***>(control);
        using Virtual = void(__fastcall *)(void *);
        if (vtable && vtable[0]) reinterpret_cast<Virtual>(vtable[0])(control);
        auto *weak = reinterpret_cast<volatile long *>(static_cast<char *>(control) + 12);
        if (_InterlockedDecrement(weak) != 0) return;
        if (vtable && vtable[1]) reinterpret_cast<Virtual>(vtable[1])(control);
    }
    static void replay(Entry &entry) noexcept {
        auto &s = state();
        if (entry.handler >= s.count) {
            releaseControl(entry.control);
            return;
        }
        const auto &handler = s.handlers[entry.handler];
        std::uintptr_t vtable{};
        if (!native_profile::read(entry.self, 0, vtable) || vtable != handler.vtable) {
            s.otherObject.fetch_add(1, std::memory_order_relaxed);
            releaseControl(entry.control);
            return;
        }
        struct RawShared {
            void *object;
            void *control;
        } shared{entry.object, entry.control};
        s.replayed.fetch_add(1);
        s.handlerReplayed[entry.handler].fetch_add(1, std::memory_order_relaxed);
        if (entry.dispatcher) {
            handler.dispatchOriginal(entry.dispatcher, entry.identifier, entry.self, &shared);
            releaseControl(entry.control); // dispatcher borrowed the retained reference
        } else {
            // The by-value handler consumes the callee-owned reference.
            handler.original(entry.self, entry.identifier, &shared);
        }
    }
    static bool tryHold(unsigned index, void *self, const void *identifier,
                        const void *shared, void *dispatcher) noexcept {
        auto &s = state();
        s.calls.fetch_add(1);
        const auto &handler = s.handlers[index];
        if (!self || !identifier || !shared || dispatcher != handler.dispatcherInstance)
            return false;
        // Live identity: the object must still carry the profiled vtable.
        void *vtable{};
        if (!native_profile::read(self, 0, vtable) ||
            reinterpret_cast<std::uintptr_t>(vtable) != handler.vtable) {
            s.otherObject.fetch_add(1);
            return false;
        }
        // The typed dispatcher is a per-class singleton, but prove the packet
        // itself is the profiled class before holding anything.
        if (dispatcher) {
            std::uintptr_t packetVtable{};
            void *object{};
            std::memcpy(&object, shared, sizeof(void *));
            if (!object || !native_profile::read(object, 0, packetVtable) ||
                packetVtable != handler.packetVtable) {
                s.otherClass.fetch_add(1, std::memory_order_relaxed);
                return false;
            }
        }
        s.handlerCalls[index].fetch_add(1, std::memory_order_relaxed);
        if (!handler.delayTolerant || !s.enabled.load(std::memory_order_acquire))
            return false;
        Entry entry;
        entry.self = self;
        entry.dispatcher = dispatcher;
        entry.handler = index;
        std::memcpy(&entry.object, shared, sizeof(void *));
        std::memcpy(&entry.control, static_cast<const char *>(shared) + sizeof(void *), sizeof(void *));
        std::memcpy(entry.identifier, identifier, s.identifierSize);
        if (!entry.object || (dispatcher && !entry.control)) return false;
        entry.stamp = stamp();
        bool queued = false;
        {
            std::lock_guard lock(s.mutex);
            if (s.queueCount < kQueueCapacity && s.enabled.load(std::memory_order_acquire)) {
                // The by-value paths transfer a reference. A typed dispatcher
                // only borrows, so retain our own reference before returning.
                if (dispatcher)
                    _InterlockedIncrement(reinterpret_cast<volatile long *>(
                        static_cast<char *>(entry.control) + s.useOffset));
                const auto tail = (s.head + s.queueCount) % kQueueCapacity;
                s.queue[tail] = entry;
                ++s.queueCount;
                queued = true;
            } else if (s.queueCount == kQueueCapacity) {
                s.overflow.fetch_add(1, std::memory_order_relaxed);
            }
        }
        if (!queued) return false;
        s.deferred.fetch_add(1);
        s.handlerDeferred[index].fetch_add(1, std::memory_order_relaxed);
        return true;
    }
    template <unsigned Index>
    static void __fastcall detourAt(void *self, const void *identifier, void *shared) noexcept {
        auto &handler = state().handlers[Index];
        if (!tryHold(Index, self, identifier, shared, nullptr) && handler.original)
            handler.original(self, identifier, shared);
    }
    static void __fastcall detourDispatch(void *dispatcher, const void *identifier,
                                         void *self, const void *shared) noexcept {
        constexpr auto index = kMaxHandlers - 1;
        auto &handler = state().handlers[index];
        if (!tryHold(index, self, identifier, shared, dispatcher) && handler.dispatchOriginal)
            handler.dispatchOriginal(dispatcher, identifier, self, shared);
    }
    static constexpr Handler detours[kMaxHandlers] = {
        &NativePacketDeferral::detourAt<0>, &NativePacketDeferral::detourAt<1>,
        &NativePacketDeferral::detourAt<2>, &NativePacketDeferral::detourAt<3>,
        &NativePacketDeferral::detourAt<4>};
};
} // namespace bedrock
