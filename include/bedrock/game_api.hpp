#pragma once

// Bukkit-inspired, version-independent client API. Minecraft memory access is
// deliberately isolated behind GameAccess: wrappers never guess offsets or
// call an unverified game method themselves.
#include <bedrock/events.hpp>
#include <bedrock/hook_runtime.h>
#include <bedrock/native_keyboard.hpp>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdint>
#include <mutex>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace bedrock {

struct ServerAddress {
    std::string ip;
    std::uint16_t port{};

    [[nodiscard]] bool valid() const noexcept { return !ip.empty() && port != 0; }
    [[nodiscard]] std::string address() const {
        if (ip.find(':') != std::string::npos && (ip.empty() || ip.front() != '['))
            return "[" + ip + "]:" + std::to_string(port);
        return ip + ":" + std::to_string(port);
    }
};

// Values match Minecraft's MouseDevice::feed InputKey values.
enum class MouseButton : unsigned { Left = 1, Right = 2, Middle = 3, X1 = 4, X2 = 5 };

// Implemented by the game-specific adapter after it has validated the live
// pointers/layout for the exact executable. All returned pointers are borrowed.
class GameAccess {
public:
    virtual ~GameAccess() = default;
    [[nodiscard]] virtual bool isActorValid(void* actor) const noexcept = 0;
    [[nodiscard]] virtual void* localPlayer() const noexcept = 0;
    [[nodiscard]] virtual void* worldOf(void* actor) const noexcept = 0;
    [[nodiscard]] virtual std::optional<std::string> nameTag(void* actor) const = 0;
    [[nodiscard]] virtual std::vector<std::string> playerListNames(void* world) const = 0;
    [[nodiscard]] virtual std::optional<ServerAddress> serverAddress(void* localPlayer) const = 0;

    // Input methods return false when the adapter cannot perform that action.
    // No Win32 input injection is used by these wrappers.
    [[nodiscard]] virtual bool keyDown(unsigned key) const noexcept { (void)key; return false; }
    virtual bool sendKey(unsigned key, bool pressed) noexcept {
        (void)key; (void)pressed; return false;
    }
    [[nodiscard]] virtual bool mouseDown(MouseButton button) const noexcept {
        (void)button; return false;
    }
    virtual bool click(MouseButton button) noexcept { (void)button; return false; }
    virtual bool moveMouse(int deltaX, int deltaY) noexcept {
        (void)deltaX; (void)deltaY; return false;
    }
    virtual bool turn(float yawDelta, float pitchDelta) noexcept {
        (void)yawDelta; (void)pitchDelta; return false;
    }
};

// Useful for input-only examples that do not need to provide version-specific
// Actor/world access. The hook-backed keyboard and mouse APIs still work.
class EmptyGameAccess final : public GameAccess {
public:
    [[nodiscard]] bool isActorValid(void*) const noexcept override { return false; }
    [[nodiscard]] void* localPlayer() const noexcept override { return nullptr; }
    [[nodiscard]] void* worldOf(void*) const noexcept override { return nullptr; }
    [[nodiscard]] std::optional<std::string> nameTag(void*) const override {
        return std::nullopt;
    }
    [[nodiscard]] std::vector<std::string> playerListNames(void*) const override {
        return {};
    }
    [[nodiscard]] std::optional<ServerAddress> serverAddress(void*) const override {
        return std::nullopt;
    }
};
inline EmptyGameAccess& emptyGameAccess() {
    static EmptyGameAccess access;
    return access;
}

struct GameEventRegistry {
    struct EnumType {
        EventChannel<KeyEvent> Keyboard;
        EventChannel<NativeKeyEvent> NativeKeyboard;
        EventChannel<MouseEvent> Mouse;
        EventChannel<ActorNameTagEvent> ActorNameTag;
        EventChannel<ViewNameTagEvent> ViewNameTag;
        EventChannel<UpdateEvent> Update;
        EventChannel<GameplayEvent> Gameplay;
        EventChannel<FovEvent> Fov;
    } Enum;
};

class GameApi;
class World;

class Actor {
public:
    Actor() = default;
    Actor(GameApi& api, void* handle) noexcept : api_(&api), handle_(handle) {}

