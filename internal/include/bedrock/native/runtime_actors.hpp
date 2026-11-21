#pragma once
#include <bedrock/detail/native_profile_functions.hpp>
#include <bedrock/profiles/profile_json.hpp>
#include <cstring>
#include <optional>
#include <vector>
namespace bedrock {
// Invoke on the owning Level tick only. Returned handles are not owned and
// must be validated/consumed on that tick, never retained by a worker.
class NativeRuntimeActors {
  public:
    bool load(std::string_view selected, std::string_view embedded, std::string &error) {
        try {
            const auto profile =
                load(native_profile::layoutProfile(selected, embedded, "RuntimeActorList"),
                     error);
            if (profile && !release_) {
                // The selected profile may predate the "release" field (the
                // runtime's 24 h profile cache outranks a freshly embedded
                // profile). Resolve the release function from whichever
                // profile text carries it, so a stale cache cannot silently
                // disable the cross-allocator fix below.
                tryResolveRelease(selected);
                if (!release_)
                    tryResolveRelease(embedded);
            }
            return profile;
        } catch (const std::exception &e) {
            error = e.what();
            return false;
        }
    }
    bool load(std::string_view profile, std::string &error) {
        target_ = nullptr;
        release_ = nullptr;
        try {
            const auto root = parseNativeProfile(profile);
            const auto &layout = root.at("layouts").at("RuntimeActorList");
            if (layout.at("schema") != 1 ||
                layout.at("abi") != "void(Level*, std::vector<Actor*>* hidden_return)")
                throw std::runtime_error("unsupported runtime Actor list ABI");
            limit_ = native_profile::number(layout.at("maximum_actors"));
            if (!limit_ || limit_ > 65536)
                throw std::runtime_error("invalid runtime Actor list bound");
            target_ = native_profile::checked(root, layout.at("function"));
            if (layout.contains("release"))
                release_ = native_profile::checked(root, layout.at("release"));
            error.clear();
            return true;
        } catch (const std::exception &e) {
            error = e.what();
            return false;
        }
    }
    std::optional<std::vector<void *>> snapshot(void *level) const {
        if (!level || !target_)
            return std::nullopt;
        std::vector<void *> result;
        if (!invoke(level, &result)) {
            releaseBuffer(result);
            return std::nullopt;
        }
        // The game builds `result` with its OWN allocator (it zeroes the
        // hidden-return vector first, then grows it through the game's
        // central allocator). Letting this module's statically linked CRT
        // destroy that buffer frees foreign memory and corrupts the heap —
        // detected by the real Windows heap manager as 0xC0000374, while
        // Wine's heap silently tolerated it. Copy the elements into our
        // own storage and hand the game's buffer back to the game's own
        // deallocate instead.
        const std::size_t count = result.size();
        std::optional<std::vector<void *>> copy;
        if (count <= limit_)
            copy = std::vector<void *>(result.begin(), result.end());
        releaseBuffer(result);
        return copy;
    }
    bool ready() const noexcept {
        return target_ != nullptr;
    }

  private:
    void tryResolveRelease(std::string_view profile) noexcept {
        try {
            const auto root = parseNativeProfile(profile);
            const auto &layouts = root.at("layouts");
            if (!layouts.contains("RuntimeActorList"))
                return;
            const auto &layout = layouts.at("RuntimeActorList");
            if (layout.contains("release"))
                release_ = native_profile::checked(root, layout.at("release"));
        } catch (...) {
        }
    }
    // Detach the game-built vector (so this module's CRT never frees its
    // buffer) and release the buffer through the game's own deallocate,
    // mirroring the game's _Deallocate: allocations of 0x1000 bytes or
    // more were over-allocated for alignment and stash the raw base in
    // the 8 bytes before the user pointer (validated the same way the
    // game validates it). Without a resolved release function the buffer
    // is deliberately leaked — bounded by maximum_actors — rather than
    // freed into the wrong heap.
    void releaseBuffer(std::vector<void *> &vec) const noexcept {
        void *buffer = vec.data();
        const std::size_t bytes = vec.capacity() * sizeof(void *);
        std::memset(&vec, 0, sizeof(vec));
        if (!buffer || !release_)
            return;
        __try {
            void *base = buffer;
            if (bytes >= 0x1000) {
                void *stashed =
                    *reinterpret_cast<void **>(static_cast<char *>(buffer) - sizeof(void *));
                const auto back = static_cast<std::uintptr_t>(
                    static_cast<char *>(buffer) - sizeof(void *) -
                    static_cast<char *>(stashed));
                if (back > 0x20)
                    return;
                base = stashed;
            }
            reinterpret_cast<void(__fastcall *)(void *)>(release_)(base);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
    }
    bool invoke(void *level, std::vector<void *> *output) const noexcept {
        __try {
            reinterpret_cast<void(__fastcall *)(void *, std::vector<void *> *)>(target_)(level,
                                                                                         output);
            return true;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
    }
    void *target_{};
    void *release_{};
    std::size_t limit_{};
};
} // namespace bedrock
