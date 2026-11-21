#include <bedrock/sdk/world_render.hpp>

#include <bedrock/profiles/profile_json.hpp>
#include "profile_facade.hpp"
#ifdef _WIN32
#include <bedrock/native/outline_renderer.hpp>
#include <bedrock/runtime/hook_runtime.h>
#include <bedrock/detail/hook_dispatch.hpp>
#include <bedrock/profiles/resolver.hpp>
#include <atomic>
#include <vector>
#include <span>
#endif

namespace bedrock::sdk {

struct WorldRenderSubscription::Impl {};
WorldRenderSubscription::WorldRenderSubscription() noexcept = default;
WorldRenderSubscription::~WorldRenderSubscription() = default;
WorldRenderSubscription::WorldRenderSubscription(WorldRenderSubscription&&) noexcept = default;
WorldRenderSubscription& WorldRenderSubscription::operator=(WorldRenderSubscription&&) noexcept = default;

struct WorldRenderer::Impl {
#ifdef _WIN32
    void* renderer{};
    void* screen{};
    bedrock::NativeOutlineRenderer* outline{};
#endif
};
WorldRenderer::WorldRenderer():impl_(std::make_unique<Impl>()){}
WorldRenderer::~WorldRenderer()=default;
#ifdef _WIN32
void worldRenderPostImpl(WorldRenderer& renderer, void* nativeRenderer, void* screen, void* outline) {
    renderer.impl_->renderer = nativeRenderer;
    renderer.impl_->screen = screen;
    renderer.impl_->outline = static_cast<bedrock::NativeOutlineRenderer*>(outline);
}
#endif
Vector3 WorldRenderer::origin() const noexcept {
#ifdef _WIN32
    if (impl_->outline && impl_->renderer) {
        if (auto value=impl_->outline->renderOrigin(impl_->renderer)) {
            return {value->x,value->y,value->z};
        }
    }
#endif
    return {};
}
bool WorldRenderer::drawLines(std::span<const WorldLineSegment> segments,
                              std::array<float,4> rgba,float) noexcept {
#ifdef _WIN32
    if(!impl_->outline || !impl_->renderer || !impl_->screen || segments.empty()) return false;
    bedrock::WorldOutlineGeometry geometry;
    geometry.origin=bedrock::Vec3{origin().x,origin().y,origin().z};
    bedrock::WorldGeometryBatch batch;
    batch.color=rgba;
    try {
        batch.vertices.reserve(segments.size()*2);
        for(const auto& segment:segments) {
            batch.vertices.push_back({segment.from.x,segment.from.y,segment.from.z});
            batch.vertices.push_back({segment.to.x,segment.to.y,segment.to.z});
        }
        geometry.lines.push_back(std::move(batch));
    } catch(...) { return false; }
    auto result=impl_->outline->draw(impl_->renderer,impl_->screen,geometry);
    return result==bedrock::OutlineDraw::Drawn;
#else
    (void)segments; (void)rgba;
    return false;
#endif
}
bool WorldRenderer::drawNameQuad(std::array<WorldNameVertex,4> vertices,
                                 std::array<float,4>) noexcept {
#ifdef _WIN32
    if(!impl_->outline || !impl_->renderer || !impl_->screen) return false;
    bedrock::WorldOutlineGeometry geometry;
    geometry.origin=bedrock::Vec3{origin().x,origin().y,origin().z};
    try {
        geometry.textQuads.reserve(8);
        for(std::size_t pass=0;pass<2;++pass) {
            geometry.textQuads.push_back({vertices[0].position.x,vertices[0].position.y,vertices[0].position.z});
            geometry.textQuads.push_back({vertices[1].position.x,vertices[1].position.y,vertices[1].position.z});
            geometry.textQuads.push_back({vertices[2].position.x,vertices[2].position.y,vertices[2].position.z});
            geometry.textQuads.push_back({vertices[3].position.x,vertices[3].position.y,vertices[3].position.z});
        }
    } catch(...) { return false; }
    auto result=impl_->outline->draw(impl_->renderer,impl_->screen,geometry);
    return result==bedrock::OutlineDraw::Drawn;
#else
    (void)vertices;
    return false;
#endif
}

struct WorldRender::Impl {
#ifdef _WIN32
    bedrock::NativeOutlineRenderer outline;
    BDS_HookToken token{};
    std::vector<std::function<void(WorldRenderer&)>> callbacks;
    std::atomic<bool> drawing{};
#endif
};

#ifdef _WIN32
void dispatchWorldRenderPost(void* nativeRenderer, void* screen, void* user) {
    auto& impl = *static_cast<WorldRender::Impl*>(user);
    bool expected = false;
    if (!impl.drawing.compare_exchange_strong(expected, true)) return;
    struct DrawingReset { std::atomic<bool>& drawing; ~DrawingReset(){drawing.store(false);} } reset{impl.drawing};
    try {
        WorldRenderer renderer;
        worldRenderPostImpl(renderer, nativeRenderer, screen, &impl.outline);
        for (const auto& callback : impl.callbacks) callback(renderer);
    } catch (...) {}
}

void BDS_HOOK_CALL worldRenderHookPost(void* user, void* raw,
                                       BDS_HookNext next, void* frame) noexcept {
    auto* args = static_cast<BDS_RenderLevelArgs*>(raw);
    next(frame, raw);
    dispatchWorldRenderPost(args->renderer, args->screen, user);
}
#endif

WorldRender::WorldRender():impl_(std::make_unique<Impl>()){}
WorldRender::~WorldRender()=default;

bool WorldRender::install(const SymbolResolver&, std::string_view profileJson,std::string& error) {
    return install(profileJson,std::string_view{},error);
}
bool WorldRender::install(const Profile& profile,std::string& error) {
#ifdef _WIN32
    const auto runtime=ProfileRuntimeAccess::runtime(profile);
    if(!runtime) { error="world render profile runtime snapshot unavailable"; return false; }
    if(!impl_->outline.load(*runtime,error)) return false;
    auto* target=impl_->outline.renderTarget();
    if(!target) { error="world render target unavailable"; return false; }
    if(!BDS_HookSubscribe(BDS_HOOK_RENDER_LEVEL,target,&worldRenderHookPost,impl_.get(),&impl_->token)) {
        error="shared post-renderLevel hook registration failed"; return false;
    }
    return true;
#else
    (void)profile;
    error="world render unavailable on this host";
    return false;
#endif
}

bool WorldRender::install(std::string_view selectedProfileJson,
                         std::string_view embeddedProfileJson,std::string& error) {
#ifdef _WIN32
    const auto& json=selectedProfileJson.empty()?embeddedProfileJson:selectedProfileJson;
    try {
        bedrock::ProfileManager manager;
        if(!manager.prepareEmbeddedJson(std::string(json), [&](const bedrock::RuntimeProfile& profile,std::string& inner){
            return impl_->outline.load(profile,inner);
        },error)) return false;
        auto* target=impl_->outline.renderTarget();
        if(!target) { error="world render target unavailable"; return false; }
        if(!BDS_HookSubscribe(BDS_HOOK_RENDER_LEVEL,target,&worldRenderHookPost,impl_.get(),&impl_->token)) {
            error="shared post-renderLevel hook registration failed"; return false;
        }
        return true;
    } catch(const std::exception& e) { error=e.what(); return false; }
#else
    (void)selectedProfileJson; (void)embeddedProfileJson;
    error="world render unavailable on this host";
    return false;
#endif
}

WorldRenderSubscription WorldRender::onPostRender(std::function<void(WorldRenderer&)> callback) {
    WorldRenderSubscription subscription;
#ifdef _WIN32
    if(callback) impl_->callbacks.push_back(std::move(callback));
#endif
    (void)callback;
    return subscription;
}

} // namespace bedrock::sdk
