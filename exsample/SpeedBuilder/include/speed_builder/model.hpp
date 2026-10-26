#pragma once

#include <algorithm>
#include <atomic>
#include <array>
#include <cmath>
#include <cstdint>
#include <compare>
#include <limits>
#include <map>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace speed_builder {

using Tick = std::uint64_t;
using WorldId = std::uint64_t;
struct BlockPos {
    int x{}, y{}, z{};
    auto operator<=>(const BlockPos&) const = default;
};
enum class CellKind { unknown, air, block };
enum class Face { down, up, north, south, west, east };
enum class Hand { main };
struct PlacementRecipe {
    std::string itemIdentifier;
    Face face = Face::up;
    float yaw{}, pitch{};
    float hitX = .5f, hitY = .5f, hitZ = .5f;
    bool orientationKnown = false;
    bool exactStateSupported = false;
    bool operator==(const PlacementRecipe&) const = default;
};
struct Cell {
    BlockPos position;
    std::string identifier;
    std::optional<std::uint64_t> stateIdentity;
    CellKind kind = CellKind::unknown;
    bool supportsPlacement = false;
    bool nativePlacementCandidate = false; // Adapter must resolve and recheck the exact state before dispatch.
    std::optional<PlacementRecipe> placement;
    bool operator==(const Cell&) const = default;
};
struct HotbarSlot { std::string identifier; unsigned count{}; };
using Hotbar = std::array<HotbarSlot, 9>;
struct SessionToken {
    WorldId world{};
    std::uint64_t generation{};
    bool operator==(const SessionToken&) const = default;
};
struct Options {
    bool enabled = true;
    unsigned radius = 10;
    unsigned maxBlocks = 512;
    unsigned captureCellsPerTick = 256;
    unsigned buildIntervalTicks = 2;
    double maxReach = 4.5;
    unsigned maxAttempts = 3;
    unsigned confirmationTicks = 20;
    unsigned maxStepsPerTick = 8;
    unsigned maxWaitTicks = 2400;
    unsigned maxBuildTicks = 2400;
    unsigned cancelKey = 27;
    std::string prompt = "記憶しましょう"; // "Remember this!"
};
inline void validateOptions(const Options& o) {
    if (o.radius < 1 || o.radius > 32 || o.maxBlocks < 1 || o.maxBlocks > 8192 ||
        o.captureCellsPerTick < 1 || o.captureCellsPerTick > 4096 ||
        o.buildIntervalTicks < 1 || o.buildIntervalTicks > 1200 ||
        !std::isfinite(o.maxReach) || o.maxReach <= 0 || o.maxReach > 16 ||
        o.maxAttempts < 1 || o.maxAttempts > 32 ||
        o.confirmationTicks < 1 || o.confirmationTicks > 1200 ||
        o.maxStepsPerTick < 1 || o.maxStepsPerTick > 8192 ||
        o.maxWaitTicks < 1 || o.maxBuildTicks < 1 ||
        o.cancelKey < 1 || o.cancelKey > 255 || o.prompt.empty())
        throw std::invalid_argument("invalid SpeedBuilder options");
}
inline BlockPos faceNormal(Face face) {
    switch (face) {
    case Face::down: return {0,-1,0};
    case Face::up: return {0,1,0};
    case Face::north: return {0,0,-1};
    case Face::south: return {0,0,1};
    case Face::west: return {-1,0,0};
    case Face::east: return {1,0,0};
    }
    throw std::invalid_argument("invalid placement face");
}
inline bool supported(const Cell& c) {
    if (c.kind != CellKind::block || c.identifier.empty() || !c.stateIdentity || !c.placement)
        return false;
    const auto& r = *c.placement;
    return r.orientationKnown && r.exactStateSupported && !r.itemIdentifier.empty() &&
        static_cast<unsigned>(r.face) <= static_cast<unsigned>(Face::east) &&
        std::isfinite(r.yaw) && std::isfinite(r.pitch) && r.pitch >= -90 && r.pitch <= 90 &&
        std::isfinite(r.hitX) && r.hitX >= 0 && r.hitX <= 1 &&
        std::isfinite(r.hitY) && r.hitY >= 0 && r.hitY <= 1 &&
        std::isfinite(r.hitZ) && r.hitZ >= 0 && r.hitZ <= 1;
}
inline bool sameState(const Cell& a, const Cell& b) {
    return a.kind == CellKind::block && b.kind == CellKind::block &&
        a.identifier == b.identifier && a.stateIdentity && b.stateIdentity &&
        a.stateIdentity == b.stateIdentity;
}
inline bool candidateForPlacement(const Cell& c) {
    return supported(c) || (c.nativePlacementCandidate && c.kind==CellKind::block && c.stateIdentity &&
        c.placement && !c.placement->itemIdentifier.empty());
}
inline std::optional<BlockPos> supportPosition(const Cell& c) {
    if (!candidateForPlacement(c)) return std::nullopt;
    const auto n = faceNormal(c.placement->face);
    const auto sub = [](int a, int b) -> std::optional<int> {
        const auto v = std::int64_t(a) - b;
        if (v < std::numeric_limits<int>::min() || v > std::numeric_limits<int>::max())
            return std::nullopt;
        return static_cast<int>(v);
    };
    const auto x = sub(c.position.x,n.x), y = sub(c.position.y,n.y), z = sub(c.position.z,n.z);
    if (!x || !y || !z) return std::nullopt;
    return BlockPos{*x,*y,*z};
}
struct CaptureRequest { SessionToken token; std::vector<BlockPos> positions; };
struct PreviewSnapshot {
    SessionToken token;
    BlockPos center;
    unsigned radius{};
    unsigned maxBlocks{};
    std::vector<Cell> cells;
    Hotbar hotbarAtPrompt;
};
struct ObservationSnapshot {
    SessionToken token;
    std::vector<Cell> cells;
    bool previewGone = false;
};
enum class DiffStatus { ready, previewPresent, incomplete, changedBlock, tooManyBlocks, empty };
struct PreparedCandidates {
    SessionToken token;
    DiffStatus status = DiffStatus::incomplete;
    std::vector<Cell> removed;
    std::size_t persistent{};
};

