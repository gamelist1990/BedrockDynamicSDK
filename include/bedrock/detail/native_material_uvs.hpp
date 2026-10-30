#pragma once
#include <bedrock/detail/material_uv_patch.hpp>
#include <bedrock/detail/native_profile_functions.hpp>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace bedrock::detail {
// Exact-profile adapter: borrowed objects are used only in a drained main
// frame. Nothing is retained across frames and no game ownership is changed.
class NativeMaterialUvs {
    using Getter = void *(__fastcall *)(const void *);
    using Lookup = void *(__fastcall *)(void *, const std::string *);
    Getter components_{}, baked_{};
    Lookup lookup_{};
    void *registry_{};
    struct Layout {
        std::size_t atlas_manager_offset{};
        std::size_t baked_materials_offset{};
        std::size_t block_flags_offset{};
        std::size_t block_ready_mask{};
        std::size_t component_head_offset{};
        std::size_t item_name_offset{};
        std::size_t item_textures_offset{};
        std::size_t material_uvs_offset{};
        std::size_t maximum_blocks{};
        std::size_t maximum_material_links{};
        std::size_t maximum_materials{};
        std::size_t maximum_materials_per_block{};
        std::size_t maximum_name_bytes{};
        std::size_t maximum_types{};
        std::size_t maximum_uv_sets{};
        std::size_t maximum_variants{};
        std::size_t maximum_vector_capacity{};
        std::size_t node_material_offset{};
        std::size_t node_render_layer_offset{};
        std::size_t node_texture_offset{};
        std::size_t registry_count_offset{};
        std::size_t registry_displacement_offset{};
        std::size_t registry_instruction_bytes{};
        std::size_t registry_rva{};
        std::size_t shared_pointer_bytes{};
        std::size_t source_coordinates_offset{};
        std::size_t source_pbr_offset{};
        std::size_t source_stride{};
        std::size_t source_weight_offset{};
        std::size_t string_capacity_offset{};
        std::size_t string_inline_bytes{};
        std::size_t string_size_offset{};
        std::size_t target_coordinates_offset{};
        std::size_t target_pbr_offset{};
        std::size_t target_stride{};
        std::size_t target_weight_offset{};
        std::size_t tree_nil_offset{};
        std::size_t tree_parent_offset{};
        std::size_t tree_right_offset{};
        std::size_t tree_value_offset{};
        std::size_t type_permutations_offset{};
        std::size_t unsupported_layer_first{};
        std::size_t unsupported_layer_last{};
        std::size_t write_bytes{};
    } offsets_;
    MaterialUvLayout uv_{};
    template<class T> static bool read(const void *p, std::size_t o, T &v) {
        return native_profile::read(p, o, v);
    }
    static bool bytes(const void *p, void *out, std::size_t count) noexcept {
        SIZE_T copied{};
        return p && ReadProcessMemory(GetCurrentProcess(), p, out, count, &copied) && copied == count;
    }
    static bool write(void *p, const void *from, std::size_t count) noexcept {
        __try { std::memcpy(p, from, count); return true; }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
    static bool writable(const void *p, std::size_t count) noexcept {
        const auto start = reinterpret_cast<std::uintptr_t>(p);
        if (!start || count > UINTPTR_MAX - start) return false;
        auto cursor = start;
        while (cursor < start + count) {
            MEMORY_BASIC_INFORMATION info{};
            if (!VirtualQuery(reinterpret_cast<void *>(cursor), &info, sizeof(info)) ||
                info.State != MEM_COMMIT || (info.Protect & (PAGE_GUARD | PAGE_NOACCESS)) ||
                !(info.Protect & (PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)))
                return false;
            auto end = reinterpret_cast<std::uintptr_t>(info.BaseAddress) + info.RegionSize;
            if (end <= cursor) return false;
            cursor = end;
        }
        return true;
    }
    struct Vector {
        std::uintptr_t begin{}, end{}, capacity{};
        bool operator==(const Vector &) const = default;
    };
    bool vector(const void *p, std::size_t offset, std::size_t stride,
                std::size_t limit, Vector &v) const {
        return read(p, offset, v) && v.begin <= v.end && v.end <= v.capacity &&
            (!v.end || v.begin) && (v.end - v.begin) % stride == 0 &&
            (v.capacity - v.begin) % stride == 0 && (v.end - v.begin) / stride <= limit &&
            (v.capacity - v.begin) / stride <= offsets_.maximum_vector_capacity;
    }
    static void *get(Getter fn, const void *p) noexcept {
        __try { return fn(p); } __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
    }
    static void *lookup(Lookup fn, void *atlas, const std::string *name) noexcept {
        __try { return fn(atlas, name); } __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
    }
    bool name(const void *node, std::size_t offset, std::string &out) const {
        std::size_t size{}, capacity{};
        if (!read(node, offset + offsets_.string_size_offset, size) ||
            !read(node, offset + offsets_.string_capacity_offset, capacity) ||
            !size || size > offsets_.maximum_name_bytes || size > capacity) return false;
        const void *text = static_cast<const std::byte *>(node) + offset;
        if (capacity >= offsets_.string_inline_bytes && !read(node, offset, text)) return false;
        out.resize(size);
        return bytes(text, out.data(), size) && out.find('\0') == std::string::npos;
    }
    struct Patch {
        void *material{};
        Vector target{}, source{};
        const void *sourceVector{};
        std::vector<std::byte> before, after, sourceBytes;
    };
    std::vector<Patch> patches_;
  public:
    struct Counts { std::uint64_t materials{}, rows{}, mismatched{}, unmatched{}, unchanged{}; } counts;
    void load(const nlohmann::json &root, const nlohmann::json &layout) {
        const auto &config = layout.at("custom_material_uvs");
        // Parse once; traversal does not query JSON or interpret metadata as offsets.
        offsets_.atlas_manager_offset = native_profile::number(config.at("atlas_manager_offset"));
        offsets_.baked_materials_offset = native_profile::number(config.at("baked_materials_offset"));
        offsets_.block_flags_offset = native_profile::number(config.at("block_flags_offset"));
        offsets_.block_ready_mask = native_profile::number(config.at("block_ready_mask"));
        offsets_.component_head_offset = native_profile::number(config.at("component_head_offset"));
        offsets_.item_name_offset = native_profile::number(config.at("item_name_offset"));
        offsets_.item_textures_offset = native_profile::number(config.at("item_textures_offset"));
        offsets_.material_uvs_offset = native_profile::number(config.at("material_uvs_offset"));
        offsets_.maximum_blocks = native_profile::number(config.at("maximum_blocks"));
        offsets_.maximum_material_links = native_profile::number(config.at("maximum_material_links"));
        offsets_.maximum_materials = native_profile::number(config.at("maximum_materials"));
        offsets_.maximum_materials_per_block = native_profile::number(config.at("maximum_materials_per_block"));
        offsets_.maximum_name_bytes = native_profile::number(config.at("maximum_name_bytes"));
        offsets_.maximum_types = native_profile::number(config.at("maximum_types"));
        offsets_.maximum_uv_sets = native_profile::number(config.at("maximum_uv_sets"));
        offsets_.maximum_variants = native_profile::number(config.at("maximum_variants"));
        offsets_.maximum_vector_capacity = native_profile::number(config.at("maximum_vector_capacity"));
        offsets_.node_material_offset = native_profile::number(config.at("node_material_offset"));
        offsets_.node_render_layer_offset = native_profile::number(config.at("node_render_layer_offset"));
        offsets_.node_texture_offset = native_profile::number(config.at("node_texture_offset"));
        offsets_.registry_count_offset = native_profile::number(config.at("registry_count_offset"));
        offsets_.registry_displacement_offset = native_profile::number(config.at("registry_displacement_offset"));
        offsets_.registry_instruction_bytes = native_profile::number(config.at("registry_instruction_bytes"));
        offsets_.registry_rva = native_profile::number(config.at("registry_rva"));
        offsets_.shared_pointer_bytes = native_profile::number(config.at("shared_pointer_bytes"));
        offsets_.source_coordinates_offset = native_profile::number(config.at("source_coordinates_offset"));
        offsets_.source_pbr_offset = native_profile::number(config.at("source_pbr_offset"));
        offsets_.source_stride = native_profile::number(config.at("source_stride"));
        offsets_.source_weight_offset = native_profile::number(config.at("source_weight_offset"));
        offsets_.string_capacity_offset = native_profile::number(config.at("string_capacity_offset"));
        offsets_.string_inline_bytes = native_profile::number(config.at("string_inline_bytes"));
        offsets_.string_size_offset = native_profile::number(config.at("string_size_offset"));
        offsets_.target_coordinates_offset = native_profile::number(config.at("target_coordinates_offset"));
        offsets_.target_pbr_offset = native_profile::number(config.at("target_pbr_offset"));
        offsets_.target_stride = native_profile::number(config.at("target_stride"));
        offsets_.target_weight_offset = native_profile::number(config.at("target_weight_offset"));
        offsets_.tree_nil_offset = native_profile::number(config.at("tree_nil_offset"));
        offsets_.tree_parent_offset = native_profile::number(config.at("tree_parent_offset"));
        offsets_.tree_right_offset = native_profile::number(config.at("tree_right_offset"));
        offsets_.tree_value_offset = native_profile::number(config.at("tree_value_offset"));
        offsets_.type_permutations_offset = native_profile::number(config.at("type_permutations_offset"));
        offsets_.unsupported_layer_first = native_profile::number(config.at("unsupported_layer_first"));
        offsets_.unsupported_layer_last = native_profile::number(config.at("unsupported_layer_last"));
        offsets_.write_bytes = native_profile::number(config.at("write_bytes"));
        const auto bind = [&](const char *key, const char *abi) {
            const auto &def = layout.at("functions").at(key);
            if (def.at("abi") != abi) throw std::runtime_error("custom UV ABI mismatch");
            return native_profile::checked(root, def);
        };
        components_ = reinterpret_cast<Getter>(bind("block_materials", "void* __fastcall(const void* block)"));
        baked_ = reinterpret_cast<Getter>(bind("block_baked_materials", "void* __fastcall(const void* block)"));
        lookup_ = reinterpret_cast<Lookup>(bind("atlas_texture_item", "void* __fastcall(void* atlasManager, const std::string* name)"));
        auto *guard = static_cast<const std::byte *>(native_profile::checked(root, config.at("registry_guard")));
        std::int32_t displacement{};
        if (!read(guard, offsets_.registry_displacement_offset, displacement))
            throw std::runtime_error("custom UV registry reference unreadable");
        registry_ = const_cast<std::byte *>(guard) + offsets_.registry_instruction_bytes + displacement;
        const auto base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
        const auto location = reinterpret_cast<std::uintptr_t>(registry_);
        if (location < base || location - base != offsets_.registry_rva)
            throw std::runtime_error("custom UV registry reference mismatch");
        for (const auto &entry : config.at("layout_guards")) native_profile::checked(root, entry);
        uv_ = {offsets_.source_stride, offsets_.target_stride, offsets_.source_coordinates_offset,
            offsets_.source_weight_offset, offsets_.source_pbr_offset, offsets_.target_coordinates_offset,
            offsets_.target_weight_offset, offsets_.target_pbr_offset, offsets_.write_bytes};
        if (!uv_.sourceStride || !uv_.targetStride || !uv_.writeBytes ||
            !offsets_.shared_pointer_bytes || !offsets_.maximum_variants ||
            uv_.writeBytes > uv_.targetStride || uv_.targetStride > 4096 || uv_.sourceStride > 4096)
            throw std::runtime_error("invalid custom UV record ABI");
    }
    void *atlasManager(const void *atlas) const {
        void *manager{};
        return read(atlas, offsets_.atlas_manager_offset, manager) ? manager : nullptr;
    }
    bool prepare(void *atlas) {
        counts = {};
        patches_.clear();
        if (!atlas) return false;
        void *head{}, *root{};
        std::size_t expected{};
        unsigned char nil{};
        if (!read(registry_, 0, head) || !head || !read(registry_, offsets_.registry_count_offset, expected) ||
            expected > offsets_.maximum_types || !read(head, offsets_.tree_nil_offset, nil) || nil != 1 ||
            !read(head, offsets_.tree_parent_offset, root)) return false;
        std::vector<void *> nodes{root};
        std::set<void *> visited, blocks;
        std::map<void *, std::string> materials;
        std::set<void *> ambiguous;
        std::size_t blockEntries{}, materialEntries{};
        while (!nodes.empty()) {
            void *node = nodes.back(); nodes.pop_back();
            if (node == head) continue;
            void *left{}, *right{}, *type{};
            if (!node || !visited.insert(node).second || visited.size() > expected ||
                !read(node, offsets_.tree_nil_offset, nil) || nil ||
                !read(node, 0, left) || !read(node, offsets_.tree_right_offset, right) ||
                !read(node, offsets_.tree_value_offset, type)) return false;
            nodes.push_back(left); nodes.push_back(right);
            if (!type) continue;
            Vector permutations{};
            if (!vector(type, offsets_.type_permutations_offset, sizeof(void *), offsets_.maximum_blocks, permutations)) return false;
            for (auto p = permutations.begin; p < permutations.end; p += sizeof(void *)) {
                if (++blockEntries > offsets_.maximum_blocks) return false;
                void *block{};
                if (!read(reinterpret_cast<void *>(p), 0, block)) return false;
                if (!block || !blocks.insert(block).second) continue;
                unsigned char flags{};
                if (!read(block, offsets_.block_flags_offset, flags)) return false;
                if (!(flags & offsets_.block_ready_mask)) continue;
                auto *baked = get(baked_, block);
                if (!baked) continue;
                Vector resolved{};
                if (!vector(baked, offsets_.baked_materials_offset, offsets_.shared_pointer_bytes, offsets_.maximum_materials_per_block, resolved)) return false;
                std::set<void *> used;
                for (auto q = resolved.begin; q < resolved.end; q += offsets_.shared_pointer_bytes) {
                    void *material{};
                    if (!read(reinterpret_cast<void *>(q), 0, material)) return false;
                    if (material) used.insert(material);
                }
                auto *component = get(components_, block);
                if (!component) continue;
                void *sentinel{}, *entry{};
                if (!read(component, offsets_.component_head_offset, sentinel) || !sentinel || !read(sentinel, 0, entry)) return false;
                std::set<void *> entries;
                while (entry != sentinel) {
                    if (!entry || !entries.insert(entry).second || entries.size() > offsets_.maximum_materials_per_block ||
                        ++materialEntries > offsets_.maximum_material_links) return false;
                    void *material{};
                    std::string texture;
                    if (!read(entry, offsets_.node_material_offset, material)) return false;
                    if (material && used.contains(material)) {
                        unsigned char layer{};
                        if (!read(entry, offsets_.node_render_layer_offset, layer)) return false;
                        if (layer >= offsets_.unsupported_layer_first && layer <= offsets_.unsupported_layer_last) {
                            ambiguous.insert(material);
                        } else if (name(entry, offsets_.node_texture_offset, texture)) {
                            auto [it, inserted] = materials.emplace(material, texture);
                            if (!inserted && it->second != texture) ambiguous.insert(material);
                            if (materials.size() > offsets_.maximum_materials) return false;
                        } else ++counts.unmatched;
                    }
                    if (!read(entry, 0, entry)) return false;
                }
            }
        }
        if (visited.size() != expected) return false;
        std::size_t rows{};
        for (const auto &[material, texture] : materials) {
            if (ambiguous.contains(material)) { ++counts.unmatched; continue; }
            auto *item = lookup(lookup_, atlas, &texture);
            Vector outer{}, source{}, target{};
            std::string resolvedName;
            // The lookup can silently return "missing". Never overwrite a live
            // material unless the resolved atlas item has the requested name.
            if (!item || !name(item, offsets_.item_name_offset, resolvedName) || resolvedName != texture || !vector(item, offsets_.item_textures_offset, sizeof(Vector), offsets_.maximum_variants, outer) ||
                outer.begin == outer.end ||
                !vector(reinterpret_cast<void *>(outer.begin), 0, uv_.sourceStride, offsets_.maximum_variants, source) ||
                !vector(material, offsets_.material_uvs_offset, uv_.targetStride, offsets_.maximum_variants, target)) {
                ++counts.unmatched; continue;
            }
            const auto count = (target.end - target.begin) / uv_.targetStride;
            if (!count || count != (source.end - source.begin) / uv_.sourceStride) { ++counts.mismatched; continue; }
            rows += count;
            if (rows > offsets_.maximum_uv_sets) return false;
            Patch patch{material, target, source, reinterpret_cast<void *>(outer.begin), {}, {}, {}};
            patch.before.resize(target.end - target.begin);
            patch.sourceBytes.resize(source.end - source.begin);
            if (!bytes(reinterpret_cast<void *>(source.begin), patch.sourceBytes.data(), patch.sourceBytes.size()) ||
                !bytes(reinterpret_cast<void *>(target.begin), patch.before.data(), patch.before.size()) ||
                !planMaterialUvs(uv_, patch.sourceBytes, patch.before, patch.after)) { ++counts.unmatched; continue; }
            if (patch.before == patch.after) { ++counts.unchanged; continue; }
            patches_.push_back(std::move(patch));
        }
        return true;
    }
    void discard() noexcept { patches_.clear(); }
    bool commit() {
        // Preflight every object and every byte again before the first write.
        std::map<std::uintptr_t, std::uintptr_t> ranges;
        for (const auto &p : patches_) {
            auto next = ranges.lower_bound(p.target.begin);
            if ((next != ranges.end() && next->first < p.target.end) ||
                (next != ranges.begin() && std::prev(next)->second > p.target.begin)) return false;
            ranges.emplace(p.target.begin, p.target.end);
            Vector target{}, source{};
            std::vector<std::byte> before(p.before.size()), src(p.sourceBytes.size());
            if (!read(p.material, offsets_.material_uvs_offset, target) || target != p.target ||
                !read(p.sourceVector, 0, source) || source != p.source ||
                !bytes(reinterpret_cast<void *>(target.begin), before.data(), before.size()) || before != p.before ||
                !bytes(reinterpret_cast<void *>(source.begin), src.data(), src.size()) || src != p.sourceBytes ||
                !writable(reinterpret_cast<void *>(target.begin), before.size())) return false;
        }
        std::vector<std::pair<void *, const std::byte *>> written;
        std::size_t total{};
        for (const auto &p : patches_) total += p.after.size() / uv_.targetStride;
        written.reserve(total); // No allocation can throw after the first write.
        for (const auto &p : patches_) {
            for (std::size_t i = 0; i < p.after.size(); i += uv_.targetStride) {
                auto *target = reinterpret_cast<void *>(p.target.begin + i);
                written.emplace_back(target, p.before.data() + i);
                if (!write(target, p.after.data() + i, uv_.writeBytes)) {
                    for (auto it = written.rbegin(); it != written.rend(); ++it) write(it->first, it->second, uv_.writeBytes);
                    return false;
                }
            }
        }
        counts.materials = patches_.size();
        counts.rows = written.size();
        patches_.clear();
        return true;
    }
};
} // namespace bedrock::detail
