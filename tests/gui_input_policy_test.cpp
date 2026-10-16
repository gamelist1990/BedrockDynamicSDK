#include <bedrock/gui_input_policy.hpp>
#include <cassert>

int main() {
    using bedrock::gui::shouldBlockGameMouse;
    for (unsigned bits=0; bits<32; ++bits) {
        const bool running = (bits & 1) != 0;
        const bool visible = (bits & 2) != 0;
        const bool capture = (bits & 4) != 0;
        const bool focused = (bits & 8) != 0;
        const bool ready = (bits & 16) != 0;
        // Fail-open on hidden/unfocused/not initialized; otherwise exclusive.
        assert(shouldBlockGameMouse(running,visible,capture,focused,ready)
               == (bits==31));
    }
}