    [[nodiscard]] static Actor forActor(GameApi& api, void* handle) noexcept {
        return Actor{api, handle};
    }
    [[nodiscard]] bool isValid() const noexcept;
    [[nodiscard]] explicit operator bool() const noexcept { return isValid(); }
    [[nodiscard]] void* handle() const noexcept { return isValid() ? handle_ : nullptr; }
    [[nodiscard]] std::string getNameTag() const;
    [[nodiscard]] std::string getName() const;
    [[nodiscard]] bool isPlayer() const;
    [[nodiscard]] World getWorld() const;
    [[nodiscard]] bool sameActor(const Actor& other) const noexcept {
        return api_ == other.api_ && handle_ == other.handle_ && handle_ != nullptr;
    }

private:
    GameApi* api_{};
    void* handle_{};
    friend class GameApi;
    friend class LocalPlayer;
    friend class World;
};

class World {
public:
    World() = default;
    World(GameApi& api, void* handle) noexcept : api_(&api), handle_(handle) {}
    [[nodiscard]] bool isValid() const noexcept { return api_ && handle_; }
    [[nodiscard]] void* handle() const noexcept { return isValid() ? handle_ : nullptr; }
    [[nodiscard]] std::vector<Actor> getActors() const;
    [[nodiscard]] std::vector<std::string> getPlayerNames() const;
private:
    GameApi* api_{};
    void* handle_{};
    friend class GameApi;
};

class Keyboard {
public:
    Keyboard() = default;
    explicit Keyboard(GameApi* api) noexcept : api_(api) {}
    [[nodiscard]] bool isDown(unsigned key) const noexcept;
    bool press(unsigned key) const noexcept;
    bool release(unsigned key) const noexcept;
    bool tap(unsigned key) const noexcept {
        if (!press(key)) return false;
        return release(key);
    }
private:
    GameApi* api_{};
};

class Mouse {
public:
    Mouse() = default;
    explicit Mouse(GameApi* api) noexcept : api_(api) {}
    [[nodiscard]] bool isDown(MouseButton button) const noexcept;
    bool setDown(MouseButton button, bool down) const noexcept;
    bool click(MouseButton button) const noexcept;
    bool move(int deltaX, int deltaY) const noexcept;
private:
    GameApi* api_{};
};

class Server {
public:
    Server() = default;
    explicit Server(ServerAddress endpoint) : endpoint_(std::move(endpoint)) {}
    [[nodiscard]] bool isValid() const noexcept { return endpoint_.valid(); }
    [[nodiscard]] const std::string& ip() const noexcept { return endpoint_.ip; }
    [[nodiscard]] std::uint16_t port() const noexcept { return endpoint_.port; }
    [[nodiscard]] std::string address() const { return endpoint_.address(); }
private:
    ServerAddress endpoint_;
};

class LocalPlayer final : public Actor {
public:
    LocalPlayer() = default;
    explicit LocalPlayer(GameApi& api) noexcept;
    [[nodiscard]] Actor asActor() const noexcept {
        return api_ ? Actor{*api_, handle_} : Actor{};
    }
    [[nodiscard]] static std::optional<LocalPlayer> forActor(const Actor& actor) noexcept;
    [[nodiscard]] Keyboard getKeyboard() const noexcept;
    [[nodiscard]] Mouse getMouse() const noexcept;
    [[nodiscard]] std::optional<Server> getServer() const;
    [[nodiscard]] std::optional<Server> server() const { return getServer(); }
    bool turn(float yawDelta, float pitchDelta) const noexcept;
};

struct CachedActorNameTag {
    void* actor{};
    std::string nameTag;
    NameTagSource source{NameTagSource::ActorGetter};
};

class GameApi {
public:
    GameApi() noexcept : access_(&emptyGameAccess()) {}
    explicit GameApi(GameAccess& access) noexcept : access_(&access) {}
    GameApi(const GameApi&) = delete;
    GameApi& operator=(const GameApi&) = delete;

    GameEventRegistry Event;

    [[nodiscard]] Actor actor(void* handle) noexcept { return Actor{*this, handle}; }
    [[nodiscard]] World world(void* handle) noexcept { return World{*this, handle}; }
    [[nodiscard]] LocalPlayer localPlayer() noexcept { return LocalPlayer{*this}; }
    [[nodiscard]] GameAccess& access() const noexcept { return *access_; }

