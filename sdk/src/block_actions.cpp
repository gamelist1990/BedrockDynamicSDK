#include "bedrock/sdk/block_actions.hpp"

#include "profile_facade.hpp"
#include "world_access_bridge.hpp"

#ifdef _WIN32
#include <bedrock/detail/block_action_report.hpp>
#include <bedrock/native/block_actions.hpp>
#endif

namespace bedrock::sdk {

struct BlockActions::Impl {
#ifdef _WIN32
    bedrock::NativeBlockActions native;
#endif
};

BlockActions::BlockActions() : impl_(std::make_unique<Impl>()) {}
BlockActions::~BlockActions() = default;
BlockActions::BlockActions(BlockActions&&) noexcept = default;
BlockActions& BlockActions::operator=(BlockActions&&) noexcept = default;

#ifdef _WIN32
namespace {
bedrock::BlockActionFailure toNative(BlockActionFailureValue failure) noexcept {
    return static_cast<bedrock::BlockActionFailure>(static_cast<int>(failure));
}
BlockActionFailureValue fromNative(bedrock::BlockActionFailure failure) noexcept {
    return static_cast<BlockActionFailureValue>(static_cast<int>(failure));
}
BlockActionReportValue fromNative(const bedrock::BlockActionReport& report) {
    BlockActionReportValue result;
    result.failure = fromNative(report.failure);
    result.virtualSlot = report.virtualSlot;
    result.observedFunctionRva = report.observedFunctionRva;
    result.called = report.called;
    result.accepted = report.accepted;
    result.progressBefore = report.progressBefore;
    result.idleStopsSuppressed = report.idleStopsSuppressed;
    return result;
}
bedrock::BlockActionReport toNative(const BlockActionReportValue& report) {
    bedrock::BlockActionReport result;
    result.failure = toNative(report.failure);
    result.virtualSlot = report.virtualSlot;
    result.observedFunctionRva = report.observedFunctionRva;
    result.called = report.called;
    result.accepted = report.accepted;
    result.progressBefore = report.progressBefore;
    result.idleStopsSuppressed = report.idleStopsSuppressed;
    return result;
}
bedrock::BlockPos toNative(BlockPos pos) noexcept { return {pos.x, pos.y, pos.z}; }
bedrock::Vec3 toNative(Vec3Value v) noexcept { return {v.x, v.y, v.z}; }
} // namespace
#endif

bool BlockActions::load(const Profile& profile, WorldAccess& world, std::string& error) {
#ifdef _WIN32
    const auto runtime = ProfileRuntimeAccess::runtime(profile);
    if (!runtime) {
        error = "block actions profile runtime snapshot unavailable";
        return false;
    }
    auto* nativeWorld = static_cast<bedrock::NativeWorldAccess*>(
        detail::nativeWorldAccessHandle(world));
    if (!nativeWorld) {
        error = "block actions require the native world backend";
        return false;
    }
    return impl_->native.load(*runtime, *nativeWorld, error);
#else
    (void)profile;
    (void)world;
    error = "block actions unavailable on this host";
    return false;
#endif
}

bool BlockActions::mine(void* player, BlockPos block, unsigned face, Vec3Value eye,
                        Vec3Value hit, bool begin, bool& destroyed,
                        BlockActionReportValue* report) const noexcept {
    destroyed = false;
#ifdef _WIN32
    bedrock::BlockActionReport nativeReport;
    const bool ok = impl_->native.mine(player, toNative(block), face, toNative(eye),
                                       toNative(hit), begin, destroyed,
                                       report ? &nativeReport : nullptr);
    if (report)
        *report = fromNative(nativeReport);
    return ok;
#else
    (void)player;
    (void)block;
    (void)face;
    (void)eye;
    (void)hit;
    (void)begin;
    if (report)
        *report = {};
    return false;
#endif
}

bool BlockActions::destroyState(void* player, float& progress, BlockPos& target) const noexcept {
#ifdef _WIN32
    bedrock::BlockPos nativeTarget{};
    const bool ok = impl_->native.destroyState(player, progress, nativeTarget);
    target = {nativeTarget.x, nativeTarget.y, nativeTarget.z};
    return ok;
#else
    (void)player;
    progress = 0;
    target = {-1, -1, -1};
    return false;
#endif
}

bool BlockActions::stop(void* player, BlockPos block) const noexcept {
#ifdef _WIN32
    return impl_->native.stop(player, toNative(block));
#else
    (void)player;
    (void)block;
    return false;
#endif
}

bool BlockActions::useItem(void* player) const noexcept {
#ifdef _WIN32
    return impl_->native.useItem(player);
#else
    (void)player;
    return false;
#endif
}

bool BlockActions::releaseItem(void* player) const noexcept {
#ifdef _WIN32
    return impl_->native.releaseItem(player);
#else
    (void)player;
    return false;
#endif
}

bool BlockActions::attackActor(void* player, void* target) const noexcept {
#ifdef _WIN32
    return impl_->native.attackActor(player, target);
#else
    (void)player;
    (void)target;
    return false;
#endif
}

void BlockActions::cancelMiningOwnership() const noexcept {
#ifdef _WIN32
    impl_->native.cancelMiningOwnership();
#endif
}

std::string BlockActions::diagnostic(const BlockActionReportValue& report) {
#ifdef _WIN32
    return bedrock::NativeBlockActions::diagnostic(toNative(report));
#else
    (void)report;
    return "block actions unavailable on this host";
#endif
}

} // namespace bedrock::sdk
