#include <bedrock/command_builder.hpp>
#include <functional>
#include <optional>

bedrock::CommandDefinition makeTopCommand(
    std::function<bedrock::CommandDecision(std::optional<int>)> queueRequest) {
    return bedrock::CommandBuilder::named(bedrock::CommandName{"top"})
        .description(bedrock::CommandDescription{"安全な上方移動"})
        .optionalInteger(bedrock::ArgumentName{"blocks"})
        .handles(std::move(queueRequest));
}
