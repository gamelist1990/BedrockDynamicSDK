#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>

namespace bedrock::detail {
struct OwnedMsvcStringLayout {
    std::size_t sizeOffset{}, capacityOffset{}, inlineBytes{}, maximumBytes{},
        largeAllocationThreshold{}, alignmentOverhead{};
};
// Read a game-constructed Release MSVC string and return a DLL-owned copy.
// freeAllocation must call the GAME allocator, not the consuming DLL's CRT.
// readHeader is supplied by the native adapter to contain bad pointer reads.
template <class Free, class Read>
std::string copyOwnedMsvcString(const std::array<std::byte, 32> &storage,
                                const OwnedMsvcStringLayout &layout, Free freeAllocation,
                                Read readHeader) {
    if (layout.sizeOffset != 16 || layout.capacityOffset != 24 || layout.inlineBytes != 16 ||
        layout.maximumBytes == 0 || layout.maximumBytes > 4096 ||
        layout.largeAllocationThreshold != 4096 || layout.alignmentOverhead != 39)
        throw std::invalid_argument("unsupported MSVC string layout");
    std::size_t size{}, capacity{};
    std::memcpy(&size, storage.data() + layout.sizeOffset, sizeof(size));
    std::memcpy(&capacity, storage.data() + layout.capacityOffset, sizeof(capacity));
    const char *bytes = reinterpret_cast<const char *>(storage.data());
    void *allocation{};
    if (capacity >= layout.inlineBytes) {
        std::memcpy(&bytes, storage.data(), sizeof(bytes));
        if (!bytes || capacity > 1024 * 1024)
            throw std::runtime_error("invalid native string allocation");
        allocation = const_cast<char *>(bytes);
        if (capacity + 1 >= layout.largeAllocationThreshold) {
            if (!readHeader(bytes - sizeof(void *), allocation))
                throw std::runtime_error("unreadable native string allocation header");
            const auto delta = reinterpret_cast<std::uintptr_t>(bytes) -
                               reinterpret_cast<std::uintptr_t>(allocation);
            if (delta < sizeof(void *) || delta > layout.alignmentOverhead)
                throw std::runtime_error("invalid native string alignment");
        }
    }
    struct Owner {
        Free &free;
        void *pointer;
        ~Owner() {
            if (pointer)
                free(pointer);
        }
    } owner{freeAllocation, allocation};
    if (size > capacity || size > layout.maximumBytes)
        throw std::runtime_error("native string exceeds profile bounds");
    return std::string(bytes, size);
}
} // namespace bedrock::detail
