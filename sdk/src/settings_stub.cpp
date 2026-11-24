#include <bedrock/detail/oreui_settings_backend.hpp>
namespace bedrock::detail::oreui_settings {
Result registerPage(const PageDescriptor&, const RuntimeProfile*) noexcept { return Result::Unavailable; }
Result unregisterPage(const char*) noexcept { return Result::Unavailable; }
Result readValue(const char*, const char*, RawValue&, char*, std::uint32_t) noexcept {
    return Result::Unavailable;
}
}