    // Adapter entry points. A view adapter should call observeViewNameTag for
    // names sampled from view/render state; the Actor getter hook calls
    // observeActorNameTag. Both sources feed Actor::forActor and World::getActors.
    void observeViewActor(void* actor) {
        if (!validActor(actor)) return;
        std::lock_guard lock(mutex_);
        if (viewActors_.insert(actor).second) invalidateActorSnapshots();
    }
    void clearObservedActors() {
        std::lock_guard lock(mutex_);
        nameTags_.clear();
        actorGetterTags_.clear();
        viewNameTags_.clear();
        viewActors_.clear();
        actorGetterHandles_.clear();
        invalidateActorSnapshots();
    }
    void observeActorNameTag(void* actor, std::string_view nameTag) {
        if (!validActor(actor) || nameTag.empty()) return;
        if (!rememberActor(actor, nameTag)) return;
        Event.Enum.ActorNameTag.emit(ActorNameTagEvent{actor, nameTag,
                                                       NameTagSource::ActorGetter});
    }
    void observeViewNameTag(void* actor, std::string_view nameTag) {
        if (!validActor(actor) || nameTag.empty()) return;
        bool viewChanged = false;
        {
            std::lock_guard lock(mutex_);
            viewActors_.insert(actor);
            const auto it = viewNameTags_.find(actor);
            viewChanged = it == viewNameTags_.end() || it->second != nameTag;
            viewNameTags_.insert_or_assign(actor, std::string(nameTag));
            if (viewActors_.insert(actor).second || viewChanged) invalidateActorSnapshots();
        }
        const bool actorChanged = rememberActor(actor, nameTag, NameTagSource::View);
        if (viewChanged) Event.Enum.ViewNameTag.emit(ViewNameTagEvent{actor, nameTag});
        if (actorChanged) Event.Enum.ActorNameTag.emit(ActorNameTagEvent{
            actor, nameTag, NameTagSource::View});
    }
    [[nodiscard]] bool dispatchKey(KeyEvent& event) noexcept {
        try { Event.Enum.Keyboard.emit(event); }
        catch (...) { return true; } // callback failures must not swallow game input
        return !event.cancelled();
    }
    [[nodiscard]] bool dispatchMouse(MouseEvent& event) noexcept {
        try { Event.Enum.Mouse.emit(event); }
        catch (...) { return true; }
        return !event.cancelled();
    }
    void dispatchUpdate(const UpdateEvent& event) noexcept {
        try { Event.Enum.Update.emit(event); } catch (...) {}
    }
    void dispatchGameplay(const GameplayEvent& event) noexcept {
        gameplayActive_.store(event.active, std::memory_order_release);
        try { Event.Enum.Gameplay.emit(event); } catch (...) {}
    }
    void dispatchFov(FovEvent& event) noexcept {
        try { Event.Enum.Fov.emit(event); } catch (...) {}
    }
    [[nodiscard]] bool gameplayActive() const noexcept {
        return gameplayActive_.load(std::memory_order_acquire);
    }
    void dispatchNativeKeyboard(const NativeKeyboardEvent& captured) noexcept {
        {
            std::lock_guard lock(inputMutex_);
            nativeKeyboard_ = captured.input;
        }
        try { Event.Enum.NativeKeyboard.emit(
            NativeKeyEvent{static_cast<unsigned>(captured.key), captured.down}); }
        catch (...) {}
    }
    void clearNativeKeyboard() noexcept {
        std::lock_guard lock(inputMutex_);
        nativeKeyboard_.reset();
    }
    [[nodiscard]] bool sendNativeKey(unsigned key, bool pressed) noexcept {
        std::optional<NativeKeyboardInput> route;
        {
            std::lock_guard lock(inputMutex_);
            route = nativeKeyboard_;
        }
        return route && route->send(static_cast<int>(key), pressed);
    }
    void observeMouseDevice(const BDS_MouseArgs& args) noexcept {
        std::lock_guard lock(inputMutex_);
        mouseDevice_ = args.self;
        lastMouseArgs_ = args;
    }
    [[nodiscard]] bool sendMouseInput(unsigned action, bool pressed) noexcept {
#ifdef _WIN32
        BDS_MouseArgs args{};
        {
            std::lock_guard lock(inputMutex_);
            if (!mouseDevice_) return false;
            args = lastMouseArgs_;
        }
        args.action = static_cast<std::int8_t>(action);
        args.state = pressed ? 1 : 0;
        args.force_motionless_pointer =
            (args.force_motionless_pointer & BDS_MOUSE_FLAG_FORCE_MOTIONLESS) |
            BDS_MOUSE_FLAG_SYNTHETIC;
        const auto* previousSyntheticApi = syntheticMouseApi_;
        syntheticMouseApi_ = this;
        BDS_HookDispatch(BDS_HOOK_MOUSE_FEED, &args);
        syntheticMouseApi_ = previousSyntheticApi;
        return true;
#else
        (void)action; (void)pressed;
        return false;
#endif
    }
    [[nodiscard]] bool isSyntheticMouseDispatch() const noexcept {
        return syntheticMouseApi_ == this;
    }

