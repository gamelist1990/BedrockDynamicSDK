#include <bedrock/detail/owned_native_callback.hpp>
#include <array>
#include <cassert>
#include <cstring>
#include <stdexcept>

int main() {
    using namespace bedrock::detail;
    NativeCallbackLayout layout{56, 16, 32};
    std::array<void *, 5> table{};
    int invokeTag{}, destroyTag{}, foreignTag{};
    table[2] = &invokeTag;
    table[4] = &destroyTag;
    alignas(void *) std::array<std::byte, 64> function{};
    struct Functor { void *vtable; } external{table.data()};
    auto read = [](const void *base, std::size_t offset, void *&out) {
        std::memcpy(&out, static_cast<const std::byte *>(base) + offset, sizeof(out));
        return true;
    };
    auto claim = [&](void *base, std::size_t offset, void *expected) {
        void *actual{};
        read(base, offset, actual);
        if (actual != expected)
            return false;
        actual = nullptr;
        std::memcpy(static_cast<std::byte *>(base) + offset, &actual, sizeof(actual));
        return true;
    };
    auto store = [&](void *owner) { std::memcpy(function.data() + 56, &owner, sizeof(owner)); };
    unsigned calls{}, destroyed{};
    bool heap{};
    auto destroy = [&](void *, bool release) { ++destroyed; heap = release; };
    auto consume = [&](auto invoke) {
        return consumeNativeCallback(function.data(), layout, &invokeTag, &destroyTag,
                                     read, claim, invoke, destroy);
    };
    store(&external);
    table[2] = &foreignTag;
    assert(!consume([&](void *) { ++calls; }));
    assert(calls == 0 && destroyed == 0);
    void *owner{};
    read(function.data(), 56, owner);
    assert(owner == &external); // Unsupported functor stays game-owned.
    table[2] = &invokeTag;
    assert(consume([&](void *value) {
        assert(value == &external);
        ++calls;
        void *claimed{};
        read(function.data(), 56, claimed);
        assert(!claimed); // Nested entry cannot claim the same callback again.
    }));
    assert(calls == 1 && destroyed == 1 && heap);
    assert(!consume([&](void *) { ++calls; }));
    assert(destroyed == 1);
    store(&external);
    try {
        consume([&](void *) { throw std::runtime_error("game callback failed"); });
        assert(false);
    } catch (const std::runtime_error &) {}
    assert(destroyed == 2 && heap);
    // An inline functor must be destructed without freeing its enclosing object.
    void *vtable = table.data();
    std::memcpy(function.data(), &vtable, sizeof(vtable));
    store(function.data());
    assert(consume([](void *) {}));
    assert(destroyed == 3 && !heap);
    store(&external);
    assert(!consumeNativeCallback(function.data(), layout, &invokeTag, &destroyTag,
                                  read, [](void *, std::size_t, void *) { return false; },
                                  [](void *) { assert(false); }, destroy));
    assert(destroyed == 3);
}
