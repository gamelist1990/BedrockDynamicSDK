#pragma once
#include <cstddef>

namespace bedrock::detail {
struct NativeCallbackLayout {
    std::size_t ownerOffset{}, invokeOffset{}, destroyOffset{};
};
// Consume only the exact game functor supported by this adapter. The caller
// supplies guarded memory access and game-owned invocation/destruction. Claim
// ownership before invocation so exceptions and re-entry cannot run it twice.
template <class Read, class Claim, class Invoke, class Destroy>
bool consumeNativeCallback(void *functionObject, NativeCallbackLayout layout,
                           void *expectedInvoke, void *expectedDestroy, Read read, Claim claim,
                           Invoke invoke, Destroy destroy) {
    void *owner{}, *vtable{}, *invoker{}, *deleter{};
    if (!read(functionObject, layout.ownerOffset, owner) || !owner ||
        !read(owner, 0, vtable) || !vtable ||
        !read(vtable, layout.invokeOffset, invoker) || invoker != expectedInvoke ||
        !read(vtable, layout.destroyOffset, deleter) || deleter != expectedDestroy ||
        !claim(functionObject, layout.ownerOffset, owner))
        return false;
    struct Cleanup {
        Destroy &destroy;
        void *owner;
        bool heap;
        ~Cleanup() { destroy(owner, heap); }
    } cleanup{destroy, owner, owner != functionObject};
    invoke(owner);
    return true;
}
} // namespace bedrock::detail
