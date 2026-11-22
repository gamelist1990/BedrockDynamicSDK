#include <bedrock/sdk.hpp>
#include <bedrock/sdk/logging.hpp>
#include <mutex>
#include <stdexcept>

namespace dynamic_client {
namespace {
std::mutex mutex;
bedrock::sdk::BrightnessOverride brightness;
bedrock::sdk::Logger* brightnessLog{};
float gamma = 25.f;
bool enabled = false;

void apply() {
    if (!brightnessLog) return;
    const auto result = brightness.setGamma(enabled ? std::optional{gamma} : std::nullopt);
    if (!result) brightnessLog->error("Fullbright: ", result.error().message);
}
}

void setBrightness(float value, bool on) {
    std::lock_guard<std::mutex> lock(mutex);
    gamma = value;
    enabled = on;
    apply(); // Owner-scoped, synchronized override; no game objects are changed here.
}

void startBrightness(const bedrock::sdk::ClientHost& host, bedrock::sdk::Logger& logger) {
    std::lock_guard<std::mutex> lock(mutex);
    const auto connected = brightness.connect(host);
    if (!connected) throw std::runtime_error(connected.error().message);
    brightnessLog = &logger;
    apply();
    logger.info("Fullbright ready");
}
}