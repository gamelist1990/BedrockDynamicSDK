#pragma once

#include "command_model.hpp"
#include "plugin_host.hpp"

#include <bedrock/native/injected_commands.hpp>

#include <deque>
#include <functional>
#include <string>

namespace stageb {

// The host owns the one NativeInjectedCommands instance in the process (§6.3).
// Packet handlers only copy the command line into a queue; JS runs later on
// the owning tick via drain().
class CommandService {
public:
    using Respond = std::function<void(const std::string&)>;

    CommandService();

    bool load(const bedrock::RuntimeProfile& profile,
              const WireDefinitions& definitions, std::string& error);
    bool replaceDefinitions(const WireDefinitions& definitions, std::string& error);
    void drain(PluginHost& host);

    void setRespond(Respond respond) { respond_ = std::move(respond); }
    void setModNames(std::vector<std::string> names) { modNames_ = std::move(names); }
    bedrock::NativeInjectedCommands& native() noexcept { return commands_; }

private:
    struct DefinitionSet {
        std::vector<bedrock::NativeInjectedCommands::EnumEntry> enums;
        std::vector<bedrock::NativeInjectedCommands::CommandEntry> commands;
    };
    static DefinitionSet makeDefinitions(const WireDefinitions& definitions,
                                         CommandService* owner);
    void enqueue(const std::string& line);

    bedrock::NativeInjectedCommands commands_;
    std::deque<std::string> queue_;
    Respond respond_;
    std::vector<std::string> modNames_;
};

} // namespace stageb
