#pragma once
#include <bedrock/profile_json.hpp>
#include <bedrock/brightness.hpp>
#include <bedrock/detail/native_profile_functions.hpp>
#include <bedrock/hook_runtime.h>
#include <algorithm>
#include <atomic>
#include <map>
#include <mutex>

namespace bedrock {
// Each object owns one brightness request. Requests in this SDK module combine
// by maximum gamma. reset()/destruction removes only this owner's request.
// No Options field is overwritten: the original getter remains authoritative
// whenever all overrides are disabled. Hook/trampoline lifetime is process-wide.
class NativeBrightness {
public:
    NativeBrightness() = default;
    NativeBrightness(const NativeBrightness&) = delete;
    NativeBrightness& operator=(const NativeBrightness&) = delete;
    ~NativeBrightness() { reset(); }
    bool load(const RuntimeProfile& profile, std::string& error) {
        if (!profile.supports({Feature::Brightness}, error)) return false;
        try {
            auto& state = shared();
            std::lock_guard lock(state.mutex);
            const auto root = bedrock::parseNativeProfile(profile.json());
            const auto& gamma = root.at("native_camera").at("visual").at("gamma");
            auto* target = reinterpret_cast<void*>(reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr)) + native_profile::number(gamma.at("rva")));
            if (state.token && state.target != target)
                throw std::runtime_error("brightness target changed while hook is installed");
            if (!state.token) target = native_profile::checked(root, gamma);
            target_ = target; error.clear(); return true;
        } catch (const std::exception& e) { error = e.what(); return false; }
    }
    // Install after ProfileManager::prepare has validated every adapter.
    bool install(std::string& error) {
        auto& state = shared(); std::lock_guard lock(state.mutex);
        if (!target_) { error = "brightness profile is not loaded"; return false; }
        if (state.token && state.target != target_) { error = "brightness target conflict"; return false; }
        if (!state.token) {
            if (!BDS_HookDirectInstall(target_, reinterpret_cast<void*>(&detour),
                    reinterpret_cast<void**>(&state.original), &state.token)) {
                error = "brightness hook conflict"; return false;
            }
            state.target = target_;
            HMODULE module{};
            GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                              reinterpret_cast<LPCWSTR>(&detour), &module);
        }
        ready_.store(true); error.clear(); return true;
    }

    bool setGamma(Gamma gamma) {
        if (!ready_.load()) return false;
        auto& state = shared(); std::lock_guard lock(state.mutex);
        state.requests[this] = gamma.value(); update(state); return true;
    }
    bool fullBright() { return setGamma(Gamma::fullBright()); }
    void reset() {
        if (!ready_.load()) return;
        auto& state = shared(); std::lock_guard lock(state.mutex);
        state.requests.erase(this); update(state);
    }
    bool ready() const noexcept { return ready_.load(); }
private:
    using Getter = float(__fastcall*)(void*);
    struct State {
        std::mutex mutex;
        std::map<const NativeBrightness*, float> requests;
        std::atomic<float> gamma{-1.f};
        Getter original{}; BDS_HookToken token{}; void* target{};
    };
    static State& shared() { static State* state = new State; return *state; }
    static void update(State& state) {
        float result = -1.f;
        for (const auto& [owner, gamma] : state.requests) result = std::max(result, gamma);
        state.gamma.store(result, std::memory_order_release);
    }
    static float __fastcall detour(void* options) {
        auto& state = shared();
        const float native = state.original(options);
        const float gamma = state.gamma.load(std::memory_order_acquire);
        return gamma < 0.f ? native : gamma;
    }
    void* target_{};
    std::atomic<bool> ready_{};
};
} // namespace bedrock
