#include <bedrock/native/keyboard.hpp>
#include <bedrock/generated/symbols.hpp>
#include <bedrock/runtime/hook_runtime.h>
#include <nlohmann/json.hpp>
#include <array>
#include <cassert>
#include <cstring>
#include <utility>
#include <vector>

namespace {
struct Processor {
    void** table{};
    std::vector<std::pair<int, bool>> transitions;
    int origin{};
};
void BDS_HOOK_CALL down(void* self, int key, int origin) {
    auto& p = *static_cast<Processor*>(self);
    p.transitions.emplace_back(key, true);
    p.origin = origin;
}
void BDS_HOOK_CALL up(void* self, int key) {
    static_cast<Processor*>(self)->transitions.emplace_back(key, false);
}
template<class T, std::size_t N>
void put(std::array<std::byte, N>& bytes, std::size_t offset, T value) {
    assert(offset + sizeof(value) <= bytes.size());
    std::memcpy(bytes.data() + offset, &value, sizeof(value));
}
}
int main() {
    std::string error;
    const auto layout = bedrock::parseNativeKeyboardLayout(
        bedrock::generated::kSymbolDatabase, error);
    assert(layout && error.empty());
    std::array<void*, 3> table{
        nullptr, reinterpret_cast<void*>(&down), reinterpret_cast<void*>(&up)};
    Processor processor{table.data()};
    std::array<std::byte, 72> owner{};
    std::array<std::byte, 32> context{};
    put(owner, layout->processorOffset, &processor);
    put(context, layout->ownerOffset, owner.data());
    put(context, layout->messageOffset, std::uint32_t{0x100});
    put(context, layout->keyOffset, int{'W'});
    auto event = bedrock::captureNativeKeyboardEvent(context.data(), *layout);
    assert(event && event->key == 'W' && event->down);
    // Observing/prefiltering a key does not send it to gameplay. The native
    // route must invoke the actual processor, including the InputOrigin ABI.
    assert(processor.transitions.empty());
    assert(event->input.send(17, true));
    assert(event->input.send(17, false));
    assert((processor.transitions ==
        std::vector<std::pair<int,bool>>{{17,true},{17,false}}));
    assert(processor.origin == 1);
    assert(!event->input.send(256, true));
    assert(!bedrock::NativeKeyboardInput{}.send(17, true));

    put(context, layout->messageOffset, std::uint32_t{0x105});
    event = bedrock::captureNativeKeyboardEvent(context.data(), *layout);
    assert(event && !event->down);
    put(context, layout->messageOffset, std::uint32_t{0x200});
    assert(!bedrock::captureNativeKeyboardEvent(context.data(), *layout));
    assert(!bedrock::captureNativeKeyboardEvent(nullptr, *layout));

    auto profile = nlohmann::json::parse(bedrock::generated::kSymbolDatabase);
    profile["layouts"].erase("GameCore");
    assert(!bedrock::parseNativeKeyboardLayout(profile.dump(), error));
    profile = nlohmann::json::parse(bedrock::generated::kSymbolDatabase);
    profile["layouts"]["GameCore"]["owner_offset"] = -1;
    assert(!bedrock::parseNativeKeyboardLayout(profile.dump(), error));
    profile["layouts"]["GameCore"]["owner_offset"] = 0;
    profile["layouts"]["GameCore"]["key_down_vtable_slot"] = 100;
    assert(!bedrock::parseNativeKeyboardLayout(profile.dump(), error));
}
