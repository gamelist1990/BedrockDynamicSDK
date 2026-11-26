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
// The three handler overloads this adapter knows are
// `LegacyClientNetworkHandler::handle(NetworkIdentifier const&,
// std::shared_ptr<UpdateBlockPacket>)` and its two siblings. They are ordinary
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
    static constexpr unsigned kMaxHandlers = 4;
    static constexpr unsigned kQueueCapacity = 512;
    static constexpr unsigned kMaxIdentifierBytes = 256;
    static constexpr unsigned kMaxAgeMs = 400;
    using Handler = void(__fastcall *)(void *self, const void *identifier, void *shared);

    struct Statistics {
        unsigned long long calls{}, deferred{}, replayed{}, overflow{}, otherObject{},
            worldDrops{}, agedOut{};
        unsigned queue{};
    };

    bool install(const RuntimeProfile &profile, std::string_view embedded, std::string &error) {
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
                       node.at("layouts").at("Packet").at("packet_deferral").contains("handlers");
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
            if (deferral.value("schema", 0) != 1)
                throw std::runtime_error("unsupported packet deferral schema");
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
                if (!referencesAddress(target, callable))
                    throw std::runtime_error("packet deferral callable identity mismatch");
                s.handlers[count] = {target, base + vtable, slot, nullptr, nullptr};
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
                if (BDS_HookDirectInstall(s.handlers[i].target, detours[i],
                                          reinterpret_cast<void **>(&s.handlers[i].original),
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
        return {s.calls.exchange(0), s.deferred.exchange(0), s.replayed.exchange(0),
                s.overflow.exchange(0), s.otherObject.exchange(0), s.worldDrops.exchange(0),
                s.agedOut.exchange(0), queue};
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
        void *object{};
        void *control{};
        long long stamp{};
        unsigned handler{};
        alignas(16) unsigned char identifier[kMaxIdentifierBytes]{};
    };
    struct HandlerSlot {
        void *target{};
        std::uintptr_t vtable{};
        unsigned slot{};
        Handler original{};
        BDS_HookToken token{};
        char label[32]{};
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
            worldDrops{}, agedOut{};
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
    static bool referencesAddress(void *function, std::uint32_t target) noexcept {
        const auto bytes = static_cast<const unsigned char *>(function);
        for (unsigned offset = 0; offset + 7 <= 0x200; ++offset) {
            const auto *at = bytes + offset;
            if ((at[0] != 0x48 && at[0] != 0x4C) || at[1] != 0x8D || (at[2] & 0xC7) != 0x05)
                continue;
            std::int32_t displacement{};
            std::memcpy(&displacement, at + 3, 4);
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
        const auto original = s.handlers[entry.handler].original;
        if (!original) {
            releaseControl(entry.control);
            return;
        }
        struct RawShared {
            void *object;
            void *control;
        } shared{entry.object, entry.control};
        s.replayed.fetch_add(1);
        // The callee owns the by-value shared_ptr and releases that use count in
        // its epilogue, which is exactly the reference this entry holds.
        original(entry.self, entry.identifier, &shared);
    }
    template <unsigned Index>
    static void __fastcall detourAt(void *self, const void *identifier, void *shared) noexcept {
        auto &s = state();
        s.calls.fetch_add(1);
        auto &handler = s.handlers[Index];
        const auto proceed = [&] { handler.original(self, identifier, shared); };
        if (!handler.original || !self || !identifier || !shared) return proceed();
        // Live identity: the object must still carry the profiled vtable.
        void *vtable{};
        if (!native_profile::read(self, 0, vtable) ||
            reinterpret_cast<std::uintptr_t>(vtable) != handler.vtable) {
            s.otherObject.fetch_add(1);
            return proceed();
        }
        if (!s.enabled.load(std::memory_order_acquire)) return proceed();
        Entry entry;
        entry.self = self;
        entry.handler = Index;
        std::memcpy(&entry.object, shared, sizeof(void *));
        std::memcpy(&entry.control, static_cast<char *>(shared) + sizeof(void *), sizeof(void *));
        std::memcpy(entry.identifier, identifier, s.identifierSize);
        entry.stamp = stamp();
        {
            std::lock_guard lock(s.mutex);
            if (s.queueCount >= kQueueCapacity) {
                s.overflow.fetch_add(1);
                return proceed();
            }
            // The by-value shared_ptr argument is owned by the callee (the
            // original handler releases it in its epilogue), so a deferred call
            // simply takes over that one reference instead of adding another.
            const auto tail = (s.head + s.queueCount) % kQueueCapacity;
            s.queue[tail] = entry;
            ++s.queueCount;
        }
        s.deferred.fetch_add(1);
    }
    static constexpr Handler detours[kMaxHandlers] = {
        &NativePacketDeferral::detourAt<0>, &NativePacketDeferral::detourAt<1>,
        &NativePacketDeferral::detourAt<2>, &NativePacketDeferral::detourAt<3>};
};
} // namespace bedrock
