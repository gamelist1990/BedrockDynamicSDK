#pragma once
#include <bedrock/detail/native_profile_functions.hpp>
#include <bedrock/profiles/profile_json.hpp>
#include <bedrock/runtime/hook_runtime.h>
#include <intrin.h>
#include <array>
#include <algorithm>
#include <atomic>

namespace bedrock {
// Native ABI, callable identity and caller pairing stay inside the SDK.
// The Mod supplies only a thread-safe policy callback. Hooks remain pinned and
// pass through when that policy is false; no render object is written.
class NativeInsideBlockEffect {
public:
    using Policy = bool(*)() noexcept;
    struct Statistics {unsigned calls{}, eligible{}, skipped{}, otherCaller{};std::uintptr_t callerRva{};};
    bool install(const RuntimeProfile& profile, std::string_view embedded, Policy policy, std::string& error) {
        try {
            auto& s = state();
            if (s.token) throw std::runtime_error("InsideBlockEffect already installed in this module");
            if (!policy) throw std::runtime_error("InsideBlockEffect policy is missing");
            auto root = parseNativeProfile(profile.json());
            const auto needsSupplement=[&]{
                const auto& camera=root.at("native_camera").at("visual");
                if(!camera.contains("render_dragon_inside_effect"))return true;
                const auto& effect=camera.at("render_dragon_inside_effect");
                return !effect.contains("predicate_vtable_slot")||!effect.contains("type_getter_vtable_slot")||effect.at("variants").empty();
            };
            if(needsSupplement()){
                const auto fallback=parseNativeProfile(embedded);
                for(const auto* field:{"sha256","text_sha256","size_of_image","text_rva","text_size"})
                    if(root.at("target").at(field)!=fallback.at("target").at(field))
                        throw std::runtime_error("InsideBlockEffect embedded definition belongs to another executable");
                root=fallback;
            }
            const auto& effect = root.at("native_camera").at("visual").at("render_dragon_inside_effect");
            if (effect.at("schema") != 1 || !effect.at("variants").is_array() ||
                    effect.at("variants").empty() || effect.at("variants").size() > s.variants.size())
                throw std::runtime_error("unsupported InsideBlockEffect variants/schema");
            const auto predicateSlot = native_profile::number(effect.at("predicate_vtable_slot"));
            const auto typeSlot = native_profile::number(effect.at("type_getter_vtable_slot"));
            if (predicateSlot > 64 || typeSlot > 64 || predicateSlot == typeSlot)
                throw std::runtime_error("invalid InsideBlockEffect callable slots");
            auto* target = native_profile::checked(root, effect.at("predicate"));
            const auto base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
            const auto extent = native_profile::number(root.at("target").at("size_of_image"));
            std::size_t count{};
            for (const auto& variant : effect.at("variants")) {
                const auto table = native_profile::number(variant.at("callable_vtable_rva"));
                const auto caller = native_profile::number(variant.at("return_rva"));
                if (table >= extent || std::uint64_t(table) + (std::max(predicateSlot,typeSlot)+1)*8 > extent || caller >= extent)
                    throw std::runtime_error("InsideBlockEffect identity outside module");
                native_profile::checked(root, variant.at("caller_check"));
                auto* typeGetter = native_profile::checked(root, variant.at("type_getter_check"));
                void *call{}, *type{};
                if (!native_profile::read(reinterpret_cast<void*>(base+table),predicateSlot*8,call) || call != target ||
                        !native_profile::read(reinterpret_cast<void*>(base+table),typeSlot*8,type) || type != typeGetter)
                    throw std::runtime_error("InsideBlockEffect callable identity mismatch");
                s.variants[count++] = {base+table,base+caller};
            }
            s.count=count;s.base=base;s.extent=extent;s.policy=policy;
            if (!BDS_HookDirectInstall(target,reinterpret_cast<void*>(&detour),reinterpret_cast<void**>(&s.original),&s.token))
                throw std::runtime_error("InsideBlockEffect hook conflict");
            HMODULE pinned{};
            GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_PIN,
                reinterpret_cast<LPCWSTR>(&detour),&pinned);
            ready_=true;error.clear();return true;
        } catch (const std::exception& e) {error=e.what();return false;}
    }
    bool ready() const noexcept {return ready_;}
    Statistics takeStatistics() noexcept {
        auto& s=state();return {s.calls.exchange(0),s.eligible.exchange(0),s.skipped.exchange(0),s.otherCaller.exchange(0),s.callerRva.load()};
    }
private:
    using Predicate=bool(__fastcall*)(void*,void*);
    struct Variant {std::uintptr_t table{},caller{};};
    struct State {
        std::array<Variant,16> variants{};std::size_t count{};
        std::uintptr_t base{},extent{};Policy policy{};Predicate original{};BDS_HookToken token{};
        std::atomic<unsigned> calls{},eligible{},skipped{},otherCaller{};std::atomic<std::uintptr_t> callerRva{};
    };
    static State& state() {static State* s=new State;return *s;}
    static bool __fastcall detour(void* predicate,void* arguments) {
        const auto caller=reinterpret_cast<std::uintptr_t>(_ReturnAddress());
        auto& s=state();std::uintptr_t table{};
        if (!native_profile::read(predicate,0,table))return s.original(predicate,arguments);
        bool tagged=false,paired=false;
        for (std::size_t i=0;i<s.count;++i)if (table==s.variants[i].table){tagged=true;paired|=caller==s.variants[i].caller;}
        if (!tagged)return s.original(predicate,arguments);
        const bool permitted=s.original(predicate,arguments);s.calls.fetch_add(1);
        if (caller>=s.base && caller-s.base<s.extent)s.callerRva.store(caller-s.base);
        if (!paired){s.otherCaller.fetch_add(1);return permitted;}
        if (permitted)s.eligible.fetch_add(1);
        if (permitted && s.policy()){s.skipped.fetch_add(1);return false;}
        return permitted;
    }
    bool ready_{};
};
} // namespace bedrock
