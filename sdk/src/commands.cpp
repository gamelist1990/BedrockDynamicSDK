#include <bedrock/sdk/commands.hpp>
#include <bedrock/sdk/profiles.hpp>
#include "profile_facade.hpp"
#ifdef _WIN32
#include <bedrock/native/injected_commands.hpp>
#include <bedrock/native/message_commands.hpp>
#include <bedrock/profiles/profile_manager.hpp>
#endif

#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace bedrock::sdk {

struct Commands::Impl {
#ifdef _WIN32
    bedrock::NativeInjectedCommands native;
    bedrock::NativeMessageCommands localNative;
#endif
    // Local-registry fallback state (see commands.hpp). The native
    // objects above are Windows-only; the bookkeeping is not, so host
    // builds keep the same shape and simply never activate.
    bool localEnabled{};
    bool preferLocal{};
    bool localLoadAttempted{};
    bool localLoaded{};
    std::string localError;
    // Name -> current handler for local-registry dispatch. The packet
    // path calls each definition's handler directly; the local path
    // resolves through this map so replace() can swap handlers.
    std::mutex localMutex;
    std::unordered_map<std::string, std::function<void(const std::string&)>> localHandlers;
    std::unordered_set<std::string> localRegistered;
    enum class LocalSession { Idle, Active };
    LocalSession localSession{LocalSession::Idle};
    std::string localState{"idle"};
    std::chrono::steady_clock::time_point lastDrive{};

#ifdef _WIN32
    // The local path delivers "/name remainder" (the registry parser's
    // message argument); the packet path delivers the line without the
    // leading slash. Normalize so both reach the handler in one form.
    void dispatchLocal(const std::string& name, std::string line) {
        if (!line.empty() && line.front() == '/') line.erase(line.begin());
        while (!line.empty() && line.back() == ' ') line.pop_back();
        std::function<void(const std::string&)> handler;
        {
            std::lock_guard lock(localMutex);
            const auto found = localHandlers.find(name);
            if (found != localHandlers.end()) handler = found->second;
        }
        if (handler) handler(line);
    }

    // Mirror one definition set into the local registry bookkeeping.
    // Registration itself is only possible around the adapter's
    // load(): add() before it, addLoaded() after a successful one.
    // When the local load was attempted and failed, definitions are
    // skipped entirely.
    void mirrorLocal(const std::vector<CommandDefinition>& commands) {
        {
            std::lock_guard lock(localMutex);
            for (const auto& c : commands) localHandlers[c.name] = c.handler;
        }
        if (!localEnabled) return;
        if (localLoadAttempted && !localLoaded) return;
        for (const auto& c : commands) {
            if (localRegistered.count(c.name)) continue;
            auto handler = [this, name = c.name](bedrock::NativeMessageInvocation invocation) {
                dispatchLocal(name, std::move(invocation.line));
            };
            const bool ok = localLoaded
                ? localNative.addLoaded(c.name, c.description, std::move(handler), "message", true)
                : localNative.add(c.name, c.description, std::move(handler), "message", true);
            if (ok) localRegistered.insert(c.name);
        }
    }

    // Best-effort: a local-registry failure is recorded, never
    // propagated. The message-forwarding layout is candidate-status
    // profile data, so the adapter is loaded with an explicit
    // candidate opt-in view of the caller's snapshot (the snapshot
    // itself is never mutated).
    void loadLocal(const bedrock::RuntimeProfile& profile) {
        if (!localEnabled) return;
        localLoadAttempted = true;
        const auto view = profile.withCandidatePolicy(bedrock::CandidatePolicy::AllowCandidates);
        const bedrock::RuntimeProfile& effective = view ? *view : profile;
        std::string error;
        if (!localNative.load(effective, error)) {
            localError = std::move(error);
            localLoaded = false;
            return;
        }
        localLoaded = true;
        localError.clear();
    }
#endif
};

Commands::Commands(): impl_(std::make_unique<Impl>()) {}
Commands::~Commands() = default;

#ifdef _WIN32
bedrock::InjectedType toNative(CommandParameterType type, const std::string& enumName) {
    switch (type) {
        case CommandParameterType::Integer: return bedrock::InjectedType::integer();
        case CommandParameterType::Message: return bedrock::InjectedType::message();
        case CommandParameterType::PacketEnum: return bedrock::InjectedType::packetEnum(enumName);
        default: return bedrock::InjectedType::customEnum(enumName);
    }
}

