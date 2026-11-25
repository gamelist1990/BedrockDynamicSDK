#pragma once
#include <bedrock/detail/native_profile_functions.hpp>
#include <bedrock/profiles/profile_json.hpp>
#include <bedrock/runtime/hook_runtime.h>
#include <intrin.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <stdexcept>
#include <vector>

namespace bedrock {
// RenderDragon scene-actor extraction filter.
//
// extractAndDrawWithExclusion<Pass, Component> asks a per-object predicate
// (_Do_call of a std::function-shaped callable) whether an object may be added
// to the pass. The predicate body is shared, the callable table is per
// (pass, component) instantiation, so the table read from the callable object
// identifies the pass exactly.
//
// This adapter detours that shared body, keeps the original answer for every
// object it was not asked about, and lets the Mod's policy exclude a specific
// object. It never writes game memory and never retains a game pointer: the
// policy is a pure function of the object pointer for the duration of the call.
class NativeSceneActorExclusion {
  public:
    // Called on the render thread for each object of a watched pass whose
    // original answer was "permit". Return true to exclude the object.
    using Policy = bool (*)(void *object) noexcept;
    struct Statistics {
        unsigned calls{}, permitted{}, excluded{};
        unsigned long long tableMask{};  // bit i = variant i observed
    };
    static constexpr std::size_t kSampleWords = 32;
    static constexpr std::size_t kContextWords = 8;
    struct Sample {
        std::uintptr_t table{}, object{}, context{};
        std::uintptr_t words[kSampleWords]{};
        std::uintptr_t contextWords[kContextWords]{};
        unsigned calls{};
    };
    static constexpr std::size_t kMaxVariants = 8;
    static constexpr std::size_t kProbeSlots = 64;

