#include <bedrock/profiles/profile_json.hpp>
#include <bedrock/native/keyboard.hpp>
#include <nlohmann/json.hpp>
#include <cstring>
#include <stdexcept>

namespace bedrock {
namespace {
template<class T> T readAt(const void* base, std::size_t offset) {
    T value{};
    std::memcpy(&value, static_cast<const std::byte*>(base) + offset, sizeof(T));
    return value;
}
}
std::optional<NativeKeyboardLayout> parseNativeKeyboardLayout(
    std::string_view profile, std::string& error) {
    try {
        const auto root = bedrock::parseNativeProfile(profile);
        const auto& value = root.at("native_keyboard");
        if (value.at("schema") != 1 || value.at("pointer_size") != sizeof(void*) ||
            value.at("callback_symbol") != "GameCore::keyboardEvent")
            throw std::runtime_error("Unsupported native keyboard layout");
        const auto bounded = [&](const char* name, std::size_t maximum) {
            if (!value.at(name).is_number_integer())
                throw std::runtime_error("Native keyboard layout fields must be integers");
            const auto number = value.at(name).get<std::int64_t>();
            if (number < 0 || static_cast<std::uint64_t>(number) > maximum)
                throw std::runtime_error("Invalid native keyboard layout offset");
            return static_cast<std::size_t>(number);
        };
        NativeKeyboardLayout layout{
            bounded("owner_offset", 256), bounded("processor_offset", 4096),
            bounded("message_offset", 256), bounded("key_offset", 256),
            bounded("key_down_vtable_slot", 64), bounded("key_up_vtable_slot", 64),
            static_cast<int>(bounded("hardware_origin", 16))};
        if (layout.keyDownSlot == layout.keyUpSlot)
            throw std::runtime_error("Native keyboard slots must be distinct");
        error.clear();
        return layout;
    } catch (const std::exception& e) {
        error = e.what();
        return std::nullopt;
    }
}
std::optional<NativeKeyboardEvent> captureNativeKeyboardEvent(
    const void* context, const NativeKeyboardLayout& layout) {
    if (!context) return std::nullopt;
    const auto message = readAt<std::uint32_t>(context, layout.messageOffset);
    const auto key = readAt<int>(context, layout.keyOffset);
    if ((message & ~4u) != 0x100 && (message & ~4u) != 0x101)
        return std::nullopt;
    if (key < 0 || key >= 256) return std::nullopt;
    const auto owner = readAt<void*>(context, layout.ownerOffset);
    if (!owner) return std::nullopt;
    const auto processor = readAt<void*>(owner, layout.processorOffset);
    if (!processor) return std::nullopt;
    const auto table = readAt<void*>(processor, 0);
    if (!table) return std::nullopt;
    NativeKeyboardInput input{
        processor,
        readAt<NativeKeyboardInput::KeyDown>(table, layout.keyDownSlot * sizeof(void*)),
        readAt<NativeKeyboardInput::KeyUp>(table, layout.keyUpSlot * sizeof(void*)),
        layout.hardwareOrigin};
    if (!input.keyDown || !input.keyUp) return std::nullopt;
    return NativeKeyboardEvent{input, key, (message & ~4u) == 0x100};
}
} // namespace bedrock
