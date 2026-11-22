#include "bedrock/sdk/overlay.hpp"
#ifdef _WIN32
#include <bedrock/render/gui_overlay.hpp>
#endif
#include <memory>
#include <string>
#include <utility>

namespace bedrock::sdk {

struct Overlay::Impl {
#ifdef _WIN32
    void (*drawCallback)(void* userdata) = nullptr;
    void* userdata = nullptr;
    bool installed = false;
#endif
};

Overlay::Overlay() : impl_(std::make_unique<Impl>()) {}
Overlay::~Overlay() = default;
Overlay::Overlay(Overlay&&) noexcept = default;
Overlay& Overlay::operator=(Overlay&&) noexcept = default;

bool Overlay::install(void (*drawCallback)(void* userdata),
                      void* userdata,
                      const OverlayOptions& options,
                      std::string& error) {
#ifdef _WIN32
    try {
        impl_->drawCallback = drawCallback;
        impl_->userdata = userdata;
        
        bedrock::gui::OverlayOptions nativeOptions;
        nativeOptions.toggleVirtualKey = options.toggleVirtualKey;
        nativeOptions.initiallyVisible = options.initiallyVisible;
        nativeOptions.captureInput = options.captureInput;
        nativeOptions.nativeCursor = options.nativeCursor;
        nativeOptions.requireMinecraftProcess = true;
        
        auto trampoline = [](void* userdata) {
            auto* self = static_cast<Overlay*>(userdata);
            if (self && self->impl_->drawCallback) {
                self->impl_->drawCallback(self->impl_->userdata);
            }
        };
        
        if (!bedrock::gui::Overlay::install(trampoline, this, error, nativeOptions)) {
            return false;
        }
        impl_->installed = true;
        return true;
    } catch (...) {
        error = "exception in Overlay::install";
        return false;
    }
#else
    (void)drawCallback; (void)userdata; (void)options;
    error = "native Overlay unavailable on this host";
    return false;
#endif
}

void Overlay::uninstall() noexcept {
#ifdef _WIN32
    if (!impl_->installed) return;
    bedrock::gui::Overlay::shutdown();
    impl_->installed = false;
    impl_->drawCallback = nullptr;
    impl_->userdata = nullptr;
#else
    (void)impl_;
#endif
}

} // namespace bedrock::sdk