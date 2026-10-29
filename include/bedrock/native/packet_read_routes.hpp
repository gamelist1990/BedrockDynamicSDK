#pragma once
#include <bedrock/analysis/memory_inspector.hpp>
#include <bedrock/runtime/hook_runtime.h>
#include <map>
#include <algorithm>
#include <set>

namespace bedrock {
// Discover actual per-packet decode functions through Packet vtables, as
// packet dispatcher clients do. No packet factory or candidate is executed.
// Identity requires getID's constant leaf AND the common Packet read anchor.
class NativePacketReadRoutes {
  public:
    struct Route {
        unsigned id{};
        std::uintptr_t vtable{};
        std::vector<void *> targets;
    };
    void load(const nlohmann::json &root, void *commonRead) {
        routes_.clear();
        base_ = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
        begin_ = base_ + native_profile::number(root.at("target").at("text_rva"));
        end_ = begin_ + native_profile::number(root.at("target").at("text_size"));
        const auto &packet = root.at("layouts").at("Packet");
        const auto &read = root.at("layouts").at("PacketRead");
        const auto idSlot = native_profile::number(packet.at("get_id_slot"));
        const auto anchorSlot = native_profile::number(read.at("vtable_anchor_slot"));
        std::vector<unsigned> slots;
        for (const auto &value : read.at("decode_slots"))
            slots.push_back(native_profile::number(value));
        if (idSlot > 64 || anchorSlot > 64 || slots.empty() || slots.size() > 4 ||
            std::any_of(slots.begin(), slots.end(), [](unsigned v) { return v > 64; }))
            throw std::runtime_error("invalid Packet decode vtable metadata");
        const auto sections = loadedMainSections();
        const auto rdata = std::find_if(sections.begin(), sections.end(),
                                        [](const auto &s) { return s.name == ".rdata"; });
        if (rdata == sections.end() || rdata->size > 256 * 1024 * 1024)
            throw std::runtime_error("Packet rdata section unavailable");
        std::array<std::byte, 65536> block{};
        std::map<unsigned, std::vector<Route>> candidates;
        for (std::size_t offset = 0; offset < rdata->size; offset += block.size()) {
            const auto size = std::min(block.size(), rdata->size - offset);
            if (!MemoryInspector::copy(rdata->address + offset, std::span(block).first(size)))
                continue;
            for (std::size_t at = 0; at + 8 <= size; at += 8) {
                std::uintptr_t anchor{};
                std::memcpy(&anchor, block.data() + at, 8);
                if (anchor != reinterpret_cast<std::uintptr_t>(commonRead) ||
                    offset + at < anchorSlot * 8)
                    continue;
                const auto table = rdata->address + offset + at - anchorSlot * 8;
                const auto maxSlot = std::max(
                    {idSlot, anchorSlot,
                     static_cast<std::uint32_t>(*std::max_element(slots.begin(), slots.end()))});
                if (table < rdata->address ||
                    table > rdata->address + rdata->size - (maxSlot + 1) * 8)
                    continue;
                std::uintptr_t idFunction{};
                if (!native_profile::read(reinterpret_cast<void *>(table), idSlot * 8,
                                          idFunction) ||
                    idFunction < begin_ || idFunction + 6 > end_)
                    continue;
                std::array<unsigned char, 6> code{};
                if (!native_profile::read(reinterpret_cast<void *>(idFunction), 0, code) ||
                    code[0] != 0xb8 || code[5] != 0xc3)
                    continue;
                std::uint32_t id{};
                std::memcpy(&id, code.data() + 1, 4);
                if (id > 0x3ff)
                    continue;
                Route route{id, table, {}};
                for (const auto slot : slots) {
                    void *target{};
                    if (!native_profile::read(reinterpret_cast<void *>(table), slot * 8, target) ||
                        !functionEntry(target)) {
                        route.targets.clear();
                        break;
                    }
                    if (std::find(route.targets.begin(), route.targets.end(), target) ==
                        route.targets.end())
                        route.targets.push_back(target);
                }
                if (!route.targets.empty())
                    candidates[id].push_back(std::move(route));
            }
        }
        for (auto &[id, values] : candidates) {
            // Multiple identical implementations are harmless; differing
            // decoder sets remain ambiguous and cannot become callable hooks.
            if (std::all_of(values.begin(), values.end(),
                            [&](const Route &r) { return r.targets == values.front().targets; }))
                routes_[id] = std::move(values.front());
        }
    }
    const std::map<unsigned, Route> &routes() const noexcept {
        return routes_;
    }

  private:
    bool functionEntry(void *target) const noexcept {
        const auto address = reinterpret_cast<std::uintptr_t>(target);
        if (address < begin_ || address >= end_)
            return false;
        DWORD64 image{};
        const auto *entry = RtlLookupFunctionEntry(address, &image, nullptr);
        return entry && image + entry->BeginAddress == address;
    }
    std::uintptr_t base_{}, begin_{}, end_{};
    std::map<unsigned, Route> routes_;
};
} // namespace bedrock