// Pure worker function. Unknown reads and non-air replacements fail closed.
// All input/output storage is owned; there are no SDK objects or callbacks.
inline PreparedCandidates prepareCandidates(const PreviewSnapshot& preview,
                                             const ObservationSnapshot& after) {
    PreparedCandidates out;
    out.token = preview.token;
    if (!(preview.token == after.token)) return out;
    const auto side = std::uint64_t(preview.radius) * 2 + 1;
    if (preview.radius < 1 || preview.radius > 32 ||
        preview.cells.size() != side * side * side || after.cells.size() != preview.cells.size())
        return out;
    std::map<BlockPos, const Cell*> before, current;
    for (const auto& c : preview.cells) {
        const auto dx = std::int64_t(c.position.x) - preview.center.x;
        const auto dy = std::int64_t(c.position.y) - preview.center.y;
        const auto dz = std::int64_t(c.position.z) - preview.center.z;
        if (std::abs(dx) > preview.radius || std::abs(dy) > preview.radius ||
            std::abs(dz) > preview.radius || c.kind == CellKind::unknown ||
            !before.emplace(c.position,&c).second) return out;
    }
    for (const auto& c : after.cells)
        if (c.kind == CellKind::unknown || !current.emplace(c.position,&c).second) return out;
    for (const auto& [position, original] : before) {
        const auto it = current.find(position);
        if (it == current.end()) return out;
        const auto& now = *it->second;
        if (original->kind == CellKind::air) {
            if (now.kind != CellKind::air) { out.status = DiffStatus::changedBlock; return out; }
        } else if (now.kind == CellKind::air) {
            out.removed.push_back(*original);
        } else if (sameState(*original,now)) {
            ++out.persistent;
        } else {
            // Missing state IDs cannot establish that a permanent block is unchanged.
            out.status = DiffStatus::changedBlock;
            return out;
        }
    }
    if (!after.previewGone) out.status = DiffStatus::previewPresent;
    else if (out.removed.empty()) out.status = DiffStatus::empty;
    else if (out.removed.size() > preview.maxBlocks) out.status = DiffStatus::tooManyBlocks;
    else out.status = DiffStatus::ready;
    std::sort(out.removed.begin(),out.removed.end(),[](const Cell& a, const Cell& b) {
        if (a.position.y != b.position.y) return a.position.y < b.position.y;
        return a.position < b.position;
    });
    return out;
}