    bool install(const RuntimeProfile &profile, std::string_view embedded, Policy policy,
                 bool probe, std::string &error) {
        try {
            auto &s = state();
            if (s.token)
                throw std::runtime_error("scene-actor exclusion already installed in this module");
            if (!policy)
                throw std::runtime_error("scene-actor exclusion policy is missing");
            // The embedded definition is the same executable; it is only used
            // when the selected profile predates this feature.
            auto root = parseNativeProfile(profile.json());
            const auto needsSupplement = [&] {
                if (!root.contains("native_camera")) return true;
                const auto &camera = root.at("native_camera");
                if (!camera.contains("visual")) return true;
                const auto &visual = camera.at("visual");
                return !visual.contains("render_dragon_scene_actor_exclusion") ||
                       !visual.at("render_dragon_scene_actor_exclusion").contains("variants") ||
                       visual.at("render_dragon_scene_actor_exclusion").at("variants").empty();
            };
            if (needsSupplement()) {
                const auto fallback = parseNativeProfile(embedded);
                for (const auto *field :
                     {"sha256", "text_sha256", "size_of_image", "text_rva", "text_size"})
                    if (root.at("target").at(field) != fallback.at("target").at(field))
                        throw std::runtime_error(
                            "scene-actor exclusion embedded definition belongs to another "
                            "executable");
                root = fallback;
            }
            const auto &exclusion = root.at("native_camera")
                                        .at("visual")
                                        .at("render_dragon_scene_actor_exclusion");
            // The extraction predicate receives RenderDragon frame objects, not
            // Actors. Exclusion may only be armed when the profile records a
            // proven object->Actor identity; otherwise the probe (diagnostic,
            // no exclusion) is the only mode that may run.
            const auto identity = exclusion.value("object_identity", std::string{"unresolved"});
            if (identity != "actor_pointer" && !probe)
                throw std::runtime_error(
                    "scene-actor exclusion object identity is " + identity +
                    "; the extraction predicate does not receive Actor pointers (probe only)");
            if (exclusion.at("schema") != 1 || !exclusion.at("variants").is_array() ||
                exclusion.at("variants").empty() ||
                exclusion.at("variants").size() > s.variants.size())
                throw std::runtime_error("unsupported scene-actor exclusion variants/schema");
            const auto predicateSlot = native_profile::number(exclusion.at("predicate_vtable_slot"));
            const auto typeSlot = native_profile::number(exclusion.at("type_getter_vtable_slot"));
            if (predicateSlot > 32 || typeSlot > 32 || predicateSlot == typeSlot)
                throw std::runtime_error("invalid scene-actor exclusion callable slots");
            auto *target = native_profile::checked(root, exclusion.at("predicate"));
            const auto base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
            const auto extent = native_profile::number(root.at("target").at("size_of_image"));
            std::size_t count{};
            for (const auto &variant : exclusion.at("variants")) {
                const auto table = native_profile::number(variant.at("callable_vtable_rva"));
                const auto descriptor = native_profile::number(variant.at("type_descriptor_rva"));
                if (table >= extent ||
                    std::uint64_t(table) + (std::max(predicateSlot, typeSlot) + 1) * 8 > extent)
                    throw std::runtime_error("scene-actor exclusion table outside module");
                auto *typeGetter = native_profile::checked(root, variant.at("type_getter_check"));
                void *call{}, *type{};
                if (!native_profile::read(reinterpret_cast<void *>(base + table),
                                          predicateSlot * 8, call) ||
                    call != target ||
                    !native_profile::read(reinterpret_cast<void *>(base + table), typeSlot * 8,
                                          type) ||
                    type != typeGetter)
                    throw std::runtime_error("scene-actor exclusion callable identity mismatch");
                // The type getter must return the descriptor that names this
                // (pass, component) instantiation: lea rax,[rip+rel]; ret.
                std::uint8_t bytes[8]{};
                if (!native_profile::read(typeGetter, 0, bytes) || bytes[0] != 0x48 ||
                    bytes[1] != 0x8D || bytes[2] != 0x05 || bytes[7] != 0xC3)
                    throw std::runtime_error("scene-actor exclusion type getter is not a descriptor "
                                             "getter");
                std::int32_t relative{};
                std::memcpy(&relative, bytes + 3, sizeof relative);
                const auto getterRva =
                    static_cast<std::uintptr_t>(reinterpret_cast<std::uintptr_t>(typeGetter) - base);
                if (getterRva + 7 + relative != descriptor)
                    throw std::runtime_error("scene-actor exclusion descriptor mismatch");
                s.variants[count++] = {base + table, descriptor};
            }
            s.count = count;
            s.base = base;
            s.extent = extent;
            s.policy = policy;
            s.probe.store(probe);
            if (!BDS_HookDirectInstall(target, reinterpret_cast<void *>(&detour),
                                       reinterpret_cast<void **>(&s.original), &s.token))
                throw std::runtime_error("scene-actor exclusion hook conflict");
            HMODULE pinned{};
            GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                   GET_MODULE_HANDLE_EX_FLAG_PIN,
                               reinterpret_cast<LPCWSTR>(&detour), &pinned);
            ready_ = true;
            error.clear();
            return true;
        } catch (const std::exception &e) {
            error = e.what();
            return false;
        }
    }
    bool ready() const noexcept {
        return ready_;
    }
    void setProbe(bool enabled) noexcept {
        state().probe.store(enabled);
    }
    bool probeEnabled() const noexcept {
        return state().probe.load();
    }
    Statistics takeStatistics() noexcept {
        auto &s = state();
        return {s.calls.exchange(0), s.permitted.exchange(0), s.excluded.exchange(0),
                s.tableMask.exchange(0)};
    }
    // Drains the diagnostic ring. Diagnostic use only: the render thread keeps
    // writing while this copies, so a sample may be lost, never misinterpreted.
    std::vector<Sample> takeSamples() {
        auto &s = state();
        std::vector<Sample> out;
        out.reserve(kProbeSlots);
        for (std::size_t i = 0; i < kProbeSlots; ++i) {
            const auto object = s.sampleObject[i].exchange(0);
            if (!object) continue;
            Sample sample{};
            sample.table = s.sampleTable[i].exchange(0);
            sample.object = object;
            sample.context = s.sampleContext[i].exchange(0);
            for (std::size_t word = 0; word < kSampleWords; ++word)
                sample.words[word] = s.sampleWords[i][word].exchange(0);
            for (std::size_t word = 0; word < kContextWords; ++word)
                sample.contextWords[word] = s.contextWords[i][word].exchange(0);
            sample.calls = s.sampleCalls[i].exchange(0);
            out.push_back(sample);
        }
        return out;
    }
    void clearSamples() noexcept {
        auto &s = state();
        for (std::size_t i = 0; i < kProbeSlots; ++i) {
            s.sampleObject[i].store(0);
            s.sampleCalls[i].store(0);
        }
    }

