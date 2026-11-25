#pragma once
// Diet — occlusion decision core.
//
// Host-testable: no Windows, no native game pointers, no per-tick allocation
// after warm-up. The Mod supplies a block-occluder callback and a camera
// position; sample planning, DDA traversal, distance staging, hysteresis and
// the per-tick budget live here.
#include <cstddef>
#include <cstdint>
#include <vector>

namespace diet {

struct Vec3f {
    float x{}, y{}, z{};
};

struct Aabb {
    Vec3f min{};
    Vec3f max{};
    [[nodiscard]] bool valid() const noexcept;
    [[nodiscard]] Vec3f center() const noexcept;
    [[nodiscard]] float distanceTo(const Vec3f& point) const noexcept;
};

// Returns true when the voxel occludes line of sight (an opaque full cube).
using OccluderFn = bool (*)(void* context, int x, int y, int z);
// Steady-clock source in nanoseconds; replaceable so host tests are
// deterministic. Defaults to std::chrono::steady_clock.
using ClockFn = std::uint64_t (*)() noexcept;
[[nodiscard]] std::uint64_t steadyClockNs() noexcept;

// Copied 3D DDA (Amanatides & Woo). The voxel containing `to` is never tested:
// a target standing in a block is not occluded by its own cell.
[[nodiscard]] bool traceBlocked(OccluderFn occluder, void* context, const Vec3f& from,
                                const Vec3f& to, int maxSteps) noexcept;

struct SamplePlan {
    static constexpr unsigned capacity = 7;
    Vec3f points[capacity]{};
    unsigned count{};
};

// Sample points ordered nearest-to-camera first: nearest AABB corner, centre,
// next corner, then face centres. Duplicates are dropped so a degenerate box
// still yields one usable sample.
[[nodiscard]] SamplePlan planSamples(const Aabb& box, const Vec3f& camera,
                                     unsigned wanted) noexcept;

enum class VerdictKind : unsigned char {
    Visible = 0,
    Occluded = 1,
    DistanceCulled = 2,
    NearGuard = 3,
    PlayerProtected = 4,
};

struct Limits {
    float maxDistance = 96.0f;     // beyond: cull without raycasts
    float nearGuard = 12.0f;       // never cull closer than this
    unsigned hideAfter = 2;        // consecutive occluded checks before hiding
    unsigned nearIntervalMs = 120;
    unsigned midIntervalMs = 300;
    unsigned farIntervalMs = 600;
    unsigned maxChecksPerTick = 24;
    unsigned maxSamples = 3;
    unsigned maxRaySteps = 192;
    unsigned tickBudgetUs = 700;
    unsigned retainMs = 2000;   // keep state for targets not offered this tick
    bool protectPlayers = true;
};

struct TargetInput {
    std::uint64_t runtimeId{};
    Aabb box{};
    bool isPlayer{};
};

struct Verdict {
    std::uint64_t runtimeId{};
    Aabb box{};
    // `kind` reports the last check result; `hidden` reports the applied cull
    // state (hideAfter hysteresis). A target can be Occluded but not yet hidden.
    VerdictKind kind{VerdictKind::Visible};
    float distance{};
    unsigned streak{};
    bool hidden{};
    bool isPlayer{};
};

struct EngineStats {
    unsigned tracked{};
    unsigned checked{};
    unsigned samples{};
    unsigned visible{};
    unsigned occluded{};
    unsigned distanceCulled{};
    unsigned protectedTargets{};
    unsigned budgetStops{};
    unsigned hidden{};
    float elapsedUs{};
};

// One instance per Mod. All methods run on the owning game/tick thread. `nowNs`
// is a steady clock in nanoseconds (std::chrono::steady_clock epoch) so the
// tick budget and the per-target scheduling share one time base.
class OcclusionEngine {
public:
    void setLimits(const Limits& limits) noexcept { limits_ = limits; }
    void setClock(ClockFn clock) noexcept { clock_ = clock; }
    [[nodiscard]] const Limits& limits() const noexcept { return limits_; }

    void beginTick(std::uint64_t nowNs, const Vec3f& camera, OccluderFn occluder,
                   void* context) noexcept;
    void addTarget(const TargetInput& input) noexcept;
    void endTick(std::uint64_t nowNs) noexcept;

    [[nodiscard]] const Verdict* verdicts() const noexcept { return verdicts_.data(); }
    [[nodiscard]] unsigned verdictCount() const noexcept {
        return static_cast<unsigned>(verdicts_.size());
    }
    [[nodiscard]] const EngineStats& stats() const noexcept { return stats_; }
    [[nodiscard]] std::uint64_t epoch() const noexcept { return epoch_; }

private:
    struct Record {
        std::uint64_t runtimeId{};
        Aabb box{};
        bool isPlayer{};
        std::uint64_t lastSeenNs{};
        unsigned occludedStreak{};
        bool lastOccluded{};
        bool hidden{};
        std::uint64_t nextCheckNs{};
    };
    [[nodiscard]] Record* find(std::uint64_t runtimeId) noexcept;
    [[nodiscard]] bool overBudget() const noexcept;
    void evaluate(Record& record, std::uint64_t nowNs) noexcept;
    [[nodiscard]] unsigned intervalMs(float distance) const noexcept;

    Limits limits_{};
    std::vector<Record> targets_;
    std::vector<Verdict> verdicts_;
    std::size_t cursor_{};
    std::uint64_t nowNs_{};
    std::uint64_t budgetEndNs_{};
    Vec3f camera_{};
    OccluderFn occluder_{};
    ClockFn clock_{};
    void* context_{};
    EngineStats stats_{};
    std::uint64_t epoch_{};
};

} // namespace diet
