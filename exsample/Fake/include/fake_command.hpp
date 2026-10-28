#pragma once
#include <bedrock/command_requests.hpp>
#include <charconv>
#include <string>
#include <string_view>

namespace fake_mod {
struct Invocation {
    std::uint8_t origin=5;
    std::string message;
    std::string error;
    explicit operator bool() const noexcept { return error.empty(); }
};
inline Invocation parse(std::string_view line) {
    Invocation out;
    const auto trim=[](std::string_view& value) {
        while (!value.empty() && (value.front()==' ' || value.front()=='\t')) value.remove_prefix(1);
    };
    const auto token=[&](std::string_view& value) {
        const auto end=value.find_first_of(" \t");
        auto result=value.substr(0,end);
        value=end==std::string_view::npos ? std::string_view{} : value.substr(end);
        trim(value); return result;
    };
    trim(line);
    auto name=token(line);
    if (name!="/fake" && name!="fake") { out.error="unknown command"; return out; }
    if (line.starts_with("--origin=") || line.starts_with("--origin ") || line.starts_with("--origin\t") ||
        line=="--origin" || line.starts_with("-o ") || line.starts_with("-o\t") || line=="-o") {
        auto option=token(line); std::string_view value;
        if (option.starts_with("--origin=")) value=option.substr(9);
        else value=token(line);
        unsigned number{};
        const auto result=std::from_chars(value.data(),value.data()+value.size(),number);
        if (value.empty() || result.ec!=std::errc{} || result.ptr!=value.data()+value.size() || number>255) {
            out.error="Origin must be an integer from 0 to 255"; return out;
        }
        out.origin=static_cast<std::uint8_t>(number);
    } else if (line=="--") { line={}; }
    else if (line.starts_with("-- ") || line.starts_with("--\t")) { token(line); }
    if (line.empty()) out.error="Usage: /fake [--origin 0..255] message";
    else if (line.size()>2044 || line.find_first_of(std::string_view{"\r\n\0",3})!=std::string_view::npos)
        out.error="Message must be a single line (maximum 2044 UTF-8 bytes)";
    else out.message=std::string(line);
    return out;
}
} // namespace fake_mod
