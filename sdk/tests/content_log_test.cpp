#include <bedrock/sdk.hpp>

// Plain C++ mod proof: only the SDK umbrella and bedrock::sdk are needed.
class ContentLogProbe final : public bedrock::sdk::Mod {
public:
    ContentLogProbe() : Mod("ContentLogProbe") {}
    bool writerAvailable{};

private:
    void onEnable() override {
        using namespace bedrock::sdk;
        writerAvailable = ContentLog::available();
        // Safe to call even with no writer.
        ContentLog::write(LogLevel::Info, "ContentLogProbe", "Hello from C++");
        emitContentLog(LogLevel::Info, "ContentLogProbe", "Compatibility emitter");
    }
};

int main() {
    ContentLogProbe mod;
    if (!mod.enable() || mod.writerAvailable) return 1;
    return mod.disable() ? 0 : 1;
}