bool Commands::load(const std::string& profileJson, const std::vector<CommandEnum>& enums,
                    const std::vector<CommandDefinition>& commands, std::string& error) {
    if (impl_->preferLocal) {
        impl_->mirrorLocal(commands);
        bedrock::ProfileManager manager;
        if (!manager.prepareEmbeddedJson(profileJson,
            [this](const bedrock::RuntimeProfile& profile, std::string& loadError) {
                impl_->loadLocal(profile);
                if (!impl_->localLoaded) { loadError = impl_->localError; return false; }
                return true;
            }, error))
            return false;
        error.clear();
        return true;
    }
    for (const auto& e : enums) {
        std::string enumError;
        if (!impl_->native.addEnum(e.name, e.values, enumError) && error.empty()) error = enumError;
    }
    for (const auto& c : commands) {
        bedrock::NativeInjectedCommands::CommandEntry entry;
        entry.name = c.name;
        entry.description = c.description;
        for (const auto& overload : c.overloads) {
            std::vector<bedrock::InjectedParam> params;
            for (const auto& p : overload)
                params.push_back({p.name, toNative(p.type, p.enumName), p.optional, 0});
            entry.overloads.push_back(std::move(params));
        }
        entry.handler = c.handler;
        std::string commandError;
        if (!impl_->native.add(entry.name, entry.description, entry.overloads, entry.handler, commandError) && error.empty())
            error = commandError;
    }
    if (!error.empty()) return false;
    impl_->mirrorLocal(commands);

    bedrock::ProfileManager manager;
    if (!manager.prepareEmbeddedJson(profileJson,
        [this](const bedrock::RuntimeProfile& profile, std::string& loadError) {
            if (!impl_->native.load(profile, loadError)) return false;
            impl_->loadLocal(profile);
            return true;
        }, error))
        return false;
    error.clear();
    return true;
}

bool Commands::load(const Profile& profile, const std::vector<CommandEnum>& enums,
                    const std::vector<CommandDefinition>& commands, std::string& error) {
    const auto runtime = ProfileRuntimeAccess::runtime(profile);
    if (!runtime) return load(profile.json(), enums, commands, error);
    if (impl_->preferLocal) {
        // Local-only mode: the packet rewrite is what makes these
        // names un-executable in singleplayer (they become server
        // commands whose requests the local path never intercepts),
        // so definitions go only to the native registry adapter.
        impl_->mirrorLocal(commands);
        impl_->loadLocal(*runtime);
        if (!impl_->localLoaded) { error = impl_->localError; return false; }
        error.clear();
        return true;
    }

    for (const auto& e : enums) {
        std::string enumError;
        if (!impl_->native.addEnum(e.name, e.values, enumError) && error.empty()) error = enumError;
    }
    for (const auto& c : commands) {
        bedrock::NativeInjectedCommands::CommandEntry entry;
        entry.name = c.name;
        entry.description = c.description;
        for (const auto& overload : c.overloads) {
            std::vector<bedrock::InjectedParam> params;
            for (const auto& p : overload)
                params.push_back({p.name, toNative(p.type, p.enumName), p.optional, 0});
            entry.overloads.push_back(std::move(params));
        }
        entry.handler = c.handler;
        std::string commandError;
        if (!impl_->native.add(entry.name, entry.description, entry.overloads, entry.handler, commandError) && error.empty())
            error = commandError;
    }
    if (!error.empty()) return false;
    impl_->mirrorLocal(commands);
    if (!impl_->native.load(*runtime, error)) return false;
    impl_->loadLocal(*runtime);
    error.clear();
    return true;
}

