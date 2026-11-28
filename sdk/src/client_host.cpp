#include <bedrock/sdk/client_host.hpp>
#include <bedrock/sdk/client_info.hpp>
#include "native_game_frame.hpp"

#include <bedrock/native/content_log.hpp>
#include <bedrock/runtime/game_hooks.hpp>
#include <algorithm>
#include <atomic>
#include <mutex>

namespace bedrock::sdk {
namespace {
thread_local bool inHostCallback{};
class HostCallbackScope {
public:
    HostCallbackScope() { inHostCallback = true; }
    ~HostCallbackScope() { inHostCallback = false; }
};

bool supportedKey(unsigned key) {
    return key == 9 || key == 32 || (key >= '0' && key <= '9') ||
        (key >= 'A' && key <= 'Z') || (key >= 112 && key <= 123);
}
}

struct ClientHost::Impl {
    detail::NativeGameBindings game;
    GameHooks hooks{game.dispatch};
    std::vector<Mod*> mods;
    ErrorHandler errorHandler;
    std::shared_ptr<const RuntimeProfile> profile;
    std::recursive_mutex dispatchMutex;
    std::atomic<bool> ready{};
    std::atomic<void*> client{};
    std::atomic<std::uint64_t> worldRevision{}, dimensionRevision{};
    void* lastLevel{};
    std::uint64_t lastWorldRevision{};
    std::shared_ptr<detail::WorldSession> session;
    bool gameplay{};

    Impl(Mod& mod, ErrorHandler handler) : mods{&mod}, errorHandler(std::move(handler)) {}
    ~Impl() { game.packets.stop(); hooks.stop(); }

    void report(const Error& error) noexcept {
        if (errorHandler) {
            try { errorHandler(error); return; } catch (...) {}
        }
        OutputDebugStringA(error.message.c_str());
    }

    template<class Action>
    void dispatch(Action action) noexcept {
        if (!ready.load() || inHostCallback) return;
        HostCallbackScope callback;
        try {
            std::lock_guard lock(dispatchMutex);
            if (!ready.load()) return;
            const auto result = action();
            if (!result) report(result.error());
        } catch (const std::exception& error) {
            report({ErrorCode::Internal, error.what()});
        } catch (...) {
            report({ErrorCode::Internal, "Unhandled client host callback failure"});
        }
    }

    Result<void> tick(void* level) {
        const auto world = worldRevision.load();
        if (!session || lastLevel != level || lastWorldRevision != world) {
            session = std::make_shared<detail::WorldSession>();
            lastLevel = level;
            lastWorldRevision = world;
        }
        const auto dimension = dimensionRevision.load();
        detail::NativeGameFrame frame{game, client.load(), level, dimension};
        frame.session = session;
        frame.dimensionRevision = dimension;
        detail::GameFrameScope scope{frame};
        // One WorldSession / GameFrame for all attached Mods. A failing Mod
        // is reported but never prevents the remaining Mods from ticking
        // (§7.1). Take a copy so detach() during a callback cannot invalidate
        // the iteration.
        // tick() runs inside dispatch() with dispatchMutex already held.
        std::vector<Mod*> current = mods;
        for (Mod* mod : current) {
            frame.world = &mod->world();
            try {
                const auto result = mod->update();
                if (!result) report(result.error());
            } catch (const std::exception& error) {
                report({ErrorCode::Internal, error.what()});
            } catch (...) {
                report({ErrorCode::Internal, "Mod update failed"});
            }
        }
        return Result<void>::ok();
    }