enum class Phase { idle, capturing, waiting, building, complete, cancelled, failed };
enum class StopReason { none, cancelled, worldChanged, timeout, captureUnknown, oversized, noTargets };
struct Stats {
    std::size_t captured{}, capturePasses{}, persistent{}, removed{}, queued{};
    std::size_t unsupported{}, confirmed{}, exhausted{}, attempts{}, dispatchFailures{};
    std::size_t remaining{};
};
struct PlacementScan {
    unsigned visited{}, unknown{}, occupied{}, inFlight{}, cooldown{}, noSupport{}, dependency{}, outOfReach{}, missingItem{};
};
struct EyePosition { double x{}, y{}, z{}; };
struct BuildFrame {
    WorldId world{};
    Tick tick{};
    EyePosition eye;
    Hotbar hotbar;
    std::vector<Cell> cells; // Current game-thread copies of targets and supports.
};
struct PlacementRequest {
    SessionToken token;
    std::uint64_t actionId{};
    BlockPos target, support;
    PlacementRecipe recipe;
    unsigned hotbarSlot{}; // zero based; select this slot before main-hand use
    Hand hand = Hand::main;
};

// Construct, access, and destroy on one game thread. Transfer snapshots to workers,
// never the Model. Ticks are monotonic within a world generation.
class Model {
public:
    explicit Model(Options options = {}) : options_(std::move(options)), owner_(std::this_thread::get_id()) {
        validateOptions(options_);
    }
    Model(const Model&) = delete;
    Model& operator=(const Model&) = delete;
    Model(Model&&) = delete;
    Model& operator=(Model&&) = delete;

    void reset(WorldId world) {
        checkThread();
        clear(world);
    }
    void cancel() {
        checkThread();
        stop(Phase::cancelled,StopReason::cancelled);
    }
    bool onPrompt(WorldId world, Tick tick, BlockPos center, const Hotbar& hotbar,
                  std::string_view title, std::string_view subtitle) {
        checkThread();
        if (world != token_.world) clear(world);
        const bool matched = !title.empty() && subtitle.find(options_.prompt) != std::string_view::npos;
        // Adapter feeds the current title/subtitle once per tick (including empty).
        const bool rising = matched && !promptVisible_;
        promptVisible_ = matched;
        if (!options_.enabled || !rising) return false;
        clear(world);
        promptVisible_ = true;
        token_.generation = nextGeneration();
        center_ = center;
        baselineHotbar_ = hotbar;grantFloor_=counts(hotbar);
        started_ = lastTick_ = tick;
        phase_ = Phase::capturing;
        const auto r = static_cast<int>(options_.radius);
        for (int z = -r; z <= r; ++z)
            for (int y = -r; y <= r; ++y)
                for (int x = -r; x <= r; ++x) {
                    const auto px = std::int64_t(center.x) + x;
                    const auto py = std::int64_t(center.y) + y;
                    const auto pz = std::int64_t(center.z) + z;
                    if (px < std::numeric_limits<int>::min() || px > std::numeric_limits<int>::max() ||
                        py < std::numeric_limits<int>::min() || py > std::numeric_limits<int>::max() ||
                        pz < std::numeric_limits<int>::min() || pz > std::numeric_limits<int>::max()) {
                        stop(Phase::failed,StopReason::captureUnknown);
                        return false;
                    }
                    positions_.push_back({static_cast<int>(px),static_cast<int>(py),static_cast<int>(pz)});
                }
        return true;
    }

