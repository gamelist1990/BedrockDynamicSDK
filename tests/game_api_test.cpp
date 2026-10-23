#include <bedrock/game_api.hpp>

#include <algorithm>
#include <array>
#include <cassert>
#include <unordered_map>

namespace {
struct FakeGame final : bedrock::GameAccess {
    void* local = reinterpret_cast<void*>(0x10);
    void* worldHandle = reinterpret_cast<void*>(0x20);
    std::unordered_map<void*, std::string> tags{
        {local, "\xC2\xA7" "a[VIP] Alex\xC2\xA7" "7 Level 12"},
        {reinterpret_cast<void*>(0x30), "\xC2\xA7" "pOtherPlayer"}};
    std::unordered_map<void*, bedrock::HeldItemInfo> heldItems{
        {local, {"minecraft:bow", "Bow", 1}}};
    bool keyState{};
    unsigned sentKey{};
    unsigned mouseClicks{};
    mutable unsigned heldItemReads{};
    mutable unsigned nameTagReads{};
    mutable unsigned playerNameReads{};
    float yaw{}, pitch{};

    bool isActorValid(void* actor) const noexcept override { return tags.contains(actor); }
    void* localPlayer() const noexcept override { return local; }
    void* worldOf(void* actor) const noexcept override {
        return isActorValid(actor) ? worldHandle : nullptr;
    }
    std::optional<std::string> nameTag(void* actor) const override {
        ++nameTagReads;
        const auto it = tags.find(actor);
        return it == tags.end() ? std::nullopt : std::optional<std::string>{it->second};
    }
    std::optional<bedrock::HeldItemInfo> heldItem(void* actor) const override {
        ++heldItemReads;
        const auto it = heldItems.find(actor);
        return it == heldItems.end() ? std::nullopt
                                     : std::optional<bedrock::HeldItemInfo>{it->second};
    }
    std::vector<std::string> playerListNames(void*) const override {
        ++playerNameReads;
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
    unsigned renderViewEvents = 0;
    std::uint64_t lastRenderFrame = 0;
    api.Event.Enum.ActorNameTag.then([&](const auto& event) {
        assert(event.actor != nullptr && !event.nameTag.empty());
        ++actorEvents;
    });
    api.Event.Enum.ViewNameTag.then([&](const auto& event) {
        assert(event.actor != nullptr && !event.nameTag.empty());
        ++viewEvents;
    });
    api.Event.Enum.RenderViewActor.then([&](const auto& event) {
        assert(event.viewObject != nullptr && event.actor != nullptr);
        lastRenderFrame = event.frame;
        ++renderViewEvents;
    });

    auto self = api.localPlayer();
    assert(self.isValid());
    assert(self.getWorld().getActors().size() == 1);
    api.observeActorNameTag(game.local, game.tags.at(game.local));
    assert(self.getName() == "Alex");
    const auto initialNameReads = game.nameTagReads;
    assert(self.isPlayer());
    assert(game.nameTagReads == initialNameReads); // repeated getter uses actor cache
    const auto heldItem = self.getHeldItem();
    assert(heldItem && heldItem->identifier == "minecraft:bow" && heldItem->count == 1);
    const auto repeatedHeldItem = self.getHeldItem();
    assert(repeatedHeldItem && repeatedHeldItem->identifier == heldItem->identifier);
    assert(game.heldItemReads == 1);
    api.dispatchLevelTick(game.worldHandle, 1);
    const auto refreshedHeldItem = self.getHeldItem();
    assert(refreshedHeldItem && refreshedHeldItem->identifier == heldItem->identifier);
    assert(game.heldItemReads == 2); // tick invalidates cached native snapshots
    assert(!api.actor(reinterpret_cast<void*>(0x30)).getHeldItem());
    assert(self.getWorld().isValid());
    assert(api.world(game.worldHandle).getPlayerNames().size() == 2);
    const auto playerNameReads = game.playerNameReads;
    (void)api.world(game.worldHandle).getPlayerNames();
    assert(game.playerNameReads == playerNameReads);
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

    // Render-view candidates are a distinct per-frame snapshot, deduplicated
    // and replaced on the next pass; they are not merged into world actors.
    std::array<void*, 3> visibleActors{game.local, other, other};
    const int viewObjectToken = 1;
    api.publishRenderViewActors(&viewObjectToken, 7, visibleActors);
    const auto visible = api.cachedRenderViewActors();
    assert(visible.viewObject == &viewObjectToken && visible.frame == 7);
    assert(visible.actors && visible.actors->size() == 2);
    assert(renderViewEvents == 2);
    std::array<void*, 1> nextVisible{other};
    api.publishRenderViewActors(&viewObjectToken, 8, nextVisible);
    const auto nextFrame = api.cachedRenderViewActors();
    assert(nextFrame.frame == 8 && nextFrame.actors->size() == 1);
    assert(renderViewEvents == 3 && lastRenderFrame == 8);
    assert(api.actors().size() == 2); // no source conflation

    // Explicit reads prefer the live adapter and replace cached color/name
    // data so PlayerList consumers do not get stuck on an old nametag.
    game.tags[other] = "\xC2\xA7" "cOtherPlayer";
    assert(api.refreshCachedActorNameTags(16) > 0);
    assert(api.actor(other).getNameTag() == "\xC2\xA7" "cOtherPlayer");
    assert(actorEvents == 4);
    const auto latestGetterTags = api.cachedActorNameTags(bedrock::NameTagSource::ActorGetter);
    assert(std::any_of(latestGetterTags->begin(), latestGetterTags->end(), [other](const auto& item) {
        return item.actor == other && item.nameTag == "\xC2\xA7" "cOtherPlayer";
    }));

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