    bool connect(const RuntimeProfile& profile, std::string& error) {
        const auto& resolver = profile.resolver();
        game.dispatch.on<WorldChangeEvent>([this](const auto& event) {
            ++dimensionRevision;
            if (event.reason != WorldChangeReason::DimensionChanged) ++worldRevision;
            if (event.reason == WorldChangeReason::Joined) {
                client.store(event.clientInstance);
                game.access.observeClient(event.clientInstance);
            }
        });
        game.dispatch.on<GameplayEvent>([this](const auto& event) {
            client.store(event.clientInstance);
            dispatch([&] {
                gameplay = event.active;
                if (!gameplay)
                    for (Mod* mod : mods) mod->keybinds().clearInput();
                return Result<void>::ok();
            });
        });
        game.dispatch.on<NativeKeyEvent>([this](const auto& event) {
            if (supportedKey(event.inputKey))
                dispatch([&] {
                    if (!gameplay) return Result<void>::ok();
                    for (Mod* mod : mods) {
                        const auto result = mod->dispatchKey(static_cast<Key>(event.inputKey),
                                                             event.pressed);
                        if (!result) report(result.error());
                    }
                    return Result<void>::ok();
                });
        });
        game.dispatch.on<LevelTickEvent>([this](const auto& event) {
            dispatch([&] { return tick(event.level); });
        });
        if (!game.installPacketReceive(profile, error)) return false;
        return hooks.installWorldEvents(resolver, profile.json(), error) &&
            hooks.installGameplay(resolver, error) &&
            hooks.installNativeKeyboard(resolver, profile.json(), error) &&
            hooks.installActorNameTag(resolver, error) &&
            hooks.installLevelTick(resolver, error);
    }
};

ClientHost::ClientHost() = default;
ClientHost::~ClientHost() {
    // The owning loader must destroy the host outside callbacks.
    if (const auto stopped = stop(); !stopped) OutputDebugStringA(stopped.error().message.c_str());
}

Result<void> ClientHost::start(Mod& mod, ErrorHandler onError) {
    if (inHostCallback || impl_)
        return Result<void>::error(ErrorCode::Invalid, "Cannot start an active/reentrant client host");
    try {
        HostCallbackScope callback;
        ProfileManager profiles;
        std::string error;
        if (!profiles.open({}, {UpdatePolicy::EmbeddedOnly, CandidatePolicy::AllowCandidates}, error))
            return detail::unavailable(std::move(error));
        auto candidate = std::make_unique<Impl>(mod, std::move(onError));
        const auto profile = profiles.snapshot();
        candidate->profile = profile;
        if (!candidate->game.load(*profile, error)) return detail::unavailable(std::move(error));

        // Primary log surface: the game's vanilla Content Log viewer. Install
        // after the exact-build profile is validated, but before mod.enable(),
        // so every SDK Logger in this module (including JS LogSink lines from
        // the ScriptClient host) is routed there automatically. Fails closed
        // to the existing file/debug sinks when the symbol is unavailable.
        std::string contentLogError;
        if (!bedrock::native::NativeContentLog::instance().load(*profile, contentLogError)) {
            OutputDebugStringA(("[BedrockSDK] Content Log unavailable; keeping file/debug logs only: " +
                                contentLogError + "\n").c_str());
        } else {
            bedrock::native::NativeContentLog::instance().install();
        }

        if (auto enabled = mod.enable(); !enabled) return enabled;
        try {
            if (!candidate->connect(*profile, error)) {
                candidate->hooks.stop();
                if (const auto disabled = mod.disable(); !disabled) candidate->report(disabled.error());
                return detail::unavailable(std::move(error));
            }
        } catch (...) {
            candidate->hooks.stop();
            if (const auto disabled = mod.disable(); !disabled) candidate->report(disabled.error());
            throw;
        }
        impl_ = std::move(candidate);
        impl_->ready.store(true);
        // Client diagnostics are best-effort: a missing sampler never fails the
        // host. FPS/ping/CPS read zero until the sources are available.
        std::string clientInfoError;
        if (!ClientInfo::install(profile->json(), profile->json(), clientInfoError))
            OutputDebugStringA(("[BedrockSDK] Client diagnostics unavailable: " +
                                clientInfoError + "\n").c_str());
        else if (!clientInfoError.empty())
            OutputDebugStringA(("[BedrockSDK] Client diagnostics partial: " +
                                clientInfoError + "\n").c_str());
        return Result<void>::ok();
    } catch (const std::exception& error) {
        return Result<void>::error(ErrorCode::Internal, error.what());
    } catch (...) {
        return Result<void>::error(ErrorCode::Internal, "Unhandled client host startup failure");
    }
}

Result<void> ClientHost::stop() {
    if (inHostCallback)
        return Result<void>::error(ErrorCode::Invalid, "Cannot stop the client host from a callback");
    if (!impl_) return Result<void>::ok();
    HostCallbackScope callback;
    auto host = std::move(impl_);
    host->ready.store(false);
    host->hooks.stop(); // The shared broker drains in-flight subscribers.
    ClientInfo::stop();
    Result<void> firstError = Result<void>::ok();
    for (Mod* mod : host->mods) {
        const auto result = mod->disable();
        if (!result && !firstError) firstError = result;
    }
    host->mods.clear();
    return firstError;
}

Result<void> ClientHost::attach(Mod& mod) {
    if (!impl_ || !impl_->ready.load() || inHostCallback)
        return Result<void>::error(ErrorCode::Invalid,
                                   "Cannot attach a mod to an inactive/reentrant client host");
    std::lock_guard lock(impl_->dispatchMutex);
    if (std::find(impl_->mods.begin(), impl_->mods.end(), &mod) != impl_->mods.end())
        return Result<void>::error(ErrorCode::AlreadyRegistered, "Mod already attached");
    if (const auto enabled = mod.enable(); !enabled) return enabled;
    impl_->mods.push_back(&mod);
    return Result<void>::ok();
}

Result<void> ClientHost::detach(Mod& mod) {
    if (!impl_ || inHostCallback)
        return Result<void>::error(ErrorCode::Invalid,
                                   "Cannot detach a mod from a reentrant client host");
    std::lock_guard lock(impl_->dispatchMutex);
    const auto it = std::find(impl_->mods.begin(), impl_->mods.end(), &mod);
    if (it == impl_->mods.end())
        return Result<void>::error(ErrorCode::NotFound, "Mod is not attached");
    impl_->mods.erase(it); // no further callbacks are delivered
    return mod.disable();
}

Result<void> ClientHost::synchronize(const std::function<void()>& operation) {
    if (!impl_ || !impl_->ready.load() || inHostCallback || !operation)
        return Result<void>::error(ErrorCode::Invalid, "Cannot synchronize an inactive/reentrant client host");
    std::lock_guard lock(impl_->dispatchMutex);
    operation();
    return Result<void>::ok();
}

std::shared_ptr<const RuntimeProfile> ClientHost::profileSnapshot() const {
    return impl_ ? impl_->profile : nullptr;
}
} // namespace bedrock::sdk
