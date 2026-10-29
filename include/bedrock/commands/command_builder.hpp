#pragma once
#include <bedrock/api/api_types.hpp>
#include <concepts>
#include <functional>
#include <optional>
#include <type_traits>
#include <utility>

namespace bedrock {
template<bool Optional> class IntegerCommandBuilder;

// Construction is complete only when a typed handler has been supplied.
// Building a definition never reads game memory or installs native hooks.
class CommandDefinition {
    template<bool> friend class IntegerCommandBuilder;
public:
    CommandDefinition(const CommandDefinition&) = default;
    CommandDefinition(CommandDefinition&&) noexcept = default;
    const CommandName& name() const noexcept { return name_; }
    const CommandDescription& description() const noexcept { return description_; }
    const ArgumentName& argument() const noexcept { return argument_; }
    bool optional() const noexcept { return optional_; }
    CommandDecision invoke(std::optional<int> value) const {
        if (!optional_ && !value) return CommandDecision::Rejected;
        return handler_(value);
    }
private:
    CommandDefinition(CommandName name, CommandDescription description, ArgumentName argument, bool optional,
                      std::function<CommandDecision(std::optional<int>)> handler)
        : name_(std::move(name)), description_(std::move(description)), argument_(std::move(argument)),
          optional_(optional), handler_(std::move(handler)) {
        if (!handler_) throw std::invalid_argument("command handler is empty");
    }
    CommandName name_;
    CommandDescription description_;
    ArgumentName argument_;
    bool optional_;
    std::function<CommandDecision(std::optional<int>)> handler_;
};

template<class Handler, class Argument>
concept CommandHandler = std::invocable<Handler&, Argument> && std::copy_constructible<Handler> &&
    std::same_as<std::invoke_result_t<Handler&, Argument>, CommandDecision>;

template<bool Optional>
class IntegerCommandBuilder {
public:
    using Argument = std::conditional_t<Optional, std::optional<int>, int>;
    IntegerCommandBuilder(CommandName name, CommandDescription description, ArgumentName argument)
        : name_(std::move(name)), description_(std::move(description)), argument_(std::move(argument)) {}

    template<CommandHandler<Argument> Handler>
    CommandDefinition then(Handler handler) && {
        return CommandDefinition{std::move(name_), std::move(description_), std::move(argument_), Optional,
            [fn = std::move(handler)](std::optional<int> value) mutable {
                if constexpr (Optional) return std::invoke(fn, value);
                else return value ? std::invoke(fn, *value) : CommandDecision::Rejected;
            }};
    }
    // Compatibility spelling; new code uses then().
    template<CommandHandler<Argument> Handler>
    CommandDefinition handles(Handler handler) && { return std::move(*this).then(std::move(handler)); }
private:
    CommandName name_;
    CommandDescription description_;
    ArgumentName argument_;
};

class CommandBuilder {
public:
    explicit CommandBuilder(CommandName name) : name_(std::move(name)) {}
    // Compact form: a single optional integer, received as optional<int>.
    CommandBuilder(CommandName name, ArgumentName optionalInteger)
        : name_(std::move(name)), argument_(std::move(optionalInteger)) {}
    static CommandBuilder named(CommandName name) { return CommandBuilder{std::move(name)}; }

    CommandBuilder description(CommandDescription description) && {
        description_ = std::move(description); return std::move(*this);
    }
    IntegerCommandBuilder<true> optionalInteger(ArgumentName name) && {
        return {std::move(name_), std::move(description_), std::move(name)};
    }
    IntegerCommandBuilder<false> requiredInteger(ArgumentName name) && {
        return {std::move(name_), std::move(description_), std::move(name)};
    }
    template<CommandHandler<std::optional<int>> Handler>
    CommandDefinition then(Handler handler) && {
        return std::move(*this).optionalInteger(std::move(argument_)).then(std::move(handler));
    }
private:
    CommandName name_;
    CommandDescription description_{std::string{}};
    ArgumentName argument_{"value"};
};
} // namespace bedrock
