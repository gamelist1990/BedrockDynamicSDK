#pragma once

#include "game_types.hpp"
#include "resolver.hpp"
#include "profiles.hpp"

#include <array>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>

namespace bedrock::sdk {

struct WorldLineSegment {
    Vector3 from{};
    Vector3 to{};
};

struct WorldNameVertex {
    Vector3 position{};
};

class WorldRenderSubscription {
public:
    WorldRenderSubscription() noexcept;
    ~WorldRenderSubscription();
    WorldRenderSubscription(WorldRenderSubscription&&) noexcept;
    WorldRenderSubscription& operator=(WorldRenderSubscription&&) noexcept;
    WorldRenderSubscription(const WorldRenderSubscription&) = delete;
    WorldRenderSubscription& operator=(const WorldRenderSubscription&) = delete;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    friend class WorldRender;
};

class WorldRenderer {
public:
    WorldRenderer();
    ~WorldRenderer();

    [[nodiscard]] Vector3 origin() const noexcept;
    bool drawLines(std::span<const WorldLineSegment> segments,
                  std::array<float, 4> rgba,
                  float thickness = 1.0f) noexcept;
    bool drawNameQuad(std::array<WorldNameVertex, 4> vertices,
                      std::array<float, 4> rgba = {1.0f, 1.0f, 1.0f, 1.0f}) noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    friend class WorldRender;
    friend void worldRenderPostImpl(WorldRenderer&, void*, void*, void*);
};

class WorldRender {
public:
    WorldRender();
    ~WorldRender();
    WorldRender(const WorldRender&) = delete;
    WorldRender& operator=(const WorldRender&) = delete;

    bool install(const SymbolResolver& resolver, std::string_view profileJson,
                 std::string& error);
    bool install(const Profile& profile, std::string& error);
    bool install(std::string_view selectedProfileJson,
                 std::string_view embeddedProfileJson,
                 std::string& error);

    WorldRenderSubscription onPostRender(
        std::function<void(WorldRenderer&)> callback);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    friend void worldRenderPostImpl(WorldRenderer&, void*, void*, void*);
    friend void dispatchWorldRenderPost(void*, void*, void*);
};

} // namespace bedrock::sdk
