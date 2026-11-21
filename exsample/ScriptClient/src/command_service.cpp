#include "command_service.hpp"

#include <algorithm>
#include <filesystem>
#include <unordered_set>
#include <windows.h>

namespace stageb {
namespace {

constexpr std::size_t kMaxQueuedLines = 64; // §12: bounded command queue

bedrock::sdk::CommandParameterType toType(const WireParam& param) {
    switch (param.kind) {
        case WireParamKind::PacketEnum:
            return bedrock::sdk::CommandParameterType::PacketEnum;
        case WireParamKind::Integer:
            return bedrock::sdk::CommandParameterType::Integer;
        case WireParamKind::Message:
            return bedrock::sdk::CommandParameterType::Message;
        case WireParamKind::CustomEnum:
        default:
            return bedrock::sdk::CommandParameterType::CustomEnum;
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
        bedrock::sdk::CommandEnum entry;
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
        bedrock::sdk::CommandDefinition entry;
        entry.name = c.name;
        entry.description = c.description;
        std::vector<bedrock::sdk::CommandParameter> params;
        for (const auto& p : c.params)
            params.push_back({p.name, toType(p), p.enumName, p.optional});
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

bool CommandService::load(const bedrock::sdk::Profile& profile,
                          const WireDefinitions& definitions, std::string& error) {
    const std::string& profileJson = profile.json();
    auto set = makeDefinitions(definitions, this);

    // §6.5: /mods is handled by C++ directly and is always present.
    bedrock::sdk::CommandEnum actions;
    actions.name = "ModsAction";
    actions.values = {"list", "reload", "path"};
    set.enums.push_back(std::move(actions));
    bedrock::sdk::CommandEnum names;
    names.name = "ModsModName";
    names.values = modNames_.empty() ? std::vector<std::string>{"none"} : modNames_;
    set.enums.push_back(std::move(names));

    bedrock::sdk::CommandDefinition mods;
    mods.name = "mods";
    mods.description = "Manage JS/TS mods";
    bedrock::sdk::CommandParameter action{"action", bedrock::sdk::CommandParameterType::CustomEnum, "ModsAction", false};
    bedrock::sdk::CommandParameter modName{"name", bedrock::sdk::CommandParameterType::CustomEnum, "ModsModName", true};
    mods.overloads.push_back({action, modName});
    mods.handler = [this](const std::string& line) { enqueue(line); };
    set.commands.push_back(std::move(mods));
    error.clear();
    (void)profileJson;
    return commands_.load(profile, set.enums, set.commands, error);
}

bool CommandService::replaceDefinitions(const WireDefinitions& definitions,
                                        std::string& error) {
    auto set = makeDefinitions(definitions, this);
    bedrock::sdk::CommandEnum actions;
    actions.name = "ModsAction";
    actions.values = {"list", "reload", "path"};
    set.enums.push_back(std::move(actions));

    bedrock::sdk::CommandEnum names;
    names.name = "ModsModName";
    names.values = modNames_; // refreshed by setModNames() after every scan (§6.5)
    set.enums.push_back(std::move(names));

    bedrock::sdk::CommandDefinition mods;
    mods.name = "mods";
    mods.description = "Manage JS/TS mods";
    mods.overloads.push_back({{"action", bedrock::sdk::CommandParameterType::CustomEnum, "ModsAction", false},
                              {"name", bedrock::sdk::CommandParameterType::CustomEnum, "ModsModName", true}});
    mods.handler = [this](const std::string& line) { enqueue(line); };
    set.commands.push_back(std::move(mods));
    return commands_.replace(set.enums, set.commands, error);
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
