#pragma once
#include <bedrock/native/world_access.hpp>
#include <bedrock/detail/block_action_report.hpp>
#include <bedrock/detail/native_profile_functions.hpp>
#include <array>
#include <vector>
#include <sstream>
#include <atomic>
#include <mutex>
#include <bedrock/runtime/hook_runtime.h>

namespace bedrock {
// Targeted GameMode operations. The game's survival break timer, selected
// tool, reach checks and normal transaction path remain authoritative.
class NativeBlockActions {
public:
    ~NativeBlockActions() { cancelMiningOwnership(); }
    // Physical input and explicit bot cancellation relinquish this lease
    // immediately. No native progress or time value is fabricated/restored.
    void cancelMiningOwnership() const noexcept {
        auto& s=stopGuard();
        const NativeBlockActions* owner=this;
        s.owner.compare_exchange_strong(owner,nullptr);
    }
    bool load(const RuntimeProfile& profile, NativeWorldAccess& world, std::string& error) {
        world_ = nullptr;
        try {
            const auto root = parseNativeProfile(profile.json());
            const auto& layout = root.at("layouts").at("BlockActions");
            if (layout.at("schema") != 1 || layout.at("pointer_size") != 8)
                throw std::runtime_error("unsupported targeted block action layout");
            constexpr const char* names[]{"start_destroy", "continue_destroy", "stop_destroy",
                "use_item", "release_item", "attack_actor"};
            for (std::size_t i = 0; i < functions_.size(); ++i) {
                const auto& action = layout.at(names[i]);
                functions_[i].clear();
                functions_[i].push_back(native_profile::checked(root, action.at("function")));
                if (action.contains("alternate_functions")) {
                    const auto& alternates = action.at("alternate_functions");
                    if (!alternates.is_array() || alternates.size() > 3)
                        throw std::runtime_error("invalid block action alternate bindings");
                    for (const auto& ref : alternates) {
                        auto* function = native_profile::checked(root, ref);
                        if (std::find(functions_[i].begin(), functions_[i].end(), function) != functions_[i].end())
                            throw std::runtime_error("duplicate block action binding");
                        functions_[i].push_back(function);
                    }
                }
                slots_[i] = native_profile::number(action.at("slot"));
                if (slots_[i] > 64) throw std::runtime_error("invalid block action virtual slot");
            }
            clockTick_ = native_profile::checked(root, layout.at("clock_tick"));
            if (layout.contains("destroy_state")) {
                const auto& state = layout.at("destroy_state");
                destroyTargetOffset_ = static_cast<std::size_t>(native_profile::number(state.at("target_offset")));
                destroyProgressOffset_ = static_cast<std::size_t>(native_profile::number(state.at("progress_offset")));
                if (state.contains("progress_mirror_offset"))
                    destroyProgressMirrorOffset_ = static_cast<std::size_t>(
                        native_profile::number(state.at("progress_mirror_offset")));
                if (destroyTargetOffset_ > 512 || destroyProgressOffset_ > 512)
                    throw std::runtime_error("invalid destroy state offsets");
                if (state.contains("destroying_flag_offset")) {
                    destroyingFlagOffset_ = static_cast<std::size_t>(
                        native_profile::number(state.at("destroying_flag_offset")));
                    if (destroyingFlagOffset_ > 65536)
                        throw std::runtime_error("invalid destroying flag offset");
                }
                if (state.contains("timer_offset")) {
                    timerOffset_ = static_cast<std::size_t>(native_profile::number(state.at("timer_offset")));
                    timerCounterOffset_ = static_cast<std::size_t>(native_profile::number(state.at("timer_counter_offset")));
                    if (timerOffset_ > 512 || timerCounterOffset_ > 512)
                        throw std::runtime_error("invalid destroy timer layout");
                }
            }
            base_ = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
            imageSize_ = native_profile::number(root.at("target").at("size_of_image"));
            world_ = &world; error.clear(); return true;
        } catch (const std::exception& e) { error = e.what(); return false; }
    }
    bool mine(void* player, const BlockPos& block, unsigned face, Vec3 eye, Vec3 hit,
              bool begin, bool& destroyed, BlockActionReport* output = nullptr) const noexcept {
        BlockActionReport local;
        auto& report = output ? *output : local;
        report = {};
        destroyed = false;
        if (!world_) { report.failure = BlockActionFailure::NotLoaded; return false; }
        if (face > 5) { report.failure = BlockActionFailure::InvalidFace; return false; }
        void* mode = world_->gameMode(player);
        if (!mode) { report.failure = BlockActionFailure::GameModeUnavailable; return false; }
        const std::size_t index = begin ? 0 : 1;
        auto* function = resolve(mode, index, &report);
        if (!function) { report.failure = BlockActionFailure::VirtualFunctionMismatch; return false; }
        if (!installStopGuard()) { report.failure=BlockActionFailure::NativeFault; return false; }
        BlockPos previousTarget{};
        (void)destroyState(player,report.progressBefore,previousTarget);
        // The continue call takes the real hit point, not the eye: the
        // ABI's position parameter is the hit position on the block face
        // (passing the eye made the game validate the eye's own cell).
        // Set on EVERY call, not just begin: the idle input driver
        // owns the flag and clears it while no real attack is held,
        // so a begin-only write is gone by the next tick's continue.
        if (!advanceClock(mode, begin)) { report.failure = BlockActionFailure::NativeFault; return false; }
        setDestroyingFlag(player, true);
        bool entered = false;
        const bool scoped = world_->withBlockHit(player, block, face, eye, hit, [&]() noexcept {
            entered = true;
            NativeActionScope action;
            report.called = invoke(mode, function, block, face, hit, begin, destroyed, report.accepted);
            return report.called;
        });
        if (!scoped) report.failure = entered ? BlockActionFailure::NativeFault : BlockActionFailure::HitContextUnavailable;
        else if (!report.accepted) report.failure = BlockActionFailure::GameRejected;
        if (scoped && report.accepted && !destroyed) {
            auto& s=stopGuard();
            s.mode.store(mode);
            s.until.store(GetTickCount64()+150);
            s.owner.store(this);
        } else {
            cancelMiningOwnership();
            if (!scoped || (begin && !report.accepted)) setDestroyingFlag(player,false);
        }
        report.idleStopsSuppressed=stopGuard().suppressed.load();
        return scoped && report.accepted;
    }
    // Read the engine-owned target and real progress. No completion is
    // inferred from this value; callers must observe the block disappearing.
    bool destroyState(void* player, float& progress, BlockPos& target) const noexcept {
        progress = 0; target = {-1, -1, -1};
        if (!world_ || destroyProgressOffset_ == kNoOffset) return false;
        void* mode = world_->gameMode(player);
        if (!mode) return false;
        int x = -1, y = -1, z = -1; float value = 0;
        if (!world_->readField(mode, destroyTargetOffset_, x) ||
            !world_->readField(mode, destroyTargetOffset_ + 4, y) ||
            !world_->readField(mode, destroyTargetOffset_ + 8, z) ||
            !world_->readField(mode, destroyProgressOffset_, value)) return false;
        if (!std::isfinite(value) || value<0.f || value>1.01f) return false;
        progress = value; target = {x, y, z}; return true;
    }
    static std::string diagnostic(const BlockActionReport& report) {
        std::ostringstream out;
        out << blockActionFailureText(report.failure) << " (slot=" << report.virtualSlot
            << " function_rva=0x" << std::hex << report.observedFunctionRva << std::dec
            << " called=" << report.called << " accepted=" << report.accepted << ')';
        return out.str();
    }
    bool stop(void* player, const BlockPos& block) const noexcept {
        cancelMiningOwnership();
        if (!world_) return false;
        void* mode = world_->gameMode(player);
        auto* function = resolve(mode, 2);
        if (!function) return false;
        NativeActionScope action;
        const bool called=callStop(function,mode,block);
        setDestroyingFlag(player, false);
        return called;
    }
    bool useItem(void* player) const noexcept {
        if (!world_) return false;
        void* mode = world_->gameMode(player);
        const int slot = world_->selected(player);
        auto* function = resolve(mode, 3);
        if (slot < 0 || slot > 8 || !function) return false;
        void* stack = world_->bindings_.stack(player, static_cast<unsigned>(slot));
        if (!stack) return false;
        __try {
            return reinterpret_cast<bool(__fastcall*)(void*, void*, unsigned char)>(function)(mode, stack, 0);
        } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
    bool releaseItem(void* player) const noexcept {
        if (!world_) return false;
        void* mode = world_->gameMode(player);
        auto* function = resolve(mode, 4);
        if (!function) return false;
        __try { reinterpret_cast<void(__fastcall*)(void*)>(function)(mode); return true; }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
    bool attackActor(void* player, void* target) const noexcept {
        if (!world_ || !target || target == player) return false;
        void* mode = world_->gameMode(player);
        auto* function = resolve(mode, 5);
        if (!function) return false;
        __try {
            // The exact-build thunk forwards the optional event context to
            // attack's dispatcher. No synthetic button or camera state.
            return reinterpret_cast<bool(__fastcall*)(void*, void*, void*)>(function)(mode, target, nullptr);
        } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
private:
    static bool callStop(void* function,void* mode,const BlockPos& block) noexcept {
        __try { reinterpret_cast<void(__fastcall*)(void*,const BlockPos*)>(function)(mode,&block);return true; }
        __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
    using StopFunction=void(__fastcall*)(void*,const BlockPos*);
    struct StopGuard {
        std::mutex mutex;
        StopFunction original{};
        BDS_HookToken token{};
        void* target{};
        std::atomic<const NativeBlockActions*> owner{};
        std::atomic<void*> mode{};
        std::atomic<ULONGLONG> until{};
        std::atomic<std::uint64_t> suppressed{};
    };
    // Process lifetime, like NativeBrightness: pin the detour's module and
    // leave an inactive forwarding trampoline when the adapter is destroyed.
    static StopGuard& stopGuard() { static auto* s=new StopGuard;return *s; }
    static inline thread_local unsigned nativeActionDepth_{};
    struct NativeActionScope {
        NativeActionScope() { ++nativeActionDepth_; }
        ~NativeActionScope() { --nativeActionDepth_; }
    };
    bool installStopGuard() const noexcept {
        auto& s=stopGuard();std::lock_guard lock(s.mutex);
        void* target=functions_[2].front();
        if (s.token) return s.target==target;
        if (!BDS_HookDirectInstall(target,reinterpret_cast<void*>(&stopDetour),
                reinterpret_cast<void**>(&s.original),&s.token)) return false;
        s.target=target;
        HMODULE module{};
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                          reinterpret_cast<LPCWSTR>(&stopDetour),&module);
        return true;
    }
    static void __fastcall stopDetour(void* mode,const BlockPos* block) noexcept {
        auto& s=stopGuard();
        // The idle mouse driver otherwise cancels the targeted session on
        // every frame with no physical left button held. Only defer that
        // external stop for this adapter's recently accepted bot session.
        // Native cancellations inside start/continue and explicit stop()
        // always reach the original. Real input clears ownership immediately.
        if (!nativeActionDepth_ && s.owner.load() && s.mode.load()==mode &&
            GetTickCount64()<s.until.load()) {
            ++s.suppressed;return;
        }
        if (s.original) s.original(mode,block);
    }
    void* resolve(void* mode, std::size_t index, BlockActionReport* report = nullptr) const noexcept {
        void* table{}; void* function{};
        if (report) report->virtualSlot = slots_[index];
        if (!world_->readField(mode, 0, table) ||
            !world_->readField(table, slots_[index] * sizeof(void*), function)) return nullptr;
        const auto address = reinterpret_cast<std::uintptr_t>(function);
        if (report && address >= base_ && address - base_ < imageSize_)
            report->observedFunctionRva = static_cast<std::uint32_t>(address - base_);
        // Dispatch through the actual checked vtable entry. Calling the base
        // implementation directly would bypass the alternate class's checks.
        return std::find(functions_[index].begin(), functions_[index].end(), function) != functions_[index].end()
            ? function : nullptr;
    }
    void setDestroyingFlag(void* player, bool on) const noexcept {
        if (!world_ || destroyingFlagOffset_ == kNoOffset || !player) return;
        const unsigned char value = on ? 1 : 0;
        (void)world_->writeField(player, destroyingFlagOffset_, value);
    }
    // Call the engine's common clock tick only when its counter did not
    // advance since our previous call (for example while chat owns input).
    // This samples real engine time; no fixed time step or progress is written.
    bool advanceClock(void* mode, bool begin) const noexcept {
        if (!clockTick_ || timerOffset_==kNoOffset) return false;
        void* timer{}; long long counter{};
        if (!world_->readField(mode,timerOffset_,timer) ||
            !world_->readField(timer,timerCounterOffset_,counter)) return false;
        if (begin || (mode==lastClockMode_ && counter==lastClockCounter_)) {
            if (!tickClock(clockTick_,mode)) return false;
            if (!world_->readField(timer,timerCounterOffset_,counter)) return false;
        }
        lastClockMode_=mode;lastClockCounter_=counter;return true;
    }
    static bool tickClock(void* function, void* mode) noexcept {
        __try { reinterpret_cast<void(__fastcall*)(void*)>(function)(mode);return true; }
        __except(EXCEPTION_EXECUTE_HANDLER){return false;}
    }
    static bool invoke(void* mode, void* function, const BlockPos& block, unsigned face,
                       Vec3 hitPos, bool begin, bool& destroyed, bool& accepted) noexcept {
        __try {
            if (begin)
                accepted = reinterpret_cast<bool(__fastcall*)(void*, const BlockPos*, int, bool*)>(function)(
                    mode, &block, static_cast<int>(face), &destroyed);
            else
                accepted = reinterpret_cast<bool(__fastcall*)(void*, const BlockPos*, unsigned char, const Vec3*, bool*)>(function)(
                    mode, &block, static_cast<unsigned char>(face), &hitPos, &destroyed);
            return true;
        } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
    static constexpr std::size_t kNoOffset = static_cast<std::size_t>(-1);
    NativeWorldAccess* world_{};
    std::array<std::vector<void*>, 6> functions_{};
    std::array<std::uint32_t, 6> slots_{};
    std::uintptr_t base_{};
    std::uint32_t imageSize_{};
    std::size_t destroyTargetOffset_{kNoOffset};
    std::size_t destroyProgressOffset_{kNoOffset};
    std::size_t destroyProgressMirrorOffset_{kNoOffset};
    std::size_t destroyingFlagOffset_{kNoOffset};
    std::size_t timerOffset_{kNoOffset};
    std::size_t timerCounterOffset_{0};
    void* clockTick_{};
    mutable void* lastClockMode_{};
    mutable long long lastClockCounter_{};
};
} // namespace bedrock
