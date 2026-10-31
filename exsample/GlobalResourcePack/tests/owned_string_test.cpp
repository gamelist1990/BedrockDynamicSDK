#include <bedrock/detail/owned_msvc_string.hpp>
#include <cassert>
#include <array>
#include <cstring>

int main() {
    using namespace bedrock::detail;
    const OwnedMsvcStringLayout layout{16, 24, 16, 512, 4096, 39};
    std::array<std::byte, 32> object{};
    const auto fields = [&](std::size_t size, std::size_t capacity) {
        std::memcpy(object.data() + 16, &size, sizeof(size));
        std::memcpy(object.data() + 24, &capacity, sizeof(capacity));
    };
    unsigned freed{};
    void *lastFreed{};
    auto free = [&](void *address) {
        ++freed;
        lastFreed = address;
    };
    auto read = [](const void *address, void *&value) {
        std::memcpy(&value, address, sizeof(value));
        return true;
    };
    std::memcpy(object.data(), "screen", 6);
    fields(6, 15);
    assert(copyOwnedMsvcString(object, layout, free, read) == "screen");
    assert(freed == 0);
    char external[64] = "global_texture_pack_tab";
    char *pointer = external;
    std::memcpy(object.data(), &pointer, sizeof(pointer));
    fields(23, 63);
    assert(copyOwnedMsvcString(object, layout, free, read) == "global_texture_pack_tab");
    assert(freed == 1 && lastFreed == external);
    fields(65, 63);
    try {
        copyOwnedMsvcString(object, layout, free, read);
        assert(false);
    } catch (const std::runtime_error &) {
    }
    assert(freed == 2 && lastFreed == external);
    alignas(32) std::array<std::byte, 4352> large{};
    pointer = reinterpret_cast<char *>(large.data() + 32);
    void *base = large.data();
    std::memcpy(pointer - sizeof(void *), &base, sizeof(base));
    std::memcpy(pointer, "sky", 3);
    std::memcpy(object.data(), &pointer, sizeof(pointer));
    fields(3, 4095);
    assert(copyOwnedMsvcString(object, layout, free, read) == "sky");
    assert(freed == 3 && lastFreed == base);
    fields(513, 4095);
    try {
        copyOwnedMsvcString(object, layout, free, read);
        assert(false);
    } catch (const std::runtime_error &) {
    }
    assert(freed == 4 && lastFreed == base);
    fields(3, 4095);
    try {
        copyOwnedMsvcString(object, layout, free, [](const void *, void *&) { return false; });
        assert(false);
    } catch (const std::runtime_error &) {
    }
    assert(freed == 4);
    auto badLayout = layout;
    badLayout.sizeOffset = 0;
    try {
        copyOwnedMsvcString(object, badLayout, free, read);
        assert(false);
    } catch (const std::invalid_argument &) {
    }
    assert(freed == 4);
}
