#pragma once
#include <functional>
#include <memory>
#include <string>
#include <vector>
namespace bedrock::sdk { class Profile; }
namespace bedrock::sdk {
enum class CommandParameterType { Integer, Message, CustomEnum, PacketEnum };
struct CommandParameter { std::string name; CommandParameterType type{CommandParameterType::Message}; std::string enumName; bool optional{}; };
struct CommandDefinition { std::string name; std::string description; std::vector<std::vector<CommandParameter>> overloads; std::function<void(const std::string&)> handler; };
struct CommandEnum {
    std::string name;
    std::vector<std::string> values;
    std::string mergedFrom;
    bool merged{};
};
class Commands { public:
 Commands(); ~Commands(); Commands(const Commands&) = delete; Commands& operator=(const Commands&) = delete;
 bool load(const std::string& profileJson, const std::vector<CommandEnum>& enums,
          const std::vector<CommandDefinition>& commands, std::string& error);
 bool load(const Profile& profile, const std::vector<CommandEnum>& enums,
          const std::vector<CommandDefinition>& commands, std::string& error);
 bool replace(const std::vector<CommandEnum>& enums,
              const std::vector<CommandDefinition>& commands, std::string& error);
 bool isLoaded() const noexcept;
 private: struct Impl; std::unique_ptr<Impl> impl_;
};
}
