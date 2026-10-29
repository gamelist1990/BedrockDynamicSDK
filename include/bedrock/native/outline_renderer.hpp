#pragma once
#include <bedrock/enums/render.hpp>
#include <bedrock/profiles/profile_json.hpp>
#include <bedrock/native/world_render.hpp>
#include <bedrock/detail/native_profile_functions.hpp>
#include <bedrock/api/game_api.hpp>
#include <atomic>
#include <span>

namespace bedrock {
struct WorldGeometryBatch {
    std::array<float,4> color{1,1,1,1};
    std::vector<Vec3> vertices;
};
struct WorldOutlineGeometry {
    Vec3 origin{}; // Every vertex is relative to this copied native render origin.
    std::vector<WorldGeometryBatch> lines;
    std::vector<Vec3> textQuads; // Double-sided quads, four vertices per face.
};


// Render-thread-only submission of worker-prepared geometry. Uses the engine's
// current view/projection and name_tag material, and restores borrowed scalars.
class NativeOutlineRenderer {
public:
    bool load(const RuntimeProfile& profile, std::string& error) {
        ready_ = false;
        if (!profile.supports({Feature::WorldRender}, error)) return false;
        if (!parseWorldRenderProfile(profile.json(), layout_, error)) return false;
        try {
            const auto root = bedrock::parseNativeProfile(profile.json());
            const auto& functions = root.at("native_world_render").at("functions");
            const auto base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
            target_ = reinterpret_cast<void*>(base + layout_.functions[0].rva);
            if (!BDS_HookOwnsTarget(BDS_HOOK_RENDER_LEVEL, target_)) native_profile::checked(root, functions.at("render_level"));
            begin_ = reinterpret_cast<Begin>(native_profile::checked(root, functions.at("begin")));
            vertex_ = reinterpret_cast<Vertex>(native_profile::checked(root, functions.at("vertex")));
            submit_ = reinterpret_cast<Submit>(native_profile::checked(root, functions.at("render_mesh")));
            name_ = reinterpret_cast<Name>(native_profile::checked(root, functions.at("hashed_name_ctor")));
            material_ = reinterpret_cast<Material>(native_profile::checked(root, functions.at("material_ctor")));
            group_ = reinterpret_cast<void*>(base + layout_.commonMaterialGroup);
            ready_ = true; error.clear(); return true;
        } catch (const std::exception& e) { error = e.what(); return false; }
    }
    void* renderTarget() const noexcept { return ready_ ? target_ : nullptr; }
    std::optional<Vec3> renderOrigin(void* renderer) const noexcept {
        void* camera{}; Vec3 origin;
        if (!ready_ || !native_profile::read(renderer, layout_.player, camera) ||
            !native_profile::read(camera, layout_.origin, origin) || !std::isfinite(origin.x) ||
            !std::isfinite(origin.y) || !std::isfinite(origin.z)) return std::nullopt;
        return origin;
    }
    OutlineDraw draw(void* renderer, void* screen, const WorldOutlineGeometry& geometry) noexcept {
        if (!ready_) return OutlineDraw::Skipped;
        DWORD expected{}; owner_.compare_exchange_strong(expected, GetCurrentThreadId());
        if (owner_.load() != GetCurrentThreadId()) return OutlineDraw::Skipped;
        void *camera{}, *tess{}, *shader{}, *groupTable{};
        Vec3 origin{}, previous{}; std::array<float,4> color{};
        unsigned char building{}, special{};
        if (!native_profile::read(renderer, layout_.player, camera) ||
            !native_profile::read(camera, layout_.origin, origin) || !std::isfinite(origin.x) ||
            !std::isfinite(origin.y) || !std::isfinite(origin.z) ||
            !native_profile::read(screen, layout_.tess, tess) || !tess ||
            !native_profile::read(screen, layout_.shader, shader) || !shader ||
            !native_profile::read(tess, layout_.building, building) || building ||
            !native_profile::read(tess, layout_.special, special) || special ||
            !native_profile::read(tess, layout_.translation, previous) ||
            !native_profile::read(shader, 0, color) || !native_profile::read(group_, 0, groupTable) || !groupTable)
            return OutlineDraw::Skipped;
        bool ok = true;
        const Vec3 delta{geometry.origin.x-origin.x, geometry.origin.y-origin.y, geometry.origin.z-origin.z};
        __try {
            if (!constructed_) {
                name_(nameStorage_.data(), "name_tag");
                material_(materialStorage_.data(), group_, nameStorage_.data());
                void* value{};
                if (!native_profile::read(materialStorage_.data(), 0, value) || !value) ok = false;
                else constructed_ = true;
            }
            if (ok && !write(tess, layout_.translation, delta)) ok = false;
            for (const auto& batch : geometry.lines) {
                if (!ok) break;
                ok = drawVertices(screen, tess, shader, batch.vertices, batch.color, 4, 2);
            }
            if (ok) ok = drawVertices(screen, tess, shader, geometry.textQuads, {1,1,1,1}, 1, 4);
        } __except(EXCEPTION_EXECUTE_HANDLER) { ok = false; }
        const unsigned char dirty = 1;
        write(shader, 0, color); write(shader, layout_.dirty, dirty); write(tess, layout_.translation, previous);
        return ok ? OutlineDraw::Drawn : OutlineDraw::Fault;
    }
private:
    template<class T> static bool write(void* pointer, std::size_t offset, const T& value) noexcept {
        if (!pointer) return false;
        __try { std::memcpy(static_cast<std::byte*>(pointer)+offset, &value, sizeof(T)); return true; }
        __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
    bool drawVertices(void* screen, void* tess, void* shader, const std::vector<Vec3>& vertices,
                      const std::array<float,4>& color, int topology, std::size_t primitive) {
        if (vertices.size()%primitive) return false;
        for (std::size_t start = 0; start < vertices.size();) {
            const auto count = std::min<std::size_t>(12288, vertices.size()-start);
            const unsigned char dirty = 1;
            if (!write(shader, 0, color) || !write(shader, layout_.dirty, dirty)) return false;
            begin_(tess, 0, topology, static_cast<int>(count), false);
            for (std::size_t i=0; i<count; ++i) {
                const auto& point = vertices[start+i]; vertex_(tess, point.x, point.y, point.z);
            }
            alignas(16) std::array<std::byte,0x58> scratch{};
            submit_(screen, tess, materialStorage_.data(), scratch.data()); start += count;
        }
        return true;
    }
    using Begin = void(__fastcall*)(void*,int,int,int,bool);
    using Vertex = void(__fastcall*)(void*,float,float,float);
    using Submit = void(__fastcall*)(void*,void*,void*,void*);
    using Name = void*(__fastcall*)(void*,const char*);
    using Material = void*(__fastcall*)(void*,void*,const void*);
    WorldRenderProfile layout_;
    void *target_{}, *group_{};
    Begin begin_{}; Vertex vertex_{}; Submit submit_{}; Name name_{}; Material material_{};
    alignas(16) std::array<std::byte,48> nameStorage_{};
    alignas(16) std::array<std::byte,16> materialStorage_{};
    std::atomic<DWORD> owner_{};
    bool ready_{}, constructed_{};
};
} // namespace bedrock
