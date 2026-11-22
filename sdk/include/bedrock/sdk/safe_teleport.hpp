#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "profiles.hpp"

namespace bedrock::sdk {

// --------------------------------------------------------------------------
// SafeTeleport — public facade over the native safe-teleport helper. It
// provides the tick scope AutoMine's chat messaging runs inside: reads and
// writes require the Scope returned by tick() on the owning level tick.
// A successful local write is not a promise that a remote server accepts
// it.
// --------------------------------------------------------------------------
class SafeTeleport final {
public:
    // RAII tick scope: while it lives, the facade is "on tick" and
    // message() may be used. Move-only, like the native scope.
    class Scope final {
    public:
        Scope() = default;
        ~Scope();
        Scope(const Scope&) = delete;
        Scope& operator=(const Scope&) = delete;
        Scope(Scope&&) noexcept;
        Scope& operator=(Scope&&) noexcept;
        explicit operator bool() const noexcept { return native_ != nullptr; }

    private:
        friend class SafeTeleport;
        struct NativeScope;
        explicit Scope(std::unique_ptr<NativeScope> native);
        std::unique_ptr<NativeScope> native_;
    };

    SafeTeleport();
    ~SafeTeleport();
    SafeTeleport(const SafeTeleport&) = delete;
    SafeTeleport& operator=(const SafeTeleport&) = delete;
    SafeTeleport(SafeTeleport&&) noexcept;
    SafeTeleport& operator=(SafeTeleport&&) noexcept;

    // Bind the teleport/message layouts from the selected profile.
    bool load(const Profile& profile, std::string& error);
    void reset(std::uint64_t epoch);
    void stop();

    // Open the tick scope for this client/level/epoch (empty scope when
    // the facade is not ready, already scoped, or stopped).
    Scope tick(void* client, void* level, std::uint64_t epoch);

    // Send a chat message; only valid inside the tick scope.
    bool message(const std::string& text) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace bedrock::sdk
