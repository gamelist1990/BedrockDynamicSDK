#include <bedrock/sdk.hpp>
#include <bedrock/sdk/vanilla.hpp>
#include <cassert>
#include <stdexcept>
#include <type_traits>

struct Tick { int value; };

struct TestMod final : bedrock::sdk::Mod {
    TestMod() : Mod("test-mod") {}
    int enables{}, disables{}, updates{}, runs{}, intervals{};
    void onEnable() override {
        ++enables;
        system().run([this] {
            ++runs;
            system().run([this] { ++runs; });
        });
        system().runTimeout([] { throw std::runtime_error("task failed"); }, 2);
        interval = system().runInterval([this] {
            ++intervals;
            assert(system().clearRun(interval));
        }, 2);
    }
    void onUpdate() override { ++updates; }
    void onDisable() override { ++disables; }
    bedrock::sdk::System::TaskId interval{};
};

int main() {
    namespace vanilla = bedrock::sdk::vanilla;
    static_assert(vanilla::MinecraftBlockTypes::Glass.value() == "minecraft:glass");
    static_assert(vanilla::MinecraftItemTypes::Diamond.value() == "minecraft:diamond");
    static_assert(vanilla::MinecraftEntityTypes::Zombie.value() == "minecraft:zombie");
    static_assert(vanilla::MinecraftEntityTypes::Item.value() == "minecraft:item");
    static_assert(!std::is_same_v<vanilla::BlockId, vanilla::ItemId>);

    bedrock::sdk::BrightnessOverride brightness;
    const auto badGamma = brightness.setGamma(26.f);
    assert(!badGamma && badGamma.error().code == bedrock::sdk::ErrorCode::Invalid);
    const auto unconnected = brightness.setGamma(25.f);
    assert(!unconnected && unconnected.error().code == bedrock::sdk::ErrorCode::Unavailable);

    bedrock::sdk::Events<Tick> ticks;
    int observed = 0;
    auto subscription = ticks.subscribe([&](const Tick& tick) { observed += tick.value; });
    ticks.publish({3});
    assert(observed == 3);
    subscription.reset();
    ticks.publish({4});
    assert(observed == 3);
    bedrock::sdk::Subscription later;
    {
        bedrock::sdk::EventSignal<int&, int> targets;
        later = targets += [](int& value, int amount) { value += amount; };
        targets.publish(observed, 2);
        assert(observed == 5);
    }
    later.reset();

    TestMod mod;
    assert(!mod.update());
    assert(mod.enable());
    assert(mod.enable() && mod.enables == 1);
    assert(mod.update() && mod.runs == 1);
    assert(!mod.update() && mod.runs == 2 && mod.intervals == 1 && mod.updates == 2);
    assert(mod.update() && mod.intervals == 1);
    assert(mod.disable() && mod.disables == 1);
    assert(mod.disable() && mod.disables == 1);

    using bedrock::sdk::Key;
    bedrock::sdk::KeyBindings bindings("test");
    assert(bindings.add("zoom", "key.zoom", Key::C));
    assert(!bindings.add("zoom", "key.other", Key::X));
    assert(!bindings.add("invalid", "key.invalid", static_cast<Key>(27)));
    int keyEvents = 0;
    auto keySubscription = bindings.changed += [&](const auto& event) {
        assert(event.id == "zoom");
        ++keyEvents;
    };
    assert(bindings.dispatch(Key::C, true).size() == 1);
    assert(bindings.dispatch(Key::C, true).empty());
    assert(bindings.dispatch(Key::C, false).size() == 1);
    assert(keyEvents == 2);

    auto page = bedrock::sdk::SettingsPage{"stage-a-test", "settings.title"};
    page.add(bedrock::sdk::Setting::toggle("enabled", "settings.enabled", true))
        .add(bedrock::sdk::Setting::slider("speed", "settings.speed", 2.0, 0.5, 8.0, 0.5))
        .locale("en_US", {{"settings.title", "Stage A"}, {"settings.enabled", "Enabled"}})
        .locale("ja_JP", {{"settings.title", "Stage A 設定"}, {"settings.enabled", "有効"}});
    assert(page.id() == "stage-a-test");
    auto invalid = page.value("enabled");
    assert(!invalid);
    return 0;
}
