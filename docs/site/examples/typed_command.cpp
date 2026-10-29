#include <bedrock/commands/command_builder.hpp>
#include <functional>
#include <optional>

bedrock::CommandDefinition makeTopCommand(
    std::function<bedrock::CommandDecision(std::optional<int>)> queueRequest) {
    return bedrock::CommandBuilder(bedrock::CommandName{"top"}, bedrock::ArgumentName{"blocks"})
        .description(bedrock::CommandDescription{"安全な上方移動"})
        .then(std::move(queueRequest));
}
