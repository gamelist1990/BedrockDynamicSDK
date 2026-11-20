#include "command_service.hpp"

#include <algorithm>
#include <unordered_set>

namespace stageb {
namespace {

constexpr std::size_t kMaxQueuedLines = 64; // §12: bounded command queue

bedrock::InjectedType toType(const WireParam& param) {
    switch (param.kind) {
        case WireParamKind::PacketEnum:
            return bedrock::InjectedType::packetEnum(param.enumName);
        case WireParamKind::Integer:
            return bedrock::InjectedType::integer();
        case WireParamKind::Message:
            return bedrock::InjectedType::message();
        case WireParamKind::CustomEnum:
        default:
            return bedrock::InjectedType::customEnum(param.enumName);
    }
}

} // namespace

CommandService::CommandService() = default;

CommandService::DefinitionSet CommandService::makeDefinitions(
    const WireDefinitions& definitions, CommandService* owner) {
    DefinitionSet set;
    // Fault isolation: a single malformed definition must not abort the whole
    // load (which would also kill /mods and every other mod). Drop bad enums
    // and any command that depends on a dropped enum; NativeInjectedCommands
    // validates the remaining set as one atomic batch in replaceDefinitions().
    auto validEnumName = [](const std::string& name) {
        return !name.empty() && name.size() <= 64 &&
               name.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ"
                                      "0123456789_") == std::string::npos;
    };
    std::unordered_set<std::string> validEnums;
    for (const auto& e : definitions.enums) {
        if (!validEnumName(e.name) || (!e.merged && e.values.empty())) continue;
        bedrock::NativeInjectedCommands::EnumEntry entry;
        entry.name = e.name;
        entry.values = e.values;
        entry.mergedFrom = e.mergedFrom;
        entry.merged = e.merged;
        set.enums.push_back(std::move(entry));
        validEnums.insert(e.name);
    }
    for (const auto& c : definitions.commands) {
        bool unresolved = false;
        for (const auto& p : c.params)
            if (p.kind == WireParamKind::CustomEnum && !validEnums.count(p.enumName)) {
                unresolved = true;
                break;
            }
        if (unresolved) continue;
        bedrock::NativeInjectedCommands::CommandEntry entry;
        entry.name = c.name;
        entry.description = c.description;
        std::vector<bedrock::InjectedParam> params;
        for (const auto& p : c.params)
            params.push_back({p.name, toType(p), p.optional, 0});
        entry.overloads.push_back(std::move(params));
        if (owner) {
            const auto name = c.name;
            entry.handler = [owner, name](const std::string& line) {
                owner->enqueue(line); // packet thread: copy only
            };
        }
        set.commands.push_back(std::move(entry));
    }
    return set;
}

void CommandService::enqueue(const std::string& line) {
    if (queue_.size() >= kMaxQueuedLines) return;
    queue_.push_back(line);
}

bool CommandService::load(const bedrock::RuntimeProfile& profile,
                          const WireDefinitions& definitions, std::string& error) {
    auto set = makeDefinitions(definitions, this);

    // §6.5: /mods is handled by C++ directly and is always present.
    bedrock::NativeInjectedCommands::EnumEntry actions;
    actions.name = "ModsAction";
    actions.values = {"list", "reload", "path"};
    set.enums.push_back(std::move(actions));
    bedrock::NativeInjectedCommands::EnumEntry names;
    names.name = "ModsModName";
    names.values = modNames_.empty() ? std::vector<std::string>{"none"} : modNames_;
    set.enums.push_back(std::move(names));

    bedrock::NativeInjectedCommands::CommandEntry mods;
    mods.name = "mods";
    mods.description = "Manage JS/TS mods";
    bedrock::InjectedParam action{"action", bedrock::InjectedType::customEnum("ModsAction"),
                                  false, 0};
    bedrock::InjectedParam modName{"name", bedrock::InjectedType::customEnum("ModsModName"),
                                   true, 0};
    mods.overloads.push_back({action, modName});
    mods.handler = [this](const std::string& line) { enqueue(line); };
    set.commands.push_back(std::move(mods));

    // Skip individual bad definitions instead of failing the entire service;
    // the bad mod's command simply never becomes injectable.
    for (const auto& e : set.enums) {
        std::string enumError;
        if (!commands_.addEnum(e.name, e.values, enumError)) continue;
    }
    for (const auto& c : set.commands) {
        std::string commandError;
        if (!commands_.add(c.name, c.description, c.overloads, c.handler, commandError)) continue;
    }
    error.clear();
    return commands_.load(profile, error);
}

bool CommandService::replaceDefinitions(const WireDefinitions& definitions,
                                        std::string& error) {
    auto set = makeDefinitions(definitions, this);
    bedrock::NativeInjectedCommands::EnumEntry actions;
    actions.name = "ModsAction";
    actions.values = {"list", "reload", "path"};
    set.enums.push_back(std::move(actions));

    bedrock::NativeInjectedCommands::EnumEntry names;
    names.name = "ModsModName";
    names.values = modNames_; // refreshed by setModNames() after every scan (§6.5)
    set.enums.push_back(std::move(names));

    bedrock::NativeInjectedCommands::CommandEntry mods;
    mods.name = "mods";
    mods.description = "Manage JS/TS mods";
    mods.overloads.push_back({{"action", bedrock::InjectedType::customEnum("ModsAction"), false, 0},
                              {"name", bedrock::InjectedType::customEnum("ModsModName"), true, 0}});
    mods.handler = [this](const std::string& line) { enqueue(line); };
    set.commands.push_back(std::move(mods));
    return commands_.replaceDefinitions(std::move(set.enums), std::move(set.commands), error);
}

void CommandService::drain(PluginHost& host) {
    while (!queue_.empty()) {
        const auto line = queue_.front();
        queue_.pop_front();
        const auto tokens = tokenize(line);
        if (!tokens.empty() && tokens[0] == "mods") {
            const auto response = host.handleModsCommand(line);
            if (respond_ && !response.empty()) respond_(response);
            continue;
        }
        std::string response;
        if (host.dispatchCommand(line, response) && respond_ && !response.empty())
            respond_(response);
    }
}

} // namespace stageb