  private:
    using Predicate = bool(__fastcall *)(void *, void *);
    struct Variant {
        std::uintptr_t table{}, descriptor{};
    };
    struct State {
        std::array<Variant, kMaxVariants> variants{};
        std::size_t count{};
        std::uintptr_t base{}, extent{};
        Policy policy{};
        Predicate original{};
        BDS_HookToken token{};
        std::atomic<bool> probe{false};
        std::atomic<unsigned> calls{}, permitted{}, excluded{};
        std::atomic<unsigned long long> tableMask{};
        std::array<std::atomic<std::uintptr_t>, kProbeSlots> sampleObject{};
        std::array<std::atomic<std::uintptr_t>, kProbeSlots> sampleTable{};
        std::array<std::atomic<std::uintptr_t>, kProbeSlots> sampleContext{};
        std::array<std::array<std::atomic<std::uintptr_t>, kSampleWords>, kProbeSlots>
            sampleWords{};
        std::array<std::array<std::atomic<std::uintptr_t>, kContextWords>, kProbeSlots>
            contextWords{};
        std::array<std::atomic<unsigned>, kProbeSlots> sampleCalls{};
    };
    static State &state() {
        static State *s = new State;
        return *s;
    }
    static void record(std::uintptr_t table, void *pair) noexcept {
        auto &s = state();
        if (!pair) return;
        std::uintptr_t object{}, context{};
        if (!native_profile::read(pair, 0, object) ||
            !native_profile::read(pair, 8, context) || !object)
            return;
        for (std::size_t i = 0; i < kProbeSlots; ++i) {
            if (s.sampleObject[i].load(std::memory_order_relaxed) == object) {
                s.sampleCalls[i].fetch_add(1, std::memory_order_relaxed);
                return;
            }
        }
        for (std::size_t i = 0; i < kProbeSlots; ++i) {
            std::uintptr_t expected{};
            if (!s.sampleObject[i].compare_exchange_strong(expected, object,
                                                           std::memory_order_release))
                continue;
            s.sampleTable[i].store(table, std::memory_order_relaxed);
            s.sampleContext[i].store(context, std::memory_order_relaxed);
            for (std::size_t word = 0; word < kSampleWords; ++word) {
                std::uintptr_t value{};
                native_profile::read(reinterpret_cast<void *>(object), word * 8, value);
                s.sampleWords[i][word].store(value, std::memory_order_relaxed);
            }
            for (std::size_t word = 0; word < kContextWords; ++word) {
                std::uintptr_t value{};
                native_profile::read(reinterpret_cast<void *>(context), word * 8, value);
                s.contextWords[i][word].store(value, std::memory_order_relaxed);
            }
            s.sampleCalls[i].store(1, std::memory_order_relaxed);
            return;
        }
    }
    static bool __fastcall detour(void *callable, void *pair) {
        auto &s = state();
        // The original answer is always computed first and is what every
        // unwatched object keeps, including when our policy declines.
        const bool permitted = s.original(callable, pair);
        std::uintptr_t table{};
        if (!native_profile::read(callable, 0, table)) return permitted;
        std::size_t index = s.count;
        for (std::size_t i = 0; i < s.count; ++i)
            if (table == s.variants[i].table) {
                index = i;
                break;
            }
        if (index == s.count) return permitted;
        s.calls.fetch_add(1, std::memory_order_relaxed);
        s.tableMask.fetch_or(1ull << index, std::memory_order_relaxed);
        if (s.probe.load(std::memory_order_relaxed)) {
            record(table, pair);
            return permitted;
        }
        if (!permitted) return false;
        std::uintptr_t object{};
        if (!pair || !native_profile::read(pair, 0, object) || !object) return true;
        if (s.policy(reinterpret_cast<void *>(object))) {
            s.excluded.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        s.permitted.fetch_add(1, std::memory_order_relaxed);
        return true;
    }
    bool ready_{};
};
} // namespace bedrock
