#pragma once
#include <bedrock/detail/native_teleport_access.hpp>
#include <bedrock/profiles/profile_manager.hpp>
#include <atomic>

namespace bedrock {
// Safe, incremental upward navigation. No hooks or background native reads.
// Reads and writes require the Scope returned by tick() on the owning level tick.
// A successful local write is not a promise that a remote server accepts it.
class NativeSafeTeleport {
public:
    using Observer = std::function<void(const TeleportResult&)>;
    class Scope {
        friend class NativeSafeTeleport;
        NativeSafeTeleport* owner_{};
        explicit Scope(NativeSafeTeleport* owner) : owner_(owner) {}
    public:
        Scope(const Scope&) = delete;
        Scope& operator=(const Scope&) = delete;
        Scope(Scope&& other) noexcept : owner_(std::exchange(other.owner_, nullptr)) {}
        ~Scope() { if (owner_) owner_->active_ = false; }
        explicit operator bool() const noexcept { return owner_ != nullptr; }
    };
    NativeSafeTeleport() : reader_(bindings_), safety_(bindings_, reader_) {}
    void configure(SafeTeleportOptions options) { options.validate(); options_ = options; }
    void onResult(Observer observer) { observer_ = std::move(observer); }
    bool load(const RuntimeProfile& profile, std::string& error) {
        ready_ = false;
        if (!profile.supports({Feature::World, Feature::Motion, Feature::WorldHeight, Feature::Gui}, error)) return false;
        try {
            bindings_.loadTeleport(profile.json()); reader_.clear(); ready_ = true;
            error.clear(); return true;
        } catch (const std::exception& e) { error = e.what(); return false; }
    }
    void reset(std::uint64_t epoch) { requestedEpoch_.store(epoch); resetRequested_.store(true); }
    void stop() { stopped_.store(true); }
    Scope tick(void* client, void* level, std::uint64_t epoch) {
        DWORD expected{};
        const auto thread = GetCurrentThreadId();
        ownerThread_.compare_exchange_strong(expected, thread);
        if (ownerThread_.load() != thread || active_ || !ready_ || stopped_.load()) return Scope{nullptr};
        const bool reset = resetRequested_.exchange(false);
        if (reset || epoch_ != epoch || client_ != client || level_ != level) {
            search_.reset(); reader_.clear();
        }
        client_ = client; level_ = level; epoch_ = epoch; requestedEpoch_.store(epoch);
        active_ = true;
        Scope scope{this};
        if (search_) advance();
        return scope;
    }
    bool message(const std::string& text) const {
        if (!onTick()) return false;
        return bindings_.message(client_, text, std::nullopt, std::string{});
    }
    CommandDecision request(std::optional<int> distance = std::nullopt) {
        if (!onTick()) return CommandDecision::Rejected;
        search_.reset();
        if (distance && *distance <= 0) return fail(TeleportError::InvalidDistance);
        Search next;
        if (!context(next)) return fail(TeleportError::UnavailableContext);
        const float feet = next.bounds.minimum.y;
        if (distance) {
            if (options_.maximumRise && static_cast<unsigned>(*distance) > options_.maximumRise)
                return fail(TeleportError::DistanceLimit);
            const double y = double(feet) + *distance;
            if (y + next.bounds.maximum.y - feet > next.high) return fail(TeleportError::HeightLimit);
            return finish(next, static_cast<float>(y));
        }
        next.nextY = static_cast<int>(std::floor(next.bounds.maximum.y - .0001f));
        next.lastY = next.high - 1;
        if (options_.maximumRise) {
            const double cap = std::floor(double(feet) + options_.maximumRise - 1);
            if (cap < next.lastY) next.lastY = static_cast<int>(cap);
        }
        if (next.nextY > next.lastY) return fail(TeleportError::NoSearchSpace);
        search_ = next;
        notify({TeleportPhase::Searching});
        return CommandDecision::Accepted;
    }
    void cancel() { if (ownerThread_.load() == GetCurrentThreadId()) search_.reset(); }
private:
    using Clock = std::chrono::steady_clock;
    struct Search {
        void* player{}; void* source{};
        std::uint64_t runtime{}, epoch{};
        Vec3 position{};
        ActorBounds bounds{};
        int low{}, high{}, nextY{}, lastY{};
        TeleportError reason{TeleportError::NoSafeDestination};
    };
    bool onTick() const { return active_ && !stopped_.load() && ownerThread_.load() == GetCurrentThreadId(); }
    bool context(Search& result) {
        result.player = bindings_.local(client_); result.source = bindings_.region(client_);
        result.epoch = epoch_;
        void* actualLevel{};
        return result.player && result.source && !resetRequested_.load() && requestedEpoch_.load() == epoch_ &&
            bindings_.pose(result.player, result.position, result.runtime) &&
            world_native::read(result.player, bindings_.layout("player_level_offset"), actualLevel) && actualLevel == level_ &&
            bindings_.bounds(result.player, result.bounds) && bindings_.heights(result.source, result.low, result.high) &&
            std::abs(result.bounds.minimum.x - result.position.x) < 4 &&
            std::abs(result.bounds.minimum.y - result.position.y) < 4 &&
            std::abs(result.bounds.minimum.z - result.position.z) < 4;
    }
    static bool close(Vec3 a, Vec3 b) {
        return std::abs(a.x - b.x) <= .02f && std::abs(a.y - b.y) <= .02f && std::abs(a.z - b.z) <= .02f;
    }
    static ActorBounds atFeet(const Search& search, float y) {
        auto result = search.bounds;
        const float delta = y - result.minimum.y;
        result.minimum.y += delta; result.maximum.y += delta;
        return result;
    }
    TeleportError validate(const Search& search, Search& live) {
        if (search.epoch != epoch_ || !context(live) || live.player != search.player ||
            live.source != search.source || live.runtime != search.runtime) return TeleportError::WorldChanged;
        if (!close(live.position, search.position) || !close(live.bounds.minimum, search.bounds.minimum) ||
            !close(live.bounds.maximum, search.bounds.maximum)) return TeleportError::PlayerMoved;
        return TeleportError::None;
    }
    CommandDecision finish(const Search& search, float y) {
        Search live;
        if (const auto error = validate(search, live); error != TeleportError::None) return fail(error);
        if (const auto error = safety_.check(search.source, atFeet(search, y), live.low, live.high);
            error != TeleportError::None) return fail(error);
        if (!bindings_.move(search.player, y - search.bounds.minimum.y)) return fail(TeleportError::NativeWriteFailed);
        notify({TeleportPhase::Moved, TeleportError::None, y});
        return CommandDecision::Accepted;
    }
    void advance() {
        Search live;
        if (const auto error = validate(*search_, live); error != TeleportError::None) { fail(error); return; }
        const auto deadline = Clock::now() + options_.budget;
        auto& search = *search_;
        for (unsigned i = 0; i < options_.levelsPerTick && search.nextY <= search.lastY; ++i) {
            const int y = search.nextY++;
            auto* block = bindings_.block(search.source, {static_cast<int>(std::floor(search.position.x)), y,
                                                         static_cast<int>(std::floor(search.position.z))});
            const auto* info = reader_.describe(block);
            if (!info) { fail(TeleportError::ChunkUnavailable); return; }
            if (!teleport_native::air(info->name)) {
                const float feet = float(y + 1);
                const auto error = safety_.check(search.source, atFeet(search, feet), live.low, live.high);
                if (error == TeleportError::None) {
                    const auto ready = search; search_.reset(); finish(ready, feet); return;
                }
                search.reason = error;
            }
            if (Clock::now() >= deadline) break;
        }
        if (search.nextY > search.lastY) fail(search.reason);
    }
    CommandDecision fail(TeleportError error) {
        search_.reset(); notify({TeleportPhase::Failed, error});
        return CommandDecision::Rejected;
    }
    void notify(const TeleportResult& result) { if (observer_) observer_(result); }
    teleport_native::Bindings bindings_;
    world_native::Reader reader_;
    teleport_native::Safety safety_;
    SafeTeleportOptions options_;
    Observer observer_;
    std::optional<Search> search_;
    void* client_{}; void* level_{};
    std::uint64_t epoch_{};
    std::atomic<std::uint64_t> requestedEpoch_{};
    std::atomic<DWORD> ownerThread_{};
    std::atomic<bool> resetRequested_{}, stopped_{};
    bool ready_{}, active_{};
};
} // namespace bedrock
