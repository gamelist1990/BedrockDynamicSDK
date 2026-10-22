#include <bedrock/game_api.hpp>

#include <cassert>
#include <unordered_map>

namespace {
struct FakeGame final : bedrock::GameAccess {
    void* local = reinterpret_cast<void*>(0x10);
    void* worldHandle = reinterpret_cast<void*>(0x20);
    std::unordered_map<void*, std::string> tags{
        {local, "\xC2\xA7" "a[VIP] Alex\xC2\xA7" "7 Level 12"},
        {reinterpret_cast<void*>(0x30), "\xC2\xA7" "pOtherPlayer"}};
    bool keyState{};
    unsigned sentKey{};
    unsigned mouseClicks{};
    float yaw{}, pitch{};

    bool isActorValid(void* actor) const noexcept override { return tags.contains(actor); }
    void* localPlayer() const noexcept override { return local; }
    void* worldOf(void* actor) const noexcept override {
        return isActorValid(actor) ? worldHandle : nullptr;
    }
    std::optional<std::string> nameTag(void* actor) const override {
        const auto it = tags.find(actor);
        return it == tags.end() ? std::nullopt : std::optional<std::string>{it->second};
    }
    std::vector<std::string> playerListNames(void*) const override {
        return {"Alex", "OtherPlayer"};
    }
    std::optional<bedrock::ServerAddress> serverAddress(void*) const override {
        return bedrock::ServerAddress{"play.example.net", 19132};
    }
    bool keyDown(unsigned key) const noexcept override { return key == sentKey && keyState; }
    bool sendKey(unsigned key, bool pressed) noexcept override {
        sentKey = key; keyState = pressed; return true;
    }
    bool click(bedrock::MouseButton) noexcept override { ++mouseClicks; return true; }
    bool turn(float yawDelta, float pitchDelta) noexcept override {
        yaw += yawDelta; pitch += pitchDelta; return true;
    }
};
} // namespace

int main() {
    FakeGame game;
    bedrock::GameApi api{game};
    unsigned actorEvents = 0;
    unsigned viewEvents = 0;
    api.Event.Enum.ActorNameTag.then([&](const auto& event) {
        assert(event.actor != nullptr && !event.nameTag.empty());
        ++actorEvents;
    });
    api.Event.Enum.ViewNameTag.then([&](const auto& event) {
        assert(event.actor != nullptr && !event.nameTag.empty());
        ++viewEvents;
    });

    auto self = api.localPlayer();
    assert(self.isValid());
    api.observeActorNameTag(game.local, game.tags.at(game.local));
    assert(self.getName() == "Alex");
    assert(self.isPlayer());
    assert(self.getWorld().isValid());
    assert(api.world(game.worldHandle).getPlayerNames().size() == 2);
    const auto server = self.server();
    assert(server && server->ip() == "play.example.net");
    assert(server->port() == 19132 && server->address() == "play.example.net:19132");
    assert(bedrock::LocalPlayer::forActor(api.actor(game.local)).has_value());
    assert(!bedrock::LocalPlayer::forActor(api.actor(reinterpret_cast<void*>(0x30))));

    const auto other = reinterpret_cast<void*>(0x30);
    api.observeViewNameTag(other, game.tags.at(other));
    assert(actorEvents == 2 && viewEvents == 1);
    const auto viewTags = api.cachedActorNameTags(bedrock::NameTagSource::View);
    const auto getterTags = api.cachedActorNameTags(bedrock::NameTagSource::ActorGetter);
    assert(viewTags->size() == 1 && viewTags->front().actor == other);
    assert(getterTags->size() == 1 && getterTags->front().actor == game.local);
    const auto cachedViewActors = api.cachedActors(bedrock::NameTagSource::View);
    const auto cachedGetterActors = api.cachedActors(bedrock::NameTagSource::ActorGetter);
    assert(cachedViewActors->size() == 1 && cachedGetterActors->size() == 1);
    const auto actors = self.getWorld().getActors();
    assert(actors.size() == 2);
    assert(api.actor(other).getName() == "OtherPlayer");
    assert(actorEvents == 3);
    assert(api.cachedActorNameTags(bedrock::NameTagSource::ActorGetter)->size() == 2);

    // Explicit reads prefer the live adapter and replace cached color/name
    // data so PlayerList consumers do not get stuck on an old nametag.
    game.tags[other] = "\xC2\xA7" "cOtherPlayer";
    assert(api.refreshCachedActorNameTags(16) > 0);
    assert(api.actor(other).getNameTag() == "\xC2\xA7" "cOtherPlayer");
    assert(actorEvents == 5);
    assert(api.cachedActorNameTags(bedrock::NameTagSource::View)->front().nameTag ==
           "\xC2\xA7" "cOtherPlayer");

    auto keyboard = self.getKeyboard();
    assert(keyboard.press(87) && keyboard.isDown(87));
    assert(keyboard.release(87) && !keyboard.isDown(87));
    assert(keyboard.tap(32));
    assert(self.getMouse().click(bedrock::MouseButton::Left));
    assert(game.mouseClicks == 1);
    assert(self.turn(3.0f, -2.0f) && game.yaw == 3.0f && game.pitch == -2.0f);

    api.Event.Enum.Keyboard.then([](const bedrock::KeyEvent& event) {
        if (event.virtualKey == 65) event.cancel();
    });
    bedrock::KeyEvent key{65, true, false};
    assert(!api.dispatchKey(key));
    bedrock::KeyEvent passThrough{66, true, false};
    assert(api.dispatchKey(passThrough));
}
