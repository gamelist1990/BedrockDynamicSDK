#include <bedrock/client.hpp>
#include <bedrock/mod.hpp>
#include <cassert>
#include <iostream>
int main() {
    using bedrock::shouldSprint;
    assert(shouldSprint(true, true, false));
    assert(!shouldSprint(false, true, false));
    assert(!shouldSprint(true, false, false));
    assert(!shouldSprint(true, true, true));
    bedrock::ModRuntime runtime;
    assert(runtime.status() == bedrock::ModStatus::NotStarted);
    runtime.stop();
    std::cout << "client_api_test: PASS\n";
}
