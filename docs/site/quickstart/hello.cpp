#include <bedrock/sdk.hpp>

namespace sdk = bedrock::sdk;
static class Hello final : public sdk::Mod {
public:
    Hello() : Mod("hello") {}
private:
    void onEnable() override {
        sdk::ContentLog::write(sdk::LogLevel::Info, "Hello", "Hello!");
    }
} mod;
static sdk::ClientHost host;

extern "C" __declspec(dllexport) bool startMod() { return bool(host.start(mod)); }
extern "C" __declspec(dllexport) bool stopMod() { return bool(host.stop()); }
