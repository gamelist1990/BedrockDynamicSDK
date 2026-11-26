#pragma once

#include <bedrock/detail/native_profile_functions.hpp>
#include <bedrock/runtime/hook_runtime.h>
#include <algorithm>
#include <atomic>
#include <cstring>
#include <stdexcept>

namespace bedrock {
class NativeQueueRenderEntity {
  public:
    using Policy = bool (*)(void *) noexcept;

    bool install(const nlohmann::json &profile, Policy policy, bool probe) {
        if (!profile.contains("native_camera") || !profile.at("native_camera").contains("visual"))
            return false;
        const auto &visual = profile.at("native_camera").at("visual");
        if (!visual.contains("queue_render_entity")) return false;
        const auto &spec = visual.at("queue_render_entity");
        auto &s = state();
        auto *target = native_profile::checked(profile, spec.at("target"));
        const auto base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
        const auto extent = native_profile::number(profile.at("target").at("size_of_image"));
        const auto table = native_profile::number(spec.at("table_rva"));
        const auto slot = native_profile::number(spec.at("slot"));
        const auto getterSlot = native_profile::number(spec.at("getter_slot"));
        const auto descriptor = native_profile::number(spec.at("descriptor_rva"));
        void *entry{}, *getter{};
        auto *expectedGetter = native_profile::checked(profile, spec.at("getter"));
        if (s.token || slot > 16 || getterSlot > 16 || slot == getterSlot ||
            std::uint64_t(table) + (std::max(slot, getterSlot) + 1) * 8 > extent ||
            !native_profile::read(reinterpret_cast<void *>(base + table), slot * 8, entry) ||
            !native_profile::read(reinterpret_cast<void *>(base + table), getterSlot * 8, getter) ||
            entry != target || getter != expectedGetter)
            throw std::runtime_error("per-actor render callable identity mismatch");
        std::uint8_t bytes[8]{};
        std::int32_t displacement{};
        if (!native_profile::read(getter, 0, bytes) || bytes[0] != 0x48 || bytes[1] != 0x8D ||
            bytes[2] != 0x05 || bytes[7] != 0xC3)
            throw std::runtime_error("per-actor render type getter mismatch");
        std::memcpy(&displacement, bytes + 3, sizeof displacement);
        if (reinterpret_cast<std::uintptr_t>(getter) + 7 + displacement != base + descriptor)
            throw std::runtime_error("per-actor render descriptor mismatch");
        s.table = base + table;
        s.policy = policy;
        s.probe.store(probe, std::memory_order_relaxed);
        if (!BDS_HookDirectInstall(target, reinterpret_cast<void *>(&detour),
                                   reinterpret_cast<void **>(&s.original), &s.token))
            throw std::runtime_error("per-actor render hook conflict");
        HMODULE pinned{};
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                               GET_MODULE_HANDLE_EX_FLAG_PIN,
                           reinterpret_cast<LPCWSTR>(&detour), &pinned);
        ready_ = true;
        return true;
    }

    bool ready() const noexcept { return ready_; }
    void setProbe(bool probe) noexcept { state().probe.store(probe); }
    unsigned takeCalls() noexcept { return state().calls.exchange(0); }
    unsigned takeMatches() noexcept { return state().matches.exchange(0); }
    unsigned takeExcluded() noexcept { return state().excluded.exchange(0); }

  private:
    using Call = void(__fastcall *)(void *, void *);
    struct State {
        std::uintptr_t table{};
        Policy policy{};
        Call original{};
        BDS_HookToken token{};
        std::atomic<bool> probe{true};
        std::atomic<unsigned> calls{}, matches{}, excluded{};
    };
    static State &state() {
        static State *s = new State;
        return *s;
    }
    static void __fastcall detour(void *closure, void *actor) noexcept {
        auto &s = state();
        std::uintptr_t table{};
        if (actor && native_profile::read(closure, 0, table) && table == s.table) {
            s.calls.fetch_add(1, std::memory_order_relaxed);
            if (s.policy && s.policy(actor)) {
                s.matches.fetch_add(1, std::memory_order_relaxed);
                if (!s.probe.load(std::memory_order_relaxed)) {
                    s.excluded.fetch_add(1, std::memory_order_relaxed);
                    return; // void callback; the candidate iterator advances in the caller
                }
            }
        }
        s.original(closure, actor);
    }
    bool ready_{};
};
} // namespace bedrock
