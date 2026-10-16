#include <bedrock/events.hpp>
#include <cassert>
#include <stdexcept>
#include <vector>

struct CustomEvent { int value; };

int main() {
    bedrock::EventBus<bedrock::UpdateEvent, bedrock::KeyEvent, CustomEvent> events;
    int ticks = 0;
    auto t = events.on<bedrock::UpdateEvent>([&](const auto& ev) {
        ticks += static_cast<int>(ev.sequence);
    });
    events.emit(bedrock::UpdateEvent{3});
    assert(ticks == 3);
    assert(events.off<bedrock::UpdateEvent>(t));
    events.emit(bedrock::UpdateEvent{1});
    assert(ticks == 3);

    std::vector<int> observed;
    using E = CustomEvent;
    unsigned long long later = 0;
    auto first = events.on<E>([&](const E& ev) {
        observed.push_back(ev.value);
        if (ev.value == 1) {
            later = events.on<E>([&](const E& x) { observed.push_back(x.value * 10); });
        }
    });
    events.emit(E{1});
    assert((observed == std::vector<int>{1}));
    events.emit(E{2});
    assert((observed == std::vector<int>{1, 2, 20}));
    assert(events.off<E>(first));
    events.emit(E{3});
    assert((observed == std::vector<int>{1, 2, 20, 30}));
    assert(events.off<E>(later));
    assert(events.listeners<E>() == 0);

    bool threw = false;
    try { events.on<bedrock::KeyEvent>(std::function<void(const bedrock::KeyEvent&)>{}); }
    catch (const std::invalid_argument&) { threw = true; }
    assert(threw);

    // Generic event types are not tied to SDK headers or an injected client.
    int value = 0;
    events.on<bedrock::KeyEvent>([&](const auto& e) { if (e.pressed) value = e.virtualKey; });
    events.emit(bedrock::KeyEvent{65, true, false});
    assert(value == 65);
}
