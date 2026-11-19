#pragma once
#include <bedrock/api/automation_controls.hpp>
#include <bedrock/detail/automation_input.hpp>
#include <bedrock/detail/native_player_context.hpp>
#include <bedrock/detail/native_profile_functions.hpp>
#include <bedrock/runtime/hook_runtime.h>
#include <bedrock/native/packet_sender.hpp>
#include <bedrock/packets/wire_packet_events.hpp>
#include <bedrock/packets/movement_packets.hpp>
#include <array>
#include <atomic>
#include <chrono>
#include <mutex>
#include <type_traits>

namespace bedrock {
// A borrowed movement override around ONE engine simulation call. No keyboard
// feed, cursor capture, velocity write or position write is used. Sprint jumps
// borrow actor yaw inside simulation and restore it before returning to rendering.
class NativeAutomationControls {
public:
    ~NativeAutomationControls() { stop(); }
    bool load(const RuntimeProfile& profile, std::string& error) {
        try {
            const auto root = parseNativeProfile(profile.json());
            const auto& layout = root.at("layouts").at("AutomationControls");
            if (layout.at("schema") != 1 || layout.at("pointer_size") != 8)
                throw std::runtime_error("unsupported independent movement layout");
            hash_ = native_profile::number(layout.at("component_hash"));
            const auto componentSize = native_profile::number(layout.at("component_size"));
            if (componentSize < 12 || componentSize > 512)
                throw std::runtime_error("invalid independent input component size");
            const auto& offsets = layout.at("fields");
            fields_[0] = native_profile::number(offsets.at("move_left"));
            fields_[1] = native_profile::number(offsets.at("move_forward"));
            fields_[2] = native_profile::number(offsets.at("input_flags"));
            fields_[3] = native_profile::number(offsets.at("held_flags"));
            for (std::size_t i = 0; i < fields_.size(); ++i) {
                if (fields_[i] + 4 > componentSize) throw std::runtime_error("independent input field out of range");
                for (std::size_t j = 0; j < i; ++j)
                    if (fields_[i] < fields_[j] + 4 && fields_[j] < fields_[i] + 4)
                        throw std::runtime_error("overlapping independent input fields");
            }
            const auto& masks = layout.at("flag_masks");
            ownedMask_ = 0;
            constexpr const char* names[]{"forward", "backward", "left", "right",
                "jump", "sneak", "sprint", "forward_left", "forward_right",
                "backward_left", "backward_right", "jump_pressed", "jump_released",
                "sneak_pressed", "sneak_released"};
            for (std::size_t i = 0; i < masks_.size(); ++i) {
                masks_[i] = native_profile::number(masks.at(names[i]));
                if (!masks_[i] || (ownedMask_ & masks_[i]))
                    throw std::runtime_error("invalid independent input flag mask");
                ownedMask_ |= masks_[i];
            }
            if (!hash_) throw std::runtime_error("independent input component missing");
            // Static field evidence is tied to the exact consumer function.
            native_profile::checked(root, layout.at("consumer_guard"));
            if (!context_.load(profile, error)) throw std::runtime_error(error);
            const auto& tick = root.at("symbols").at("MultiPlayerLevel::_subTick");
            target_ = reinterpret_cast<void*>(reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr)) +
                native_profile::number(tick.at("rva")));
            if (!BDS_HookOwnsTarget(BDS_HOOK_LEVEL_SUB_TICK, target_)) native_profile::checked(root, tick);
            if (!BDS_HookSubscribe(BDS_HOOK_LEVEL_SUB_TICK, target_, &onSimulation, this, &token_))
                throw std::runtime_error("independent movement hook unavailable");
            senderLayout_.load(root); wire_.load(root);
            if (!wire_.subscribe(&onWire, this, &wireToken_))
                throw std::runtime_error("independent rotation wire hook unavailable");
            error.clear(); return true;
        } catch (const std::exception& e) { stop(); error = e.what(); return false; }
    }
    void stop() noexcept {
        clear();
        if (token_) { BDS_HookUnsubscribe(token_); token_ = nullptr; }
        if (wireToken_) { BDS_HookUnsubscribe(wireToken_); wireToken_ = nullptr; }
        if (senderToken_) { BDS_HookUnsubscribe(senderToken_); senderToken_ = nullptr; }
    }
    void update(void* client, std::uint64_t epoch, const AutomationIntent& intent, float cameraYaw,
                bool manualOverride = false) {
        if (client && !senderToken_) {
            void* sender{}; void* target{};
            if (senderLayout_.resolve(client, sender, target)) {
                DWORD64 image{};
                const auto* function = RtlLookupFunctionEntry(reinterpret_cast<DWORD64>(target), &image, nullptr);
                if ((BDS_HookOwnsTarget(BDS_HOOK_PACKET_SEND_TO_SERVER, target) ||
                     (function && image + function->BeginAddress == reinterpret_cast<DWORD64>(target))) &&
                    BDS_HookSubscribe(BDS_HOOK_PACKET_SEND_TO_SERVER, target, &onSender, this, &senderToken_))
                    sender_ = sender;
            }
        } else if (client && senderToken_) {
            void* sender{}; void* target{};
            if (senderLayout_.resolve(client, sender, target)) sender_ = sender;
        }
        std::lock_guard lock(mutex_);
        client_ = client; epoch_ = epoch; intent_ = intent; cameraYaw_ = cameraYaw;
        manualOverride_ = manualOverride;
        published_ = std::chrono::steady_clock::now();
    }
    void clear() noexcept {
        std::lock_guard lock(mutex_); intent_ = {}; client_ = nullptr; context_.reset(); applied_ = false;
        manualOverride_.store(false, std::memory_order_release);
    }
    void manualInput(unsigned channel, bool down) noexcept {
        if (channel >= 32) return;
        const auto bit = std::uint32_t{1} << channel;
        if (down) manualHeld_.fetch_or(bit); else manualHeld_.fetch_and(~bit);
        manualUntil_ = GetTickCount64() + 750;
    }
    // UI typing/clicks are not gameplay input, including key-up events.
    void clearManualInput() noexcept {
        manualHeld_.store(0);
        manualUntil_.store(0);
    }
    bool manualActive() const noexcept {
        return !manualOverride_.load(std::memory_order_acquire) &&
            (manualHeld_.load() || GetTickCount64() < manualUntil_.load());
    }
    bool applied() const noexcept { return applied_.load(); }
    AutomationInputObservation observation() const {
        std::lock_guard lock(mutex_); return observation_;
    }
