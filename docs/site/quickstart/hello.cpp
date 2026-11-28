#include <bedrock/sdk.hpp>

namespace sdk = bedrock::sdk;
namespace {
class Hello final : public sdk::Mod {
public:
    Hello() : Mod("hello") {}
private:
    void onEnable() override {
        sdk::ContentLog::write(sdk::LogLevel::Info, "Hello", "Hello!");
    }
};

Hello mod;
sdk::ClientHost host;
}

extern "C" __declspec(dllexport) bool startMod() {
    return static_cast<bool>(host.start(mod));
}

extern "C" __declspec(dllexport) bool stopMod() {
    return static_cast<bool>(host.stop());
}
