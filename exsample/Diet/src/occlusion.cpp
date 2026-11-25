#include "diet/occlusion.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>

namespace diet {
namespace {
constexpr float kEpsilon = 1.0e-4f;

float axisDistance(float value, float minimum, float maximum) {
    if (value < minimum) return minimum - value;
    if (value > maximum) return value - maximum;
    return 0.0f;
}

} // namespace

std::uint64_t steadyClockNs() noexcept {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

bool Aabb::valid() const noexcept {
    return std::isfinite(min.x) && std::isfinite(min.y) && std::isfinite(min.z) &&
           std::isfinite(max.x) && std::isfinite(max.y) && std::isfinite(max.z) &&
           max.x >= min.x && max.y >= min.y && max.z >= min.z;
}

Vec3f Aabb::center() const noexcept {
    return {(min.x + max.x) * 0.5f, (min.y + max.y) * 0.5f, (min.z + max.z) * 0.5f};
}

float Aabb::distanceTo(const Vec3f& point) const noexcept {
    const float dx = axisDistance(point.x, min.x, max.x);
    const float dy = axisDistance(point.y, min.y, max.y);
    const float dz = axisDistance(point.z, min.z, max.z);
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

bool traceBlocked(OccluderFn occluder, void* context, const Vec3f& from, const Vec3f& to,
                  int maxSteps) noexcept {
    if (!occluder || maxSteps <= 0) return false;
    if (!std::isfinite(from.x) || !std::isfinite(from.y) || !std::isfinite(from.z) ||
        !std::isfinite(to.x) || !std::isfinite(to.y) || !std::isfinite(to.z))
        return false;

    const float direction[3]{to.x - from.x, to.y - from.y, to.z - from.z};
    const float origin[3]{from.x, from.y, from.z};
    const float length = std::sqrt(direction[0] * direction[0] + direction[1] * direction[1] +
                                   direction[2] * direction[2]);
    if (!(length > 0.0f)) return false;

    int voxel[3]{static_cast<int>(std::floor(origin[0])), static_cast<int>(std::floor(origin[1])),
                 static_cast<int>(std::floor(origin[2]))};
    const int endVoxel[3]{static_cast<int>(std::floor(to.x)), static_cast<int>(std::floor(to.y)),
                          static_cast<int>(std::floor(to.z))};
    int step[3]{0, 0, 0};
    float tMax[3]{0.0f, 0.0f, 0.0f};
    float tDelta[3]{0.0f, 0.0f, 0.0f};
    for (int axis = 0; axis < 3; ++axis) {
        if (direction[axis] > 0.0f) {
            step[axis] = 1;
            const float boundary = static_cast<float>(voxel[axis] + 1);
            tMax[axis] = (boundary - origin[axis]) / direction[axis];
            tDelta[axis] = 1.0f / direction[axis];
        } else if (direction[axis] < 0.0f) {
            step[axis] = -1;
            const float boundary = static_cast<float>(voxel[axis]);
            tMax[axis] = (boundary - origin[axis]) / direction[axis];
            tDelta[axis] = -1.0f / direction[axis];
        } else {
            step[axis] = 0;
            tMax[axis] = std::numeric_limits<float>::infinity();
            tDelta[axis] = std::numeric_limits<float>::infinity();
        }
    }

    for (int stepIndex = 0; stepIndex < maxSteps; ++stepIndex) {
        if (voxel[0] == endVoxel[0] && voxel[1] == endVoxel[1] && voxel[2] == endVoxel[2])
            return false; // reached the target cell without crossing an occluder
        int axis = 0;
        if (tMax[1] < tMax[axis]) axis = 1;
        if (tMax[2] < tMax[axis]) axis = 2;
        if (!(tMax[axis] <= 1.0f)) return false; // next boundary is past the segment end
        voxel[axis] += step[axis];
        tMax[axis] += tDelta[axis];
        if (voxel[0] == endVoxel[0] && voxel[1] == endVoxel[1] && voxel[2] == endVoxel[2])
            return false;
        if (occluder(context, voxel[0], voxel[1], voxel[2])) return true;
    }
    return false;
}

SamplePlan planSamples(const Aabb& box, const Vec3f& camera, unsigned wanted) noexcept {
    SamplePlan plan;
    if (!box.valid() || wanted == 0) return plan;
    wanted = std::min(wanted, SamplePlan::capacity);
    const Vec3f centre = box.center();

    Vec3f corners[8];
    unsigned cornerCount = 0;
    const float inset = std::min({box.max.x - box.min.x, box.max.y - box.min.y,
                                  box.max.z - box.min.z}) *
                        0.25f;
    for (int index = 0; index < 8; ++index) {
        Vec3f corner{(index & 1) ? box.max.x : box.min.x, (index & 2) ? box.max.y : box.min.y,
                     (index & 4) ? box.max.z : box.min.z};
        // Keep the sample strictly inside the box so the ray cannot terminate
        // inside the target's own boundary voxel.
        corner.x += (corner.x > centre.x ? -inset : inset);
        corner.y += (corner.y > centre.y ? -inset : inset);
        corner.z += (corner.z > centre.z ? -inset : inset);
        corners[cornerCount++] = corner;
    }
    const auto square = [&camera](const Vec3f& p) {
        const float dx = p.x - camera.x, dy = p.y - camera.y, dz = p.z - camera.z;
        return dx * dx + dy * dy + dz * dz;
    };
    std::sort(corners, corners + cornerCount,
              [&square](const Vec3f& a, const Vec3f& b) { return square(a) < square(b); });

    const auto append = [&plan](const Vec3f& point) {
        for (unsigned i = 0; i < plan.count; ++i) {
            const Vec3f& existing = plan.points[i];
            if (std::fabs(existing.x - point.x) < kEpsilon &&
                std::fabs(existing.y - point.y) < kEpsilon &&
                std::fabs(existing.z - point.z) < kEpsilon)
                return;
        }
        if (plan.count < SamplePlan::capacity) plan.points[plan.count++] = point;
    };

    append(corners[0]);
    append(centre);
    append(corners[1]);
    const Vec3f faces[6]{{centre.x, centre.y, box.max.z}, {centre.x, centre.y, box.min.z},
                         {box.max.x, centre.y, centre.z}, {box.min.x, centre.y, centre.z},
                         {centre.x, box.max.y, centre.z}, {centre.x, box.min.y, centre.z}};
    for (const auto& face : faces) {
        if (plan.count >= wanted) break;
        append(face);
    }
    while (plan.count > wanted) --plan.count;
    return plan;
}

OcclusionEngine::Record* OcclusionEngine::find(std::uint64_t runtimeId) noexcept {
    for (auto& record : targets_)
        if (record.runtimeId == runtimeId) return &record;
    return nullptr;
}

unsigned OcclusionEngine::intervalMs(float distance) const noexcept {
    const float third = std::max(1.0f, limits_.maxDistance / 3.0f);
    if (distance <= third) return limits_.nearIntervalMs;
    if (distance <= third * 2.0f) return limits_.midIntervalMs;
    return limits_.farIntervalMs;
}

bool OcclusionEngine::overBudget() const noexcept {
    return (clock_ ? clock_() : steadyClockNs()) > budgetEndNs_;
}

void OcclusionEngine::beginTick(std::uint64_t nowNs, const Vec3f& camera, OccluderFn occluder,
                                void* context) noexcept {
    nowNs_ = nowNs ? nowNs : steadyClockNs();
    camera_ = camera;
    occluder_ = occluder;
    context_ = context;
    budgetEndNs_ = nowNs_ + static_cast<std::uint64_t>(limits_.tickBudgetUs) * 1000ull;
    stats_ = EngineStats{};
}

void OcclusionEngine::addTarget(const TargetInput& input) noexcept {
    if (!input.runtimeId || !input.box.valid()) return;
    Record* record = find(input.runtimeId);
    if (!record) {
        if (targets_.size() >= 4096) return; // hard bound; never grows without limit
        targets_.push_back(Record{});
        record = &targets_.back();
        record->runtimeId = input.runtimeId;
        record->nextCheckNs = nowNs_; // due immediately on first sight
    }
    record->box = input.box;
    record->isPlayer = input.isPlayer;
    record->lastSeenNs = nowNs_;
}

void OcclusionEngine::evaluate(Record& record, std::uint64_t nowNs) noexcept {
    const float distance = record.box.distanceTo(camera_);
    const auto schedule = [&](float metres) {
        record.nextCheckNs =
            nowNs + static_cast<std::uint64_t>(intervalMs(metres)) * 1000000ull;
    };
    ++stats_.checked;

    if (record.isPlayer && limits_.protectPlayers) {
        record.occludedStreak = 0;
        record.lastOccluded = false;
        record.hidden = false;
        ++stats_.protectedTargets;
        schedule(distance);
        return;
    }
    if (distance < limits_.nearGuard) {
        record.occludedStreak = 0;
        record.lastOccluded = false;
        record.hidden = false;
        schedule(distance);
        return;
    }
    if (distance > limits_.maxDistance) {
        // Distance stage: no raycast, cull for this interval.
        record.occludedStreak = limits_.hideAfter;
        record.lastOccluded = true;
        record.hidden = true;
        ++stats_.distanceCulled;
        schedule(distance);
        return;
    }

    const SamplePlan plan = planSamples(record.box, camera_, limits_.maxSamples);
    bool occluded = plan.count > 0;
    bool complete = true;
    for (unsigned i = 0; i < plan.count; ++i) {
        ++stats_.samples;
        if (!traceBlocked(occluder_, context_, camera_, plan.points[i],
                          static_cast<int>(limits_.maxRaySteps))) {
            occluded = false; // one visible sample is enough to stay visible
            break;
        }
        if (overBudget()) {
            complete = false; // partial evidence never hides a target
            break;
        }
    }
    if (!complete) {
        ++stats_.budgetStops;
        schedule(distance);
        return;
    }
    record.lastOccluded = occluded;
    if (occluded) {
        if (record.occludedStreak < 0xFFFFFFFFu) ++record.occludedStreak;
        if (record.occludedStreak >= limits_.hideAfter) record.hidden = true;
        ++stats_.occluded;
    } else {
        record.occludedStreak = 0;
        record.hidden = false; // show immediately
        ++stats_.visible;
    }
    schedule(distance);
}

void OcclusionEngine::endTick(std::uint64_t nowNs) noexcept {
    const auto started = std::chrono::steady_clock::now();
    const std::uint64_t now = nowNs ? nowNs : steadyClockNs();

    // Drop targets that have not been offered for the retention window. Actor
    // capture is incremental, so "not offered this tick" is normal.
    const std::uint64_t retainNs = static_cast<std::uint64_t>(limits_.retainMs) * 1000000ull;
    targets_.erase(std::remove_if(targets_.begin(), targets_.end(),
                                  [now, retainNs](const Record& record) {
                                      return now > record.lastSeenNs &&
                                             now - record.lastSeenNs > retainNs;
                                  }),
                   targets_.end());
    stats_.tracked = static_cast<unsigned>(targets_.size());

    std::size_t examined = 0;
    while (examined < targets_.size() && stats_.checked < limits_.maxChecksPerTick) {
        if (cursor_ >= targets_.size()) cursor_ = 0;
        Record& record = targets_[cursor_];
        ++examined;
        if (record.nextCheckNs <= now) {
            if (overBudget()) { // never start work the budget cannot afford
                ++stats_.budgetStops;
                break;
            }
            evaluate(record, now);
            if (overBudget()) {
                ++stats_.budgetStops;
                break;
            }
        }
        ++cursor_;
    }

    verdicts_.clear();
    for (const auto& record : targets_) {
        Verdict verdict;
        verdict.runtimeId = record.runtimeId;
        verdict.box = record.box;
        verdict.distance = record.box.distanceTo(camera_);
        verdict.streak = record.occludedStreak;
        verdict.hidden = record.hidden;
        verdict.isPlayer = record.isPlayer;
        if (record.hidden) {
            verdict.kind = verdict.distance > limits_.maxDistance ? VerdictKind::DistanceCulled
                                                                 : VerdictKind::Occluded;
            ++stats_.hidden;
        } else if (record.lastOccluded) {
            verdict.kind = VerdictKind::Occluded; // blocked, not yet hidden (hysteresis)
        } else if (verdict.distance < limits_.nearGuard) {
            verdict.kind = VerdictKind::NearGuard;
        } else if (record.isPlayer && limits_.protectPlayers) {
            verdict.kind = VerdictKind::PlayerProtected;
        } else {
            verdict.kind = VerdictKind::Visible;
        }
        verdicts_.push_back(verdict);
    }
    stats_.elapsedUs = std::chrono::duration_cast<std::chrono::duration<float, std::micro>>(
                           std::chrono::steady_clock::now() - started)
                           .count();
    ++epoch_;
}

} // namespace diet
