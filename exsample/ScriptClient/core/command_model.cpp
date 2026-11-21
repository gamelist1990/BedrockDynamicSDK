#include "command_model.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <set>

namespace stageb {
namespace {

std::string upperFirst(std::string s) {
    if (!s.empty() && s[0] >= 'a' && s[0] <= 'z') s[0] = static_cast<char>(s[0] - 'a' + 'A');
    return s;
}

std::string normalizeEffect(std::string token) {
    if (token.find(':') != std::string::npos) return token;
    if (token.rfind("minecraft:", 0) == 0) return token;
    return "minecraft:" + token;
}

bool parseInt(const std::string& token, int& out) {
    if (token.empty()) return false;
    char* end = nullptr;
    const long value = std::strtol(token.c_str(), &end, 10);
    if (end == token.c_str() || *end != '\0') return false;
    out = static_cast<int>(value);
    return true;
}

bool parseDouble(const std::string& token, double& out) {
    if (token.empty()) return false;
    char* end = nullptr;
    const double value = std::strtod(token.c_str(), &end);
    if (end == token.c_str() || *end != '\0') return false;
    out = value;
    return true;
}

std::string enumName(const CommandDecl& decl, const std::string& arg) {
    return upperFirst(decl.name) + upperFirst(arg);
}

} // namespace

std::vector<std::string> tokenize(std::string_view line) {
    std::vector<std::string> tokens;
    std::size_t i = 0;
    while (i < line.size()) {
        while (i < line.size() && std::isspace(static_cast<unsigned char>(line[i]))) ++i;
        if (i >= line.size()) break;
        if (line[i] == '/' && tokens.empty()) { ++i; continue; }
        std::string token;
        bool quoted = false;
        if (line[i] == '"') { quoted = true; ++i; }
        while (i < line.size()) {
            if (quoted && line[i] == '"') { ++i; break; }
            if (!quoted && std::isspace(static_cast<unsigned char>(line[i]))) break;
            if (line[i] == '\\' && i + 1 < line.size()) ++i;
            token.push_back(line[i++]);
        }
        tokens.push_back(std::move(token));
    }
    return tokens;
}

WireDefinitions buildWireDefinitions(const std::vector<CommandDecl>& declarations) {
    WireDefinitions out;
    for (const auto& decl : declarations) {
        const auto actionEnum = upperFirst(decl.name) + "Action";
        WireEnum actions;
        actions.name = actionEnum;
        for (const auto& branch : decl.branches)
            for (const auto& literal : branch.literals)
                if (std::find(actions.values.begin(), actions.values.end(), literal) ==
                    actions.values.end())
                    actions.values.push_back(literal);
        // A builder that only calls .executes(...) (no .literal(...) branch) is
        // a legal shape. Its Action enum would have no values, and
        // NativeInjectedCommands rejects an empty non-merged enum, aborting the
        // whole definition load (including /mods). Omit the enum entirely and
        // keep the flattened wire command consistent by omitting its leading
        // action parameter too.
        const bool hasActionLiteral = !actions.values.empty();
        if (hasActionLiteral) {
            for (const auto& branch : decl.branches)
                for (const auto& arg : branch.args)
                    if (arg.second.kind == ArgKind::Effect) {
                        actions.merged = true;
                        actions.mergedFrom = "Effect";
                    }
            out.enums.push_back(std::move(actions));
        }

        // Union of argument names in first-seen order; flattened as optional
        // trailing parameters (§6.4, appendix A).
        std::vector<std::pair<std::string, ArgSpec>> unionArgs;
        for (const auto& branch : decl.branches)
            for (const auto& arg : branch.args) {
                const auto it = std::find_if(unionArgs.begin(), unionArgs.end(),
                    [&](const auto& x) { return x.first == arg.first; });
                if (it == unionArgs.end()) unionArgs.push_back(arg);
            }

        WireCommand command;
        command.name = decl.name;
        command.description = decl.description;
        command.handlerName = decl.name;
        if (hasActionLiteral)
            command.params.push_back({actionEnum, WireParamKind::CustomEnum, actionEnum, false});

        for (const auto& arg : unionArgs) {
            WireParam p;
            p.name = arg.first;
            p.optional = true;
            switch (arg.second.kind) {
                case ArgKind::Effect:
                    p.kind = WireParamKind::PacketEnum;
                    p.enumName = "Effect";
                    break;
                case ArgKind::Duration: {
                    p.kind = WireParamKind::CustomEnum;
                    p.enumName = enumName(decl, arg.first);
                    WireEnum e;
                    e.name = p.enumName;
                    e.values = {"Infinity", "5", "10", "30", "60", "180", "600"};
                    out.enums.push_back(std::move(e));
                    break;
                }
                case ArgKind::Int: {
                    p.kind = arg.second.suggestions.empty() ? WireParamKind::Integer
                                                            : WireParamKind::CustomEnum;
                    if (!arg.second.suggestions.empty()) {
                        p.enumName = enumName(decl, arg.first);
                        WireEnum e;
                        e.name = p.enumName;
                        for (const auto& v : arg.second.suggestions) e.values.push_back(v);
                        out.enums.push_back(std::move(e));
                    }
                    break;
                }
                case ArgKind::Bool:
                    p.kind = WireParamKind::CustomEnum;
                    p.enumName = enumName(decl, arg.first);
                    {
                        WireEnum e;
                        e.name = p.enumName;
                        e.values = {"true", "false"};
                        out.enums.push_back(std::move(e));
                    }
                    break;
                case ArgKind::String:
                case ArgKind::Message:
                    p.kind = WireParamKind::Message;
                    break;
                case ArgKind::Float:
                    p.kind = WireParamKind::CustomEnum;
                    p.enumName = enumName(decl, arg.first);
                    {
                        WireEnum e;
                        e.name = p.enumName;
                        e.values = {"0", "1", "10"};
                        out.enums.push_back(std::move(e));
                    }
                    break;
            }
            command.params.push_back(std::move(p));
        }
        out.commands.push_back(std::move(command));
    }
    return out;
}

ParseResult parseCommandLine(const CommandDecl& declaration, const std::string& line) {
    ParseResult result;
    const auto tokens = tokenize(line);
    if (tokens.empty() || tokens[0] != declaration.name) {
        result.error = "not a /" + declaration.name + " command";
        return result;
    }

    std::size_t bestBranch = declaration.branches.size();
    std::size_t consumedBest = 0;
    for (std::size_t i = 0; i < declaration.branches.size(); ++i) {
        const auto& branch = declaration.branches[i];
        bool matches = true;
        for (std::size_t j = 0; j < branch.literals.size(); ++j) {
            if (tokens.size() <= j + 1 || tokens[j + 1] != branch.literals[j]) {
                matches = false;
                break;
            }
        }
        if (matches) {
            const std::size_t consumed = 1 + branch.literals.size();
            // Prefer the branch that consumes the most leading literals.
            if (bestBranch == declaration.branches.size() || consumed > consumedBest) {
                bestBranch = i;
                consumedBest = consumed;
            }
        }
    }
    if (bestBranch == declaration.branches.size()) {
        result.error = "unknown subcommand for /" + declaration.name;
        return result;
    }

    const auto& branch = declaration.branches[bestBranch];
    result.branch = bestBranch;
    std::size_t index = consumedBest;
    for (const auto& arg : branch.args) {
        ContextValue value;
        value.kind = arg.second.kind;
        const bool available = index < tokens.size();
        if (!available) {
            if (!arg.second.optional) {
                result.error = "missing required argument: " + arg.first;
                return result;
            }
            switch (arg.second.kind) {
                case ArgKind::Effect:
                case ArgKind::String:
                case ArgKind::Message:
                    value.text = arg.second.stringDefault;
                    break;
                case ArgKind::Duration:
                case ArgKind::Int:
                case ArgKind::Float:
                    value.number = arg.second.defaultValue;
                    break;
                case ArgKind::Bool:
                    value.boolean = arg.second.stringDefault == "true";
                    break;
            }
            result.values[arg.first] = std::move(value);
            continue;
        }

        const auto token = tokens[index++];
        value.present = true;
        switch (arg.second.kind) {
            case ArgKind::Effect:
                value.text = normalizeEffect(token);
                break;
            case ArgKind::Duration:
            case ArgKind::Float: {
                double number = 0;
                if (!parseDouble(token, number) ||
                    (std::isinf(number) && !arg.second.infinity)) {
                    result.error = "invalid number for " + arg.first + ": " + token;
                    return result;
                }
                value.number = number;
                break;
            }
            case ArgKind::Int: {
                int number = 0;
                if (!parseInt(token, number)) {
                    result.error = "invalid integer for " + arg.first + ": " + token;
                    return result;
                }
                if (number < arg.second.min || number > arg.second.max) {
                    result.error = "argument " + arg.first + " out of range [" +
                                   std::to_string(arg.second.min) + ", " +
                                   std::to_string(arg.second.max) + "]";
                    return result;
                }
                value.number = static_cast<double>(number);
                break;
            }
            case ArgKind::Bool:
                if (token == "true") value.boolean = true;
                else if (token == "false") value.boolean = false;
                else {
                    result.error = "invalid boolean for " + arg.first + ": " + token;
                    return result;
                }
                break;
            case ArgKind::String:
            case ArgKind::Message:
                value.text = token;
                break;
        }
        result.values[arg.first] = std::move(value);
    }
    // std::strtod already accepts "Infinity"/"inf"; no special case needed.
    result.ok = true;
    return result;
}

} // namespace stageb