    // Immutable per-source snapshots let render consumers reuse the cache
    // without rewalking or copying all observed Actor handles each frame.
    [[nodiscard]] std::shared_ptr<const std::vector<CachedActorNameTag>>
    cachedActorNameTags(NameTagSource source) const {
        std::lock_guard lock(mutex_);
        auto& cached = source == NameTagSource::View
            ? cachedViewNameTags_ : cachedActorNameTags_;
        if (!cached) {
            auto result = std::make_shared<std::vector<CachedActorNameTag>>();
            const auto& tags = source == NameTagSource::View ? viewNameTags_ : actorGetterTags_;
            result->reserve(tags.size());
            for (const auto& [actor, tag] : tags)
                result->push_back({actor, tag, source});
            cached = std::move(result);
        }
        return cached;
    }

    [[nodiscard]] std::shared_ptr<const std::vector<Actor>>
    cachedActors(NameTagSource source) const {
        std::lock_guard lock(mutex_);
        auto& cached = source == NameTagSource::View
            ? cachedViewActors_ : cachedNameTagActors_;
        if (!cached) {
            auto result = std::make_shared<std::vector<Actor>>();
            const auto& handles = source == NameTagSource::View
                ? viewActors_ : actorGetterHandles_;
            result->reserve(handles.size());
            for (auto* handle : handles)
                if (validActor(handle)) result->emplace_back(*const_cast<GameApi*>(this), handle);
            cached = std::move(result);
        }
        return cached;
    }

    // Refresh a bounded batch of already observed Actors through the
    // version-specific GameAccess adapter. Call from a game update/tick, not
    // from a render callback. New Actors are discovered by the View/getter
    // observation paths; this method keeps their names/colors live when their
    // tags are not currently being rendered.
    std::size_t refreshCachedActorNameTags(std::size_t maximumActors = 64) noexcept {
        if (!access_ || maximumActors == 0) return 0;
        try {
            const auto view = cachedActors(NameTagSource::View);
            const auto getter = cachedActors(NameTagSource::ActorGetter);
            std::vector<std::pair<void*, NameTagSource>> candidates;
            candidates.reserve(view->size() + getter->size() + 1);
            std::unordered_set<void*> seen;
            seen.reserve(candidates.capacity());
            const auto append = [&](const auto& actors, NameTagSource source) {
                for (const auto& actor : actors) {
                    auto* handle = actor.handle_;
                    if (handle && seen.insert(handle).second)
                        candidates.emplace_back(handle, source);
                }
            };
            // Prefer View for dual-source Actors: its independently captured
            // rendering state is more directly associated with the visible tag.
            append(*view, NameTagSource::View);
            append(*getter, NameTagSource::ActorGetter);
            auto* local = access_->localPlayer();
            if (validActor(local) && seen.insert(local).second)
                candidates.emplace_back(local, NameTagSource::ActorGetter);
            if (candidates.empty()) return 0;
            const auto start = refreshCursor_.fetch_add(maximumActors,
                std::memory_order_relaxed) % candidates.size();
            const auto count = std::min(maximumActors, candidates.size());
            std::size_t refreshed = 0;
            for (std::size_t i = 0; i < count; ++i) {
                const auto& [handle, source] = candidates[(start + i) % candidates.size()];
                if (!validActor(handle)) continue;
                auto tag = access_->nameTag(handle);
                if (!tag || tag->empty()) continue;
                if (source == NameTagSource::View) observeViewNameTag(handle, *tag);
                else observeActorNameTag(handle, *tag);
                ++refreshed;
            }
            return refreshed;
        } catch (...) {
            return 0;
        }
    }