    // Repeated request on the same tick returns the same outstanding batch.
    CaptureRequest captureRequest(Tick tick) {
        checkThread();
        if (!advance(tick) || (phase_ != Phase::capturing && phase_ != Phase::waiting)) return {token_,{}};
        if (!requested_.empty()) { captureTick_=tick; return {token_,requested_}; }
        if (captureTick_ && *captureTick_ == tick) return {token_,{}};
        captureTick_ = tick;
        const auto count = std::min<std::size_t>(options_.captureCellsPerTick,positions_.size()-pass_.size());
        requested_.assign(positions_.begin()+pass_.size(),positions_.begin()+pass_.size()+count);
        return {token_,requested_};
    }
    bool submitCapture(SessionToken token, Tick tick, std::span<const Cell> cells) {
        checkThread();
        if (!(token == token_) || !advance(tick) ||
            (phase_ != Phase::capturing && phase_ != Phase::waiting) ||
            requested_.empty() || cells.empty() || cells.size() > requested_.size() || !captureTick_ || tick != *captureTick_)
            return false;
        for (std::size_t i = 0; i < cells.size(); ++i) {
            if (cells[i].position != requested_[i]) return false;
            if (cells[i].kind == CellKind::unknown ||
                (cells[i].kind == CellKind::block && cells[i].identifier.empty())) {
                stop(Phase::failed,StopReason::captureUnknown);
                return false;
            }
        }
        for (const auto& c : cells) {
            const auto index = pass_.size();
            if (!preview_.empty() && preview_[index].kind == CellKind::block && c.kind == CellKind::air)
                frozen_ = true; // Never replace the last complete preview with a removal pass.
            pass_.push_back(c);
        }
        requested_.erase(requested_.begin(),requested_.begin()+cells.size());
        stats_.captured += cells.size();
        if (pass_.size() == positions_.size()) {
            ++stats_.capturePasses;
            latest_.swap(pass_);
            pass_.clear();
            if (!frozen_) {
                preview_.swap(latest_);
                // A worker result from an earlier complete pass is now stale.
                token_.generation = nextGeneration();
            }
            phase_ = Phase::waiting;
        }
        return true;
    }
    std::optional<PreviewSnapshot> previewSnapshot() const {
        checkThread();
        if (preview_.empty()) return std::nullopt;
        return PreviewSnapshot{token_,center_,options_.radius,options_.maxBlocks,preview_,baselineHotbar_};
    }
    std::optional<ObservationSnapshot> observationSnapshot(bool previewGone) const {
        checkThread();
        if (latest_.empty()) return std::nullopt;
        return ObservationSnapshot{token_,latest_,previewGone};
    }
    bool previewFrozen() const { checkThread(); return frozen_; }

    // Call repeatedly with current hotbar while waiting for a grant. Prepared data
    // must come from prepareCandidates, using this model's unchanged snapshot.
    bool acceptCandidates(const PreparedCandidates& prepared, Tick tick, const Hotbar& hotbar) {
        checkThread();
        if (!(prepared.token == token_) || !advance(tick) || phase_ != Phase::waiting || !frozen_)
            return false;
        if (prepared.status == DiffStatus::tooManyBlocks) {
            stop(Phase::failed,StopReason::oversized); return false;
        }
        if (prepared.status != DiffStatus::ready || prepared.removed.empty()) return false;
        if (prepared.removed.size() > options_.maxBlocks) return false;
        std::map<BlockPos,const Cell*> baseline;
        for (const auto& c : preview_) baseline.emplace(c.position,&c);
        std::map<BlockPos,bool> unique;
        std::map<std::string,std::uint64_t> needed;
        std::vector<Task> tasks;
        std::size_t skipped = 0;
        for (const auto& c : prepared.removed) {
            const auto it = baseline.find(c.position);
            if (it == baseline.end() || !(*it->second == c) || !unique.emplace(c.position,true).second)
                return false;
            if (!candidateForPlacement(c) || !supportPosition(c)) { ++skipped; continue; }
            ++needed[c.placement->itemIdentifier];
            tasks.push_back(Task{c});
        }
        stats_.removed = prepared.removed.size();
        stats_.persistent = prepared.persistent;
        stats_.unsupported = skipped;
        if (tasks.empty()) { stop(Phase::failed,StopReason::noTargets); return false; }
        const auto& before = grantFloor_; const auto now = counts(hotbar);
        for (const auto& [id,count] : needed) {
            const auto oldCount = countOf(before,id), newCount = countOf(now,id);
            if (newCount <= oldCount) return false; // Multi-cell blocks may need fewer items than cells.
        }
        tasks_ = std::move(tasks);
        stats_.queued = stats_.remaining = tasks_.size();
        buildStarted_ = tick;
        phase_ = Phase::building;
        return true;
    }

