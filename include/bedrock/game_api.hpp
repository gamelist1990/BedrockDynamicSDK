#pragma once

// Bukkit-inspired, version-independent client API. Minecraft memory access is
// deliberately isolated behind GameAccess: wrappers never guess offsets or
// call an unverified game method themselves.
#include <bedrock/events.hpp>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <mutex>
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

enum class MouseButton : unsigned { Left = 0, Right = 1, Middle = 2, X1 = 3, X2 = 4 };

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

struct GameEventRegistry {
    struct EnumType {
        EventChannel<KeyEvent> Keyboard;
        EventChannel<MouseEvent> Mouse;
        EventChannel<ActorNameTagEvent> ActorNameTag;
        EventChannel<ViewNameTagEvent> ViewNameTag;
        EventChannel<UpdateEvent> Update;
        EventChannel<GameplayEvent> Gameplay;
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
    explicit Keyboard(GameAccess* access) noexcept : access_(access) {}
    [[nodiscard]] bool isDown(unsigned key) const noexcept {
        return access_ && access_->keyDown(key);
    }
    bool press(unsigned key) const noexcept { return access_ && access_->sendKey(key, true); }
    bool release(unsigned key) const noexcept { return access_ && access_->sendKey(key, false); }
    bool tap(unsigned key) const noexcept {
        if (!press(key)) return false;
        return release(key);
    }
private:
    GameAccess* access_{};
};

class Mouse {
public:
    Mouse() = default;
    explicit Mouse(GameAccess* access) noexcept : access_(access) {}
    [[nodiscard]] bool isDown(MouseButton button) const noexcept {
        return access_ && access_->mouseDown(button);
    }
    bool click(MouseButton button) const noexcept { return access_ && access_->click(button); }
    bool move(int deltaX, int deltaY) const noexcept {
        return access_ && access_->moveMouse(deltaX, deltaY);
    }
private:
    GameAccess* access_{};
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

class GameApi {
public:
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
        viewActors_.insert(actor);
    }
    void clearObservedActors() {
        std::lock_guard lock(mutex_);
        nameTags_.clear();
        viewNameTags_.clear();
        viewActors_.clear();
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
        }
        const bool actorChanged = rememberActor(actor, nameTag);
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
        try { Event.Enum.Gameplay.emit(event); } catch (...) {}
    }

    [[nodiscard]] std::vector<Actor> actors(void* world = nullptr) {
        std::unordered_set<void*> handles;
        {
            std::lock_guard lock(mutex_);
            handles = viewActors_;
            for (const auto& [actor, _] : nameTags_) handles.insert(actor);
        }
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
    bool rememberActor(void* actor, std::string_view tag) {
        std::lock_guard lock(mutex_);
        const auto it = nameTags_.find(actor);
        const bool changed = it == nameTags_.end() || it->second != tag;
        nameTags_.insert_or_assign(actor, std::string(tag));
        return changed;
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
    mutable std::mutex mutex_;
    std::unordered_map<void*, std::string> nameTags_;
    std::unordered_map<void*, std::string> viewNameTags_;
    std::unordered_set<void*> viewActors_;
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
    return Keyboard{api_ ? api_->access_ : nullptr};
}
inline Mouse LocalPlayer::getMouse() const noexcept {
    return Mouse{api_ ? api_->access_ : nullptr};
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

} // namespace bedrock