    [[nodiscard]] std::vector<Actor> actors(void* world = nullptr) {
        const auto view = cachedActors(NameTagSource::View);
        const auto named = cachedActors(NameTagSource::ActorGetter);
        std::unordered_set<void*> handles;
        handles.reserve(view->size() + named->size() + 1);
        for (const auto& actor : *view) handles.insert(actor.handle_);
        for (const auto& actor : *named) handles.insert(actor.handle_);
        // The local player is useful even before a render/name-tag callback has
        // observed it, and is itself a member of the active world.
        if (auto* local = access_ ? access_->localPlayer() : nullptr)
            handles.insert(local);
        std::vector<Actor> result;
        result.reserve(handles.size());
        for (auto* handle : handles) {
            if (!validActor(handle)) continue;
            if (world && access_->worldOf(handle) != world) continue;
            result.emplace_back(*this, handle);
        }
        return result;
    }

private:
    void invalidateActorSnapshots() const {
        cachedViewNameTags_.reset();
        cachedActorNameTags_.reset();
        cachedViewActors_.reset();
        cachedNameTagActors_.reset();
    }
    bool rememberActor(void* actor, std::string_view tag,
                       NameTagSource source = NameTagSource::ActorGetter) {
        std::lock_guard lock(mutex_);
        const auto it = nameTags_.find(actor);
        const bool changed = it == nameTags_.end() || it->second != tag;
        nameTags_.insert_or_assign(actor, std::string(tag));
        bool sourceChanged = false;
        if (source == NameTagSource::ActorGetter) {
            const auto sourceIt = actorGetterTags_.find(actor);
            sourceChanged = sourceIt == actorGetterTags_.end() || sourceIt->second != tag;
            actorGetterTags_.insert_or_assign(actor, std::string(tag));
            actorGetterHandles_.insert(actor);
        }
        if (source == NameTagSource::View) {
            sourceChanged = viewActors_.insert(actor).second;
        }
        if (changed || sourceChanged) invalidateActorSnapshots();
        return changed || sourceChanged;
    }
    [[nodiscard]] bool validActor(void* actor) const noexcept {
        return access_ && actor && access_->isActorValid(actor);
    }
    [[nodiscard]] std::optional<std::string> cachedNameTag(void* actor) const {
        std::lock_guard lock(mutex_);
        const auto it = nameTags_.find(actor);
        if (it == nameTags_.end()) return std::nullopt;
        return it->second;
    }
    [[nodiscard]] bool playerMatch(void* actor, std::string_view plainTag,
                                   std::string* matched = nullptr) const;

    GameAccess* access_{};
    mutable std::mutex inputMutex_;
    std::optional<NativeKeyboardInput> nativeKeyboard_;
    void* mouseDevice_{};
    BDS_MouseArgs lastMouseArgs_{};
    std::atomic<bool> gameplayActive_{false};
    std::atomic<std::size_t> refreshCursor_{0};
    inline static thread_local const GameApi* syntheticMouseApi_{};
    mutable std::mutex mutex_;
    std::unordered_map<void*, std::string> nameTags_;
    std::unordered_map<void*, std::string> actorGetterTags_;
    std::unordered_map<void*, std::string> viewNameTags_;
    std::unordered_set<void*> viewActors_;
    std::unordered_set<void*> actorGetterHandles_;
    mutable std::shared_ptr<const std::vector<CachedActorNameTag>> cachedActorNameTags_;
    mutable std::shared_ptr<const std::vector<CachedActorNameTag>> cachedViewNameTags_;
    mutable std::shared_ptr<const std::vector<Actor>> cachedNameTagActors_;
    mutable std::shared_ptr<const std::vector<Actor>> cachedViewActors_;
    friend class Actor;
    friend class LocalPlayer;
    friend class World;
};

namespace detail {
inline std::string stripFormatting(std::string_view text) {
    std::string result;
    result.reserve(text.size());
    for (std::size_t i = 0; i < text.size();) {
        std::size_t length = 0;
        const auto c = static_cast<unsigned char>(text[i]);
        if (c == 0xC2 && i + 2 < text.size() &&
            static_cast<unsigned char>(text[i + 1]) == 0xA7) {
            length = 3;
        } else if (c == 0xA7 && i + 1 < text.size()) {
            length = 2;
        }
        if (length) { i += length; continue; }
        result.push_back(text[i++]);
    }
    return result;
}
inline bool containsIgnoreCase(std::string_view haystack, std::string_view needle) noexcept {
    if (needle.empty() || needle.size() > haystack.size()) return false;
    for (std::size_t i = 0; i + needle.size() <= haystack.size(); ++i) {
        bool equal = true;
        for (std::size_t j = 0; j < needle.size(); ++j) {
            if (std::tolower(static_cast<unsigned char>(haystack[i + j])) !=
                std::tolower(static_cast<unsigned char>(needle[j]))) { equal = false; break; }
        }
        if (equal) return true;
    }
    return false;
}
} // namespace detail