    void observeHotbar(const Hotbar& hotbar){
        checkThread();if(phase_!=Phase::capturing&&phase_!=Phase::waiting)return;
        const auto now=counts(hotbar);for(auto& [id,amount]:grantFloor_)amount=std::min(amount,countOf(now,id));
    }
    // Pump every tick, even without a placement opportunity, to enforce deadlines.
    void update(WorldId world, Tick tick) {
        checkThread();
        if (world != token_.world) { clear(world); reason_ = StopReason::worldChanged; return; }
        advance(tick);
    }
    std::vector<BlockPos> inspectionPositions() const {
        checkThread(); std::vector<BlockPos> out;
        if(phase_!=Phase::building || tasks_.empty())return out;
        const auto budget=std::min<std::size_t>(options_.maxStepsPerTick,tasks_.size());
        for(std::size_t i=0;i<budget;++i){const auto& task=tasks_[(cursor_+i)%tasks_.size()];
            if(task.done||task.failed)continue;
            out.push_back(task.cell.position);
            for(unsigned face=0;face<6;++face){auto cell=task.cell;cell.placement->face=static_cast<Face>(face);
                if(auto support=supportPosition(cell))out.push_back(*support);}
        }
        std::sort(out.begin(),out.end());out.erase(std::unique(out.begin(),out.end()),out.end());return out;
    }
    bool deferPlacement(SessionToken token,std::uint64_t actionId,Tick tick) {
        checkThread();if(!(token==token_)||!advance(tick)||activeAction_!=actionId)return false;
        for(auto& task:tasks_)if(task.action==actionId){activeAction_.reset();task.inFlight=false;
            if(task.attempts)--task.attempts;if(stats_.attempts)--stats_.attempts;lastPlacement_.reset();return true;}
        return false;
    }
    std::optional<PlacementRequest> nextPlacement(const BuildFrame& frame) {
        checkThread();
        update(frame.world,frame.tick);
        if (phase_ != Phase::building) return std::nullopt;
        if (stepTick_ && *stepTick_ == frame.tick) return std::nullopt;
        stepTick_ = frame.tick;
        scan_={};
        std::map<BlockPos,const Cell*> current;
        for (const auto& c : frame.cells)
            if (!current.emplace(c.position,&c).second) return std::nullopt;
        // Each call visits at most maxStepsPerTick tasks. The cursor ensures fairness.
        const auto budget = std::min<std::size_t>(options_.maxStepsPerTick,tasks_.size());
        for (std::size_t step = 0; step < budget; ++step) {
            auto& task = tasks_[cursor_++ % tasks_.size()];
            if (task.done || task.failed) continue;
            ++scan_.visited;
            const auto target = current.find(task.cell.position);
            if (target != current.end() && sameState(task.cell,*target->second)) {
                if(!task.inFlight || frame.tick-task.sent>=3)confirm(task);
                continue;
            }
            if (task.inFlight) {
                if (frame.tick-task.sent < options_.confirmationTicks) {++scan_.inFlight;continue;}
                task.inFlight = false;
            }
            if (task.attempts >= options_.maxAttempts) { exhaust(task); continue; }
            if (activeAction_ || (lastPlacement_ && frame.tick-*lastPlacement_ < options_.buildIntervalTicks)) {++scan_.cooldown;continue;}
            if (target == current.end() || target->second->kind == CellKind::unknown) {++scan_.unknown;continue;}
            if (target->second->kind != CellKind::air) {++scan_.occupied;continue;}
            auto recipe = *task.cell.placement;
            auto support = *supportPosition(task.cell);
            auto supportCell = current.find(support);
            if(task.cell.nativePlacementCandidate){
                bool found=false;double bestDistance=std::numeric_limits<double>::infinity();
                for(unsigned candidate:{1u,2u,3u,4u,5u,0u}){auto c=task.cell;c.placement->face=static_cast<Face>(candidate);
                    auto pos=supportPosition(c);if(!pos)continue;auto it=current.find(*pos);
                    if(it==current.end()||it->second->kind!=CellKind::block||!it->second->supportsPlacement)continue;
                    auto proposed=recipe;proposed.face=static_cast<Face>(candidate);
                    proposed.hitX=candidate==4?0.f:candidate==5?1.f:.5f;
                    proposed.hitY=candidate==0?0.f:candidate==1?1.f:.5f;
                    proposed.hitZ=candidate==2?0.f:candidate==3?1.f:.5f;
                    const double x=double(pos->x)+proposed.hitX-frame.eye.x,y=double(pos->y)+proposed.hitY-frame.eye.y,z=double(pos->z)+proposed.hitZ-frame.eye.z;
                    const double distance=x*x+y*y+z*z;
                    if(distance<bestDistance){bestDistance=distance;support=*pos;supportCell=it;recipe=proposed;found=true;}
                }
                if(!found){++scan_.noSupport;continue;}
            }
            if(supportCell==current.end()||supportCell->second->kind!=CellKind::block||!supportCell->second->supportsPlacement){++scan_.noSupport;continue;}
            // A planned support is usable only after its exact state is observed.
            const auto dependency = std::find_if(tasks_.begin(),tasks_.end(),[&](const Task& t) {
                return t.cell.position == support;
            });
            if (dependency != tasks_.end() && !sameState(dependency->cell,*supportCell->second)) {++scan_.dependency;continue;}
            const double dx = double(support.x)+recipe.hitX-frame.eye.x;
            const double dy = double(support.y)+recipe.hitY-frame.eye.y;
            const double dz = double(support.z)+recipe.hitZ-frame.eye.z;
            const double distance2 = dx*dx+dy*dy+dz*dz;
            if (!std::isfinite(distance2) || distance2 > options_.maxReach*options_.maxReach) {++scan_.outOfReach;continue;}
            std::optional<unsigned> slot;
            for (unsigned i = 0; i < frame.hotbar.size(); ++i)
                if (frame.hotbar[i].count && frame.hotbar[i].identifier == recipe.itemIdentifier) { slot = i; break; }
            if (!slot) {++scan_.missingItem;continue;}
            const auto action = ++actionSequence_;
            ++task.attempts;
            ++stats_.attempts;
            task.action = action;
            task.sent = frame.tick;
            task.inFlight = true;
            activeAction_ = action;
            lastPlacement_ = frame.tick;
            return PlacementRequest{token_,action,task.cell.position,support,recipe,*slot,Hand::main};
        }
        finishIfDone();
        return std::nullopt;
    }
    bool placementResult(SessionToken token, std::uint64_t actionId, bool dispatched, Tick tick) {
        checkThread();
        if (!(token == token_) || !advance(tick) || phase_ != Phase::building || activeAction_ != actionId)
            return false;
        for (auto& task : tasks_) if (task.action == actionId && task.inFlight) {
            activeAction_.reset();
            if (!dispatched) { task.inFlight = false; ++stats_.dispatchFailures; }
            if(dispatched)task.sent=tick;
            // A successful native return is NOT server confirmation.
            return true;
        }
        return false;
    }
    bool observeBlock(SessionToken token, Tick tick, const Cell& cell) {
        checkThread();
        if (!(token == token_) || !advance(tick) || phase_ != Phase::building) return false;
        for (auto& task : tasks_) if (!task.done && !task.failed && sameState(task.cell,cell) &&
                                     task.cell.position == cell.position) {
            confirm(task);
            finishIfDone();
            return true;
        }
        return false;
    }
    std::optional<std::uint64_t> recordedState(BlockPos pos) const {
        checkThread();const auto r=static_cast<std::int64_t>(options_.radius);
        const auto x=std::int64_t(pos.x)-center_.x+r,y=std::int64_t(pos.y)-center_.y+r,z=std::int64_t(pos.z)-center_.z+r;
        const auto side=2*r+1;if(x<0||x>=side||y<0||y>=side||z<0||z>=side)return {};
        const auto index=static_cast<std::size_t>((z*side+y)*side+x);
        return index<preview_.size()?preview_[index].stateIdentity:std::nullopt;
    }
    bool actionPending(SessionToken token,std::uint64_t id) const {
        checkThread();return token==token_&&phase_==Phase::building&&activeAction_==id;
    }
    bool refreshPlacement(SessionToken token,std::uint64_t id,Tick tick) {
        checkThread();if(!actionPending(token,id))return false;
        for(auto& task:tasks_)if(task.action==id){task.sent=tick;return true;}return false;
    }
    SessionToken token() const { checkThread(); return token_; }
    Phase phase() const { checkThread(); return phase_; }
    StopReason stopReason() const { checkThread(); return reason_; }
    Stats stats() const { checkThread(); return stats_; }
    PlacementScan placementScan() const {checkThread();return scan_;}

private:
    static std::uint64_t nextGeneration(){static std::atomic<std::uint64_t> counter{};return counter.fetch_add(1,std::memory_order_relaxed)+1;}
    struct Task {
        Cell cell;
        unsigned attempts{};
        Tick sent{};
        std::uint64_t action{};
        bool inFlight{}, done{}, failed{};
    };
    using Counts = std::map<std::string,std::uint64_t>;
    static Counts counts(const Hotbar& hotbar) {
        Counts result;
        for (const auto& slot : hotbar) if (!slot.identifier.empty()) result[slot.identifier] += slot.count;
        return result;
    }
    static std::uint64_t countOf(const Counts& c, const std::string& id) {
        const auto it = c.find(id);
        return it == c.end() ? 0 : it->second;
    }
    void checkThread() const {
        if (std::this_thread::get_id() != owner_) throw std::logic_error("SpeedBuilder Model accessed off owner thread");
    }
    void clear(WorldId world) {
        token_ = {world,nextGeneration()};
        phase_ = Phase::idle;
        reason_ = StopReason::none;
        stats_ = {};scan_={};
        positions_.clear(); requested_.clear(); pass_.clear(); preview_.clear(); latest_.clear(); tasks_.clear();
        baselineHotbar_ = {};grantFloor_.clear();
        frozen_ = promptVisible_ = false;
        cursor_ = 0;
        captureTick_.reset(); stepTick_.reset(); lastPlacement_.reset(); activeAction_.reset();
        started_ = buildStarted_ = lastTick_ = 0;
    }
    void stop(Phase phase, StopReason reason) {
        token_.generation = nextGeneration();
        phase_ = phase;
        reason_ = reason;
        requested_.clear();
        activeAction_.reset();
    }
    bool advance(Tick tick) {
        if (tick < lastTick_) return false;
        lastTick_ = tick;
        if ((phase_ == Phase::capturing || phase_ == Phase::waiting) && tick-started_ >= options_.maxWaitTicks)
            stop(Phase::failed,StopReason::timeout);
        if (phase_ == Phase::building && tick-buildStarted_ >= options_.maxBuildTicks)
            stop(Phase::failed,StopReason::timeout);
        if (activeAction_) {
            for (auto& task : tasks_) if (task.action == *activeAction_ && tick-task.sent >= options_.confirmationTicks) {
                activeAction_.reset();
                task.inFlight = false;
                ++stats_.dispatchFailures;
                break;
            }
        }
        return phase_ != Phase::failed && phase_ != Phase::cancelled;
    }
    void confirm(Task& task) {
        task.done = true;
        task.inFlight = false;
        if (activeAction_ == task.action) activeAction_.reset();
        ++stats_.confirmed;
        --stats_.remaining;
    }
    void exhaust(Task& task) {
        task.failed = true;
        ++stats_.exhausted;
        --stats_.remaining;
    }
    void finishIfDone() {
        if (stats_.remaining == 0) {
            phase_ = stats_.exhausted ? Phase::failed : Phase::complete;
        }
    }
    Options options_;
    std::thread::id owner_;
    SessionToken token_;
    std::uint64_t actionSequence_{};
    PlacementScan scan_{};
    Phase phase_ = Phase::idle;
    StopReason reason_ = StopReason::none;
    Stats stats_;
    BlockPos center_;
    Hotbar baselineHotbar_;Counts grantFloor_;
    std::vector<BlockPos> positions_, requested_;
    std::vector<Cell> pass_, preview_, latest_;
    std::vector<Task> tasks_;
    bool frozen_{}, promptVisible_{};
    std::size_t cursor_{};
    Tick started_{}, buildStarted_{}, lastTick_{};
    std::optional<Tick> captureTick_, stepTick_, lastPlacement_;
    std::optional<std::uint64_t> activeAction_;
};

} // namespace speed_builder
