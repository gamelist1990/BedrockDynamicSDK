#pragma once
#include <bedrock/detail/native_world_access.hpp>
#include <bedrock/detail/native_profile_functions.hpp>
#include <atomic>

namespace bedrock::native_player {
// Borrowed local-player access valid only in an owning client-tick scope.
class Context {
public:
    class Scope {
        friend class Context;
        Context* owner_{};
        explicit Scope(Context* owner) : owner_(owner) {}
    public:
        Scope(const Scope&) = delete;
        Scope& operator=(const Scope&) = delete;
        Scope(Scope&& other) noexcept : owner_(std::exchange(other.owner_, nullptr)) {}
        ~Scope() { if (owner_) owner_->active_ = false; }
        explicit operator bool() const noexcept { return owner_ != nullptr; }
    };
    bool load(const RuntimeProfile& profile, std::string& error) {
        ready_ = false;
        if (!profile.supports({Feature::Actors}, error)) return false;
        try {
            if (!parseNativeActorLayout(profile.json(), bindings_.actors, error)) return false;
            const auto root = nlohmann::json::parse(profile.json());
            local_ = reinterpret_cast<LocalGetter>(native_profile::checked(root, root.at("native_camera").at("local_player")));
            ready_ = true; error.clear(); return true;
        } catch (const std::exception& e) { error = e.what(); return false; }
    }
    void reset() noexcept { generation_.fetch_add(1); }
    Scope tick(void* client, WorldEpoch epoch) {
        DWORD expected{}; const auto thread = GetCurrentThreadId();
        owner_.compare_exchange_strong(expected, thread);
        if (!ready_ || owner_.load() != thread || active_) return Scope{nullptr};
        actor_ = invokeLocal(local_, client); epoch_ = epoch.value(); generationAtTick_ = generation_.load(); runtime_ = 0;
        if (!actor_ || !world_native::read(component(bindings_.actors.runtimeIdComponentHash),
                bindings_.actors.runtimeIdValueOffset, runtime_) || !runtime_) return Scope{nullptr};
        active_ = true; return Scope{this};
    }
    bool onThread() const { return active_ && owner_.load() == GetCurrentThreadId(); }
    bool valid() const {
        if (!onThread() || generationAtTick_ != generation_.load()) return false;
        std::uint64_t runtime{};
        return world_native::read(component(bindings_.actors.runtimeIdComponentHash), bindings_.actors.runtimeIdValueOffset, runtime) && runtime == runtime_;
    }
    void* component(std::uint32_t hash) const { return bindings_.component(actor_, hash); }
    void* actor() const noexcept { return actor_; }
    std::uint64_t runtimeId() const noexcept { return runtime_; }
    std::uint64_t epoch() const noexcept { return epoch_; }
    const NativeActorLayout& layout() const noexcept { return bindings_.actors; }
private:
    using LocalGetter = void*(__fastcall*)(void*);
    static void* invokeLocal(LocalGetter fn, void* client) noexcept {
        if (!fn || !client) return nullptr;
        __try { return fn(client); } __except(EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
    }
    world_native::Bindings bindings_;
    LocalGetter local_{};
    void* actor_{};
    std::uint64_t runtime_{}, epoch_{}, generationAtTick_{};
    std::atomic<std::uint64_t> generation_{};
    std::atomic<DWORD> owner_{};
    bool ready_{}, active_{};
};
} // namespace bedrock::native_player
