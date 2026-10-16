#include "bedrock/client_instance_tracker.hpp"
#include <cassert>
#include <iostream>

struct State { void* allowed{}; };
static bool check(const void* p, void* opaque) noexcept {
    return p == static_cast<State*>(opaque)->allowed;
}
int main() {
    int a=12,b=22;
    State state{&a};
    bedrock::ClientInstanceTracker tracker(check, &state);
    assert(tracker.get()==nullptr);
    assert(!tracker.observe(nullptr));
    assert(!tracker.observe(&b));
    assert(tracker.captures()==0);
    assert(tracker.observe(&a));
    assert(tracker.get()==&a);
    assert(tracker.captures()==1);
    state.allowed=&b;
    assert(tracker.get()==nullptr); // Previous pointer no longer valid.
    assert(tracker.observe(&b));
    assert(tracker.get()==&b);
    tracker.clear();
    assert(tracker.get()==nullptr);
    assert(tracker.captures()==2);
    std::cout << "client_instance_tracker_test: PASS\n";
}
