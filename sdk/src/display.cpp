#include <bedrock/sdk/display.hpp>

#include <cmath>
#include <mutex>

#ifdef BEDROCK_STAGE_A_NATIVE_DISPLAY
#include <bedrock/sdk/client_host.hpp>
#include <bedrock/native/brightness.hpp>
#include <bedrock/profiles/profile_manager.hpp>
#endif

namespace bedrock::sdk {

struct BrightnessOverride::Impl {
    std::mutex mutex;
#ifdef BEDROCK_STAGE_A_NATIVE_DISPLAY
    NativeBrightness brightness;
#endif
};

BrightnessOverride::BrightnessOverride() : impl_(std::make_unique<Impl>()) {}
BrightnessOverride::~BrightnessOverride() = default;

Result<void> BrightnessOverride::connect(const ClientHost& host) {
#ifdef BEDROCK_STAGE_A_NATIVE_DISPLAY
    std::lock_guard lock(impl_->mutex);
    if (impl_->brightness.ready()) return Result<void>::ok();
    const auto profile = host.profileSnapshot();
    if (!profile)
        return Result<void>::error(ErrorCode::Unavailable, "Brightness requires an active client host");
    std::string error;
    if (impl_->brightness.load(*profile, error) && impl_->brightness.install(error))
        return Result<void>::ok();
    return Result<void>::error(ErrorCode::Unavailable, std::move(error));
#else
    (void)host;
    return Result<void>::error(ErrorCode::Unavailable, "Native brightness is not available");
#endif
}

Result<void> BrightnessOverride::setGamma(std::optional<float> gamma) {
    if (gamma && (!std::isfinite(*gamma) || *gamma < 0 || *gamma > 25))
        return Result<void>::error(ErrorCode::Invalid, "Gamma must be finite and in 0..25");
#ifdef BEDROCK_STAGE_A_NATIVE_DISPLAY
    std::lock_guard lock(impl_->mutex);
    if (impl_->brightness.ready()) {
        if (!gamma) {
            impl_->brightness.reset();
            return Result<void>::ok();
        }
        if (impl_->brightness.setGamma(Gamma{*gamma})) return Result<void>::ok();
    }
#endif
    return Result<void>::error(ErrorCode::Unavailable, "Brightness override is not connected");
}

} // namespace bedrock::sdk