private:
    static void BDS_HOOK_CALL onSender(void*, void* raw, BDS_HookNext next, void* frame) {
        next(frame, raw); // broker attributes synchronous wire events to this sender
    }
    static void BDS_HOOK_CALL onWire(void* user, void* raw, BDS_HookNext next, void* frame) {
        auto& self = *static_cast<NativeAutomationControls*>(user);
        auto& args = *static_cast<BDS_PacketWireArgs*>(raw);
        AutomationIntent intent; bool current{};
        {
            std::lock_guard lock(self.mutex_); intent = self.appliedIntent_;
            current = std::chrono::steady_clock::now() - self.published_ < std::chrono::milliseconds(500);
        }
        if (!current || !intent.active || !intent.look || !self.applied_ || self.manualActive() ||
            !args.sender || args.sender != self.sender_.load()) { next(frame, raw); return; }
        if (!self.wire_.dispatch(args, [&](PacketWireSendEvent& event) {
            if (!MovementPackets::handles(event.id())) return;
            const auto move = automationMove(intent, intent.yaw);
            self.codec_.automate(event.packet(), intent.yaw, intent.pitch, move.left, move.forward);
        }, next, frame)) next(frame, raw);
    }
    struct Patch {
        void* component{};
        std::size_t offset{}, size{};
        std::array<std::byte, 4> before{}, after{};
        std::uint32_t mask{};
        bool simulationOnly{};
    };
    template<class T>
    bool patch(void* component, std::size_t offset, T value, Patch& record,
               bool analog = true) noexcept {
        T before{};
        if (!world_native::read(component, offset, before)) return false;
        if constexpr (std::is_floating_point_v<T>) {
            if (!std::isfinite(before) || !std::isfinite(value)) return false;
            if (analog && std::abs(before) > 1.01f) return false;
        }
        record.component = component; record.offset = offset; record.size = sizeof(T);
        std::memcpy(record.before.data(), &before, sizeof(T));
        std::memcpy(record.after.data(), &value, sizeof(T));
        return world_native::write(component, offset, value);
    }
    static bool readBytes(void* p, std::size_t at, void* dst, std::size_t size) noexcept {
        __try { std::memcpy(dst, static_cast<const std::byte*>(p) + at, size); return true; }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
    static void restore(const Patch& patch) noexcept {
        if (!patch.component) return;
        std::array<std::byte, 4> current{};
        if (!readBytes(patch.component, patch.offset, current.data(), patch.size)) return;
        if (patch.mask) {
            std::uint32_t live{}, before{};
            std::memcpy(&live, current.data(), sizeof(live));
            std::memcpy(&before, patch.before.data(), sizeof(before));
            const auto restored = (live & ~patch.mask) | (before & patch.mask);
            world_native::write(patch.component, patch.offset, restored);
            return;
        }
        if (patch.size == sizeof(float)) {
            float live{}, before{}, injected{};
            std::memcpy(&live, current.data(), sizeof(live));
            std::memcpy(&before, patch.before.data(), sizeof(before));
            std::memcpy(&injected, patch.after.data(), sizeof(injected));
            const float restored = detail::automationRestoredAxis(
                live, before, injected, patch.simulationOnly);
            if (patch.simulationOnly || live == injected)
                world_native::write(patch.component, patch.offset, restored);
            return;
        }
        if (std::memcmp(current.data(), patch.after.data(), patch.size)) return;
        // Restore only values still owned by this override. Engine changes
        // made during the simulation are kept.
        __try { std::memcpy(static_cast<std::byte*>(patch.component) + patch.offset,
                           patch.before.data(), patch.size); }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
    static void BDS_HOOK_CALL onSimulation(void* user, void* raw, BDS_HookNext next, void* frame) {
        auto& self = *static_cast<NativeAutomationControls*>(user);
        std::array<Patch, 5> patches{};
        AutomationInputObservation observation;
        AutomationIntent intent; void* client{}; float camera{}; std::uint64_t epoch{};
        bool current = false;
        {
            std::lock_guard lock(self.mutex_);
            intent = self.intent_; client = self.client_; camera = self.cameraYaw_; epoch = self.epoch_;
            current = std::chrono::steady_clock::now() - self.published_ < std::chrono::milliseconds(500);
        }
        auto scope = self.context_.tick(client, WorldEpoch{epoch});
        bool ok = current && intent.active && client && !self.manualActive() && self.context_.valid();
        if (ok) {
            void* input = self.context_.component(self.hash_);
            float liveYaw{};
            const auto& actor = self.context_.layout();
            void* rotation = self.context_.component(actor.actorRotationComponentHash);
            const bool haveYaw = world_native::read(rotation, actor.rotationYawOffset, liveYaw) &&
                std::isfinite(liveYaw);
            if (haveYaw) camera = liveYaw;
            if (intent.sprintJump) {
                // Forward/sprint gating and the jump's horizontal boost both use
                // actor yaw. Rotating only the analog vector leaves these in the
                // user's camera basis (possibly sideways/backwards).
                ok = haveYaw && std::isfinite(intent.yaw) &&
                    self.patch(rotation, actor.rotationYawOffset, intent.yaw, patches[4], false);
                patches[4].simulationOnly = true;
                if (ok) camera = automationSimulationYaw(intent, camera);
            }
            const auto move = automationMove(intent, camera);
            ok = ok && input && self.patch(input, self.fields_[0], move.left, patches[0]) &&
                self.patch(input, self.fields_[1], move.forward, patches[1]);
            std::uint32_t previous{}, mirrorPrevious{};
            ok = ok && world_native::read(input, self.fields_[2], previous) &&
                world_native::read(input, self.fields_[3], mirrorPrevious);
            const auto flags = detail::automationFlags(intent, move, previous, mirrorPrevious,
                                                       self.lastJump_, self.lastSneak_, self.masks_);
            if (ok) {
                ok = self.patch(input, self.fields_[2], flags.input, patches[2]);
                patches[2].mask = self.ownedMask_;
            }
            if (ok) {
                // The engine keeps a held-only mirror of the flags word at
                // +0x10 (edge bits stripped), written by its own input
                // gather; measured on 1.26.65.1 with real keys (jump held
                // shows 0x04000080 in BOTH words, jump_pressed 0x2000000
                // only in the first). Jump physics reads the mirror, so
                // patching only the first word moves but never jumps.
                // Mirror the same held state, without the edge bits.
                ok = self.patch(input, self.fields_[3], flags.held, patches[3]);
                patches[3].mask = flags.heldMask;
            }
            observation.forward = move.forward; observation.left = move.left;
            observation.yaw = camera; observation.input = flags.input;
            observation.held = flags.held; observation.sprintMask = self.masks_[6];
        }
        if (!ok) for (auto it = patches.rbegin(); it != patches.rend(); ++it) {
            restore(*it);
            *it = {}; // rollback once; next() runs without a borrowed yaw on failure
        }
        self.applied_ = ok;
        { std::lock_guard lock(self.mutex_);
          self.appliedIntent_ = ok ? intent : AutomationIntent{};
          observation.applied = ok; observation.sprintJump = intent.sprintJump;
          observation.sequence = self.observation_.sequence + 1;
          observation.sprintCalls = self.observation_.sprintCalls +
              (ok && intent.sprintJump && intent.sprint && observation.forward > .99f &&
               (observation.input & observation.sprintMask) &&
               (observation.held & observation.sprintMask) ? 1 : 0);
          self.observation_ = observation;
        }
        self.lastJump_ = ok && intent.jump; self.lastSneak_ = ok && intent.sneak;
        try { next(frame, raw); }
        catch (...) {
            if (self.context_.valid())
                for (auto it = patches.rbegin(); it != patches.rend(); ++it) restore(*it);
            self.applied_ = false; self.lastJump_ = self.lastSneak_ = false;
            throw;
        }
        if (self.context_.valid())
            for (auto it = patches.rbegin(); it != patches.rend(); ++it) restore(*it);
    }
    native_player::Context context_;
    BDS_HookToken token_{};
    BDS_HookToken wireToken_{}, senderToken_{};
    NativePacketSender senderLayout_;
    PacketWireEvents wire_;
    MovementPackets codec_;
    std::atomic<void*> sender_{};
    void* target_{};
    std::uint32_t hash_{};
    std::array<std::size_t, 4> fields_{};
    std::array<std::uint32_t, 15> masks_{};
    std::uint32_t ownedMask_{};
    mutable std::mutex mutex_;
    void* client_{};
    std::uint64_t epoch_{};
    AutomationIntent intent_{}, appliedIntent_{};
    AutomationInputObservation observation_{};
    float cameraYaw_{};
    std::chrono::steady_clock::time_point published_{};
    std::atomic<std::uint32_t> manualHeld_{};
    std::atomic<ULONGLONG> manualUntil_{};
    std::atomic<bool> manualOverride_{};
    std::atomic<bool> applied_{};
    bool lastJump_{}, lastSneak_{}; // simulation thread only
};
} // namespace bedrock