inline bool GameApi::playerMatch(void* actor, std::string_view plainTag,
                                 std::string* matched) const {
    if (!actor || plainTag.empty()) return false;
    auto* world = access_->worldOf(actor);
    if (!world) return false;
    const auto names = access_->playerListNames(world);
    std::string best;
    for (const auto& rawName : names) {
        const auto name = detail::stripFormatting(rawName);
        if (name.size() < 2 || !detail::containsIgnoreCase(plainTag, name)) continue;
        if (name.size() > best.size()) best = name;
    }
    if (best.empty()) return false;
    if (matched) *matched = std::move(best);
    return true;
}

inline bool Actor::isValid() const noexcept {
    return api_ && api_->validActor(handle_);
}
inline std::string Actor::getNameTag() const {
    if (!isValid()) return {};
    // Prefer live data so callers see a name/color change immediately; the
    // cache is a fallback for adapters that only observe the getter hook.
    if (auto tag = api_->access_->nameTag(handle_); tag && !tag->empty()) {
        api_->observeActorNameTag(handle_, *tag);
        return *tag;
    }
    if (auto cached = api_->cachedNameTag(handle_)) return *cached;
    return {};
}
inline std::string Actor::getName() const {
    const auto tag = getNameTag();
    const auto plain = detail::stripFormatting(tag);
    std::string playerName;
    if (isValid() && api_->playerMatch(handle_, plain, &playerName)) return playerName;
    return plain;
}
inline bool Actor::isPlayer() const {
    if (!isValid()) return false;
    return api_->playerMatch(handle_, detail::stripFormatting(getNameTag()));
}
inline World Actor::getWorld() const {
    return isValid() ? World{*api_, api_->access_->worldOf(handle_)} : World{};
}
inline std::vector<Actor> World::getActors() const {
    return isValid() ? api_->actors(handle_) : std::vector<Actor>{};
}
inline std::vector<std::string> World::getPlayerNames() const {
    return isValid() ? api_->access_->playerListNames(handle_) : std::vector<std::string>{};
}
inline LocalPlayer::LocalPlayer(GameApi& api) noexcept
    : Actor{api, api.access_->localPlayer()} {}
inline std::optional<LocalPlayer> LocalPlayer::forActor(const Actor& actor) noexcept {
    if (!actor.isValid() || actor.api_->access_->localPlayer() != actor.handle_)
        return std::nullopt;
    return LocalPlayer{*actor.api_};
}
inline Keyboard LocalPlayer::getKeyboard() const noexcept {
    return Keyboard{api_};
}
inline Mouse LocalPlayer::getMouse() const noexcept {
    return Mouse{api_};
}
inline std::optional<Server> LocalPlayer::getServer() const {
    if (!isValid()) return std::nullopt;
    auto endpoint = api_->access_->serverAddress(handle_);
    if (!endpoint || !endpoint->valid()) return std::nullopt;
    return Server{std::move(*endpoint)};
}
inline bool LocalPlayer::turn(float yawDelta, float pitchDelta) const noexcept {
    return isValid() && api_->access_->turn(yawDelta, pitchDelta);
}

inline bool Keyboard::isDown(unsigned key) const noexcept {
    return api_ && api_->access().keyDown(key);
}
inline bool Keyboard::press(unsigned key) const noexcept {
    return api_ && (api_->sendNativeKey(key, true) ||
                    api_->access().sendKey(key, true));
}
inline bool Keyboard::release(unsigned key) const noexcept {
    return api_ && (api_->sendNativeKey(key, false) ||
                    api_->access().sendKey(key, false));
}
inline bool Mouse::isDown(MouseButton button) const noexcept {
    return api_ && api_->access().mouseDown(button);
}
inline bool Mouse::setDown(MouseButton button, bool down) const noexcept {
    return api_ && api_->sendMouseInput(static_cast<unsigned>(button), down);
}
inline bool Mouse::click(MouseButton button) const noexcept {
    if (!api_) return false;
    if (api_->sendMouseInput(static_cast<unsigned>(button), true))
        return api_->sendMouseInput(static_cast<unsigned>(button), false);
    return api_->access().click(button);
}
inline bool Mouse::move(int deltaX, int deltaY) const noexcept {
    return api_ && api_->access().moveMouse(deltaX, deltaY);
}

} // namespace bedrock