bool Commands::replace(const std::vector<CommandEnum>& enums,
                       const std::vector<CommandDefinition>& commands, std::string& error) {
    bedrock::NativeInjectedCommands::EnumEntry nativeEnums;
    (void)nativeEnums;
    std::vector<bedrock::NativeInjectedCommands::EnumEntry> enumEntries;
    for (const auto& e : enums) {
        bedrock::NativeInjectedCommands::EnumEntry entry;
        entry.name = e.name;
        entry.values = e.values;
        entry.mergedFrom = e.mergedFrom;
        entry.merged = e.merged;
        enumEntries.push_back(std::move(entry));
    }
    std::vector<bedrock::NativeInjectedCommands::CommandEntry> nativeCommands;
    for (const auto& c : commands) {
        bedrock::NativeInjectedCommands::CommandEntry entry;
        entry.name = c.name;
        entry.description = c.description;
        for (const auto& overload : c.overloads) {
            std::vector<bedrock::InjectedParam> params;
            for (const auto& p : overload)
                params.push_back({p.name, toNative(p.type, p.enumName), p.optional, 0});
            entry.overloads.push_back(std::move(params));
        }
        entry.handler = c.handler;
        nativeCommands.push_back(std::move(entry));
    }
    if (impl_->preferLocal) {
        impl_->mirrorLocal(commands);
        error.clear();
        return true;
    }
    if (!impl_->native.replaceDefinitions(std::move(enumEntries), std::move(nativeCommands), error))
        return false;
    impl_->mirrorLocal(commands);
    return true;
}

bool Commands::isLoaded() const noexcept {
    return impl_->native.isLoaded() || (impl_->preferLocal && impl_->localLoaded);
}

void Commands::enableLocalRegistry(bool enable) noexcept { impl_->localEnabled = enable; }

void Commands::preferLocalRegistry(bool prefer) noexcept {
    impl_->preferLocal = prefer;
    if (prefer) impl_->localEnabled = true;
}

void Commands::noteLocalSessionStart() noexcept {
    impl_->localSession = Impl::LocalSession::Active;
}

void Commands::noteLocalSessionEnd() noexcept {
    impl_->localSession = Impl::LocalSession::Idle;
}

void Commands::driveLocalRegistry(void* clientInstance) {
    auto& impl = *impl_;
    if (!impl.localEnabled) { impl.localState = "disabled"; return; }
    if (!impl.localLoaded) { impl.localState = "not-loaded"; return; }
    if (!clientInstance) { impl.localState = "no-client"; return; }
    if (impl.localSession == Impl::LocalSession::Idle) { impl.localState = "session-idle"; return; }
    const auto now = std::chrono::steady_clock::now();
    if (now - impl.lastDrive < std::chrono::milliseconds(200)) return;
    impl.lastDrive = now;
    // No local/remote classification: the adapter's collision rule is
    // the arbitration. On a remote session the packet path has already
    // placed these names in the registry, registration reports a
    // collision, and the existing (packet) definitions are preserved.
    // In singleplayer the names are absent and registration succeeds.
    const auto state = impl.localNative.update(clientInstance);
    using State = bedrock::NativeMessageCommands::State;
    switch (state) {
    case State::WaitingClient: impl.localState = "waiting-client"; break;
    case State::WaitingRegistry: impl.localState = "waiting-registry"; break;
    case State::WaitingTemplate: impl.localState = "waiting-template"; break;
    case State::Registered: impl.localState = "registered"; break;
    case State::Collision: impl.localState = "collision"; break;
    case State::Failed:
        impl.localState = "failed";
        impl.localError = impl.localNative.error();
        break;
    }
}

bool Commands::localRegistryActive() const noexcept {
    return impl_->localState == "registered";
}

std::string Commands::localRegistryError() const { return impl_->localError; }
std::string Commands::localRegistryState() const { return impl_->localState; }
#else
bool Commands::load(const std::string&, const std::vector<CommandEnum>&,
                    const std::vector<CommandDefinition>&, std::string& error) {
    error = "native commands unavailable";
    return false;
}
bool Commands::load(const Profile&, const std::vector<CommandEnum>&,
                    const std::vector<CommandDefinition>&, std::string& error) {
    error = "native commands unavailable";
    return false;
}
bool Commands::replace(const std::vector<CommandEnum>&,
                       const std::vector<CommandDefinition>&, std::string& error) {
    error = "native commands unavailable";
    return false;
}
bool Commands::isLoaded() const noexcept { return false; }
void Commands::enableLocalRegistry(bool) noexcept {}
void Commands::preferLocalRegistry(bool) noexcept {}
void Commands::noteLocalSessionStart() noexcept {}
void Commands::noteLocalSessionEnd() noexcept {}
void Commands::driveLocalRegistry(void*) {}
bool Commands::localRegistryActive() const noexcept { return false; }
std::string Commands::localRegistryError() const { return "native commands unavailable"; }
std::string Commands::localRegistryState() const { return "idle"; }
#endif

} // namespace bedrock::sdk
