#include <bedrock/detail/actor_identity.hpp>
#include <bedrock/detail/hook_dispatch.hpp>
#include <cassert>
#include <stdexcept>

namespace {
struct FakeClient final : bedrock::ClientAccess {
    void* actor = this;
    void* client{};
    unsigned begins{}, ends{};
    bool allowTick = true, inTick = false;

    void observeClient(void* value) noexcept override { client = value; }
    bool beginWorldTick(void*) noexcept override {
        ++begins;
        return inTick = allowTick;
    }
    void endWorldTick() noexcept override { assert(inTick); inTick = false; ++ends; }
    bool isActorValid(void* value) const noexcept override { return value == actor; }
    void* localPlayer() const noexcept override { return actor; }
    void* worldOf(void*) const noexcept override { return actor; }
    std::optional<std::string> nameTag(void*) const override {
        return "\xC2\xA7" "a[VIP] Alex Smith";
    }
    std::vector<std::string> playerListNames(void*) const override {
        return {"Alex", "Alex Smith"};
    }
    std::optional<bedrock::ServerAddress> serverAddress(void*) const override {
        return std::nullopt;
    }
};
}

int main() {
    using namespace bedrock;
    FakeClient access;
    detail::HookDispatch dispatch{access};
    unsigned ticks = 0;
    dispatch.on<LevelTickEvent>([&](const auto&) {
        assert(access.inTick);
        ++ticks;
        throw std::runtime_error("listener");
    });
    dispatch.dispatchLevelTick(access.actor, 1);
    assert(ticks == 1 && !access.inTick && access.begins == 1 && access.ends == 1);
    access.allowTick = false;
    dispatch.dispatchLevelTick(access.actor, 2);
    assert(ticks == 1 && !access.inTick && access.begins == 2 && access.ends == 1);

    dispatch.on<GameplayEvent>([&](const auto& event) {
        assert(dispatch.gameplayActive() == event.active && access.client == event.clientInstance);
    });
    dispatch.dispatchGameplay({true, access.actor});
    dispatch.dispatchGameplay({false, access.actor});
    dispatch.on<KeyEvent>([](const auto& event) { event.cancel(); });
    KeyEvent key{65, true, false};
    assert(!dispatch.dispatchKey(key));

    unsigned tags = 0;
    dispatch.on<ActorNameTagEvent>([&](const auto& event) {
        assert(event.actor == access.actor && event.nameTag == "Alex");
        ++tags;
    });
    dispatch.observeActorNameTag(nullptr, "Alex");
    dispatch.observeActorNameTag(access.actor, "Alex");
    dispatch.observeActorNameTag(access.actor, "Alex");
    assert(tags == 1);
    dispatch.dispatchWorldChange(WorldChangeReason::DimensionChanged, access.client);
    dispatch.observeActorNameTag(access.actor, "Alex");
    assert(tags == 2);

    const auto identity = detail::readActorIdentity(access, access.actor);
    assert(identity.isPlayer && identity.name == "Alex Smith" && !identity.nameTag.empty());
    assert(detail::readActorIdentity(access, nullptr).name.empty());
}
