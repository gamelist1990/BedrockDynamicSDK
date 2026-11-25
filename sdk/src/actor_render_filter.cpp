#include <bedrock/sdk/actor_render_filter.hpp>

#ifdef _WIN32
#include <bedrock/generated/symbols.hpp>
#include <bedrock/native/scene_actor_exclusion.hpp>
#include <bedrock/profiles/profile_json.hpp>
#endif

namespace bedrock::sdk {
namespace {
struct NoExclusion {
    static bool policy(void*) noexcept { return false; }
};
} // namespace

struct ActorRenderFilter::Impl {
    bool probe{};
#ifdef _WIN32
    bedrock::NativeSceneActorExclusion native;
    ActorRenderPolicy policy{&NoExclusion::policy};
#endif
};

ActorRenderFilter::ActorRenderFilter() : impl_(std::make_unique<Impl>()) {}
ActorRenderFilter::~ActorRenderFilter() = default;
ActorRenderFilter::ActorRenderFilter(ActorRenderFilter&&) noexcept = default;
ActorRenderFilter& ActorRenderFilter::operator=(ActorRenderFilter&&) noexcept = default;

void ActorRenderFilter::setProbe(bool enabled) noexcept {
    impl_->probe = enabled;
#ifdef _WIN32
    impl_->native.setProbe(enabled);
#endif
}

bool ActorRenderFilter::probe() const noexcept {
    return impl_->probe;
}

bool ActorRenderFilter::configure(const Profile& profile, ActorRenderPolicy policy,
                                  std::string& error, std::vector<std::string>& warnings) {
#ifdef _WIN32
    warnings.clear();
    const auto runtime = ProfileRuntimeAccess::runtime(profile);
    if (!runtime) {
        error = "scene-actor exclusion profile runtime snapshot unavailable";
        return false;
    }
    try {
        if (policy) impl_->policy = policy;
        std::string filterError;
        if (!impl_->native.install(*runtime, bedrock::generated::kSymbolDatabase, impl_->policy,
                                   impl_->probe, filterError)) {
            error = filterError;
            return false;
        }
        warnings.push_back(
            "RenderDragon scene-actor extraction filter ready [per-object predicate, original "
            "answer preserved for every other object]");
        error.clear();
        return true;
    } catch (const std::exception& e) {
        error = e.what();
        return false;
    }
#else
    (void)profile;
    (void)policy;
    (void)warnings;
    error = "scene-actor exclusion is unavailable on this host";
    return false;
#endif
}

bool ActorRenderFilter::ready() const noexcept {
#ifdef _WIN32
    return impl_->native.ready();
#else
    return false;
#endif
}

ActorRenderFilterStatistics ActorRenderFilter::takeStatistics() noexcept {
    ActorRenderFilterStatistics result{};
#ifdef _WIN32
    const auto statistics = impl_->native.takeStatistics();
    result.ready = impl_->native.ready();
    result.calls = statistics.calls;
    result.permitted = statistics.permitted;
    result.excluded = statistics.excluded;
    result.variants = statistics.tableMask;
#endif
    return result;
}

std::vector<ActorRenderProbeSample> ActorRenderFilter::takeProbeSamples() {
    std::vector<ActorRenderProbeSample> result;
#ifdef _WIN32
    for (const auto& sample : impl_->native.takeSamples()) {
        ActorRenderProbeSample copy{};
        copy.table = sample.table;
        copy.object = sample.object;
        copy.context = sample.context;
        for (unsigned word = 0; word < ActorRenderProbeSample::kSampleWords; ++word)
            copy.words[word] = sample.words[word];
        for (unsigned word = 0; word < ActorRenderProbeSample::kContextWords; ++word)
            copy.contextWords[word] = sample.contextWords[word];
        copy.calls = sample.calls;
        result.push_back(copy);
    }
#endif
    return result;
}

} // namespace bedrock::sdk
