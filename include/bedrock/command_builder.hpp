#pragma once
#include <bedrock/api_types.hpp>
#include <concepts>
#include <functional>
#include <optional>
#include <type_traits>
#include <utility>
namespace bedrock {
// A definition is complete only after a typed argument and handler are supplied.
// No native pointers or game thread operations are involved in building it.
template<bool Optional>class IntegerCommandBuilder;
class CommandDefinition {
    template<bool>friend class IntegerCommandBuilder;
    CommandName name_;
    CommandDescription description_;
    ArgumentName argument_;
    bool optional_;
    std::function<CommandDecision(std::optional<int>)> handler_;
    CommandDefinition(CommandName name,CommandDescription description,ArgumentName argument,bool optional,
                      std::function<CommandDecision(std::optional<int>)> handler)
        :name_(std::move(name)),description_(std::move(description)),argument_(std::move(argument)),optional_(optional),handler_(std::move(handler)){
        if(!handler_)throw std::invalid_argument("command handler is empty");
    }
public:
    CommandDefinition(const CommandDefinition&)=default;
    CommandDefinition(CommandDefinition&&)noexcept=default;
    const CommandName& name()const noexcept{return name_;}
    const CommandDescription& description()const noexcept{return description_;}
    const ArgumentName& argument()const noexcept{return argument_;}
    bool optional()const noexcept{return optional_;}
    CommandDecision invoke(std::optional<int> value)const{
        if(!optional_&&!value)return CommandDecision::Rejected;
        return handler_(value);
    }
};
template<bool Optional>class IntegerCommandBuilder {
    CommandName name_;CommandDescription description_;ArgumentName argument_;
public:
    IntegerCommandBuilder(CommandName name,CommandDescription description,ArgumentName argument)
        :name_(std::move(name)),description_(std::move(description)),argument_(std::move(argument)){}
    using Argument=std::conditional_t<Optional,std::optional<int>,int>;
    template<class Handler>requires std::invocable<Handler&,Argument> &&
        std::same_as<std::invoke_result_t<Handler&,Argument>,CommandDecision> && std::copy_constructible<Handler>
    CommandDefinition handles(Handler handler)&&{
        return CommandDefinition{std::move(name_),std::move(description_),std::move(argument_),Optional,
            [fn=std::move(handler)](std::optional<int> v)mutable{
                if constexpr(Optional)return std::invoke(fn,v);
                else return v?std::invoke(fn,*v):CommandDecision::Rejected;
            }};
    }
};
class CommandBuilder {
    CommandName name_;CommandDescription description_{std::string{}};
public:
    explicit CommandBuilder(CommandName name):name_(std::move(name)){}
    static CommandBuilder named(CommandName name){return CommandBuilder{std::move(name)};}
    CommandBuilder description(CommandDescription description)&&{description_=std::move(description);return std::move(*this);}
    IntegerCommandBuilder<true> optionalInteger(ArgumentName name)&&{return {std::move(name_),std::move(description_),std::move(name)};}
    IntegerCommandBuilder<false> requiredInteger(ArgumentName name)&&{return {std::move(name_),std::move(description_),std::move(name)};}
};
} // namespace bedrock
