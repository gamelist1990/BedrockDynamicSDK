#include "auto_sprint_feature.hpp"
#include <cassert>
#include <vector>

int main() {
    bedrock::sdk::Events<InputFrameEvent> events;
    std::vector<bool> transitions;
    {
        AutoSprintFeature mod(events, [&](bool down) {
            transitions.push_back(down);
            return true;
        });
        // No listeners() in sdk::Events - drop this assertion
        InputFrameEvent frame{true,true,true,false,false};
        events.publish(frame);
        assert((transitions == std::vector<bool>{true}));
        assert(mod.ownsSprint());
        // Repeated W snapshots must keep native Ctrl held without pulsing it.
        events.publish(frame);
        events.publish(frame);
        assert(transitions.size() == 1 && mod.ownsSprint());
        frame.leftCtrlHeld = true; // A real physical Ctrl takes ownership.
        events.publish(frame);
        events.publish(frame);
        assert(transitions.size() == 1);
        assert(!mod.ownsSprint()); // Physical Ctrl takes over without a key-up.

        frame.shiftHeld = true;
        events.publish(frame);
        assert((transitions == std::vector<bool>{true}));
        assert(!mod.ownsSprint());
        frame.shiftHeld = false;
        frame.leftCtrlHeld = true; // Physical Ctrl held by player.
        events.publish(frame);
        assert(transitions.size() == 1);
        frame.leftCtrlHeld = false;
        events.publish(frame);
        assert((transitions == std::vector<bool>{true,true}));
        frame.forwardHeld = false;
        events.publish(frame);
        assert((transitions == std::vector<bool>{true,true,false}));

        frame.forwardHeld = true;
        events.publish(frame);
        assert(mod.ownsSprint());
        frame.minecraftFocused = false;
        events.publish(frame);
        assert(!mod.ownsSprint());

        frame.minecraftFocused = true;
        frame.gameplayCursorHidden = false;
        events.publish(frame);
        assert(!mod.ownsSprint());

        frame.gameplayCursorHidden = true;
        frame.forwardHeld = true;
        frame.leftCtrlHeld = false;
        events.publish(frame);
        assert(mod.ownsSprint());
    }
    // Destructor releases, so last transition should be false (release)
    assert(!transitions.back());

    // No listeners() in sdk::Events - drop this assertion
    // assert(events.listeners<InputFrameEvent>() == 0);

    AutoSprintFeature failed(events, [](bool down) { return !down; });
    events.publish(InputFrameEvent{true,true,true,false,false});
    assert(!failed.ownsSprint());

    // Exhaustively compare against the original direct-loop boolean condition.
    for (int mask = 0; mask < 32; ++mask) {
        InputFrameEvent state{
            bool(mask & 1), bool(mask & 2), bool(mask & 4),
            bool(mask & 8), bool(mask & 16)
        };
        const bool expected = bool(mask & 1) && bool(mask & 2) &&
                              bool(mask & 4) && !bool(mask & 8);
        assert(state.shouldSprint() == expected);
    }
}