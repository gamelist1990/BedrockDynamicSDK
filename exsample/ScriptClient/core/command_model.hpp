#pragma once

#include "common.hpp"
#include <limits>
#include <map>

namespace stageb {

// §6.4 Arg helper kinds. The runtime representation is a plain descriptor;
// no JS values are stored here (all parsing happens on the owning tick).
enum class ArgKind { Effect, Duration, Int, Bool, String, Message, Float };

struct ArgSpec {
    ArgKind kind = ArgKind::String;
    bool optional = false;
    bool infinity = false;
    bool hasDefault = false;
    double defaultValue = 0.0;
    std::string stringDefault;
    // Unspecified int bounds mean the whole int32 range, not [0, 0].
    int min = std::numeric_limits<int>::min();
    int max = std::numeric_limits<int>::max();
    std::vector<std::string> suggestions;
};

struct CommandBranch {
    std::vector<std::string> literals;
    std::vector<std::pair<std::string, ArgSpec>> args;
    std::size_t handlerIndex = static_cast<std::size_t>(-1); // into JS callback table
};

struct CommandDecl {
    std::string name;
    std::string description;
    std::vector<CommandBranch> branches;
};

enum class WireParamKind { CustomEnum, PacketEnum, Integer, Message };

struct WireParam {
    std::string name;
    WireParamKind kind = WireParamKind::CustomEnum;
    std::string enumName;
    bool optional = false;
};

struct WireEnum {
    std::string name;
    std::vector<std::string> values;
    std::string mergedFrom;
    bool merged = false;
};

struct WireCommand {
    std::string name;
    std::string description;
    std::vector<WireParam> params;
    std::string handlerName;
};

struct WireDefinitions {
    std::vector<WireEnum> enums;
    std::vector<WireCommand> commands;
};

// §6.4: flatten builder declarations into the single-overload/merged-enum
// wire shape used by NativeInjectedCommands (appendix A of DESIGN.md).
WireDefinitions buildWireDefinitions(const std::vector<CommandDecl>& declarations);

struct ContextValue {
    ArgKind kind = ArgKind::String;
    bool present = false;
    std::string text;
    double number = 0.0;
    bool boolean = false;
};

struct ParseResult {
    bool ok = false;
    std::string error;
    std::size_t branch = 0;
    std::map<std::string, ContextValue> values;
};

// §6.4: deterministic line parsing + default application + validation.
// `line` may start with '/'. Returns typed values ready for `ctx.get<T>`.
ParseResult parseCommandLine(const CommandDecl& declaration, const std::string& line);

std::vector<std::string> tokenize(std::string_view line);

} // namespace stageb
