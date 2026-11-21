#include <bedrock/sdk/commands.hpp>
#include <bedrock/sdk/profiles.hpp>
#include "profile_facade.hpp"
#ifdef _WIN32
#include <bedrock/native/injected_commands.hpp>
#include <bedrock/profiles/profile_manager.hpp>
#endif

#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace bedrock::sdk {

struct Commands::Impl {
#ifdef _WIN32
    bedrock::NativeInjectedCommands native;
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

    bedrock::ProfileManager manager;
    if (!manager.prepareEmbeddedJson(profileJson,
        [this](const bedrock::RuntimeProfile& profile, std::string& loadError) {
            return impl_->native.load(profile, loadError);
        }, error))
        return false;
    error.clear();
    return true;
}

bool Commands::load(const Profile& profile, const std::vector<CommandEnum>& enums,
                    const std::vector<CommandDefinition>& commands, std::string& error) {
    const auto runtime = ProfileRuntimeAccess::runtime(profile);
    if (!runtime) return load(profile.json(), enums, commands, error);

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
    if (!impl_->native.load(*runtime, error)) return false;
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
    return impl_->native.replaceDefinitions(std::move(enumEntries), std::move(nativeCommands), error);
}

bool Commands::isLoaded() const noexcept { return impl_->native.isLoaded(); }
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
#endif

} // namespace bedrock::sdk
