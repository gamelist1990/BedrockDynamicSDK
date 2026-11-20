#pragma once
#include <nlohmann/json.hpp>
#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace checksig {
namespace fs = std::filesystem;
using json = nlohmann::json;
constexpr std::size_t maxProfileBytes = 16 * 1024 * 1024;
constexpr std::size_t maxSymbols = 20000;

inline std::optional<std::vector<int>> parsePattern(std::string_view pattern) {
    std::istringstream in{std::string(pattern)};
    std::vector<int> bytes;
    std::string token;
    while (in >> token) {
        if (token == "?" || token == "??") bytes.push_back(-1);
        else if (token.size() == 2 &&
                 std::isxdigit(static_cast<unsigned char>(token[0])) &&
                 std::isxdigit(static_cast<unsigned char>(token[1]))) {
            unsigned value{};
            std::istringstream hexIn(token);
            hexIn >> std::hex >> value;
            if (!hexIn || value > 255) return std::nullopt;
            bytes.push_back(static_cast<int>(value));
        } else return std::nullopt;
        if (bytes.size() > 512) return std::nullopt;
    }
    if (bytes.empty()) return std::nullopt;
    return bytes;
}

struct MatchResult {
    std::vector<std::size_t> offsets;
    bool capped{};
};

inline MatchResult findMatches(const std::vector<std::byte>& haystack, std::string_view pattern) {
    MatchResult result;
    const auto parsed = parsePattern(pattern);
    if (!parsed) return result;
    std::size_t anchorAt = 0, anchorSize = 0;
    for (std::size_t i = 0; i < parsed->size();) {
        if ((*parsed)[i] < 0) { ++i; continue; }
        std::size_t j = i;
        while (j < parsed->size() && (*parsed)[j] >= 0) ++j;
        if (j - i > anchorSize) { anchorAt = i; anchorSize = j - i; }
        i = j;
    }
    if (anchorSize < 4 || parsed->size() > haystack.size()) return result;
    const auto* begin = reinterpret_cast<const char*>(haystack.data());
    const auto* end = begin + haystack.size();
    std::vector<char> anchor;
    for (std::size_t i = 0; i < anchorSize; ++i)
        anchor.push_back(static_cast<char>((*parsed)[anchorAt + i]));
    std::size_t seekFrom = 0;
    while (seekFrom + anchorSize <= haystack.size()) {
        const auto* found = std::search(begin + seekFrom, end, anchor.begin(), anchor.end());
        if (found == end) break;
        const auto position = static_cast<std::size_t>(found - begin);
        seekFrom = position + 1;
        if (position < anchorAt) continue;
        const auto start = position - anchorAt;
        if (start + parsed->size() > haystack.size()) continue;
        bool matches = true;
        for (std::size_t i = 0; i < parsed->size(); ++i) {
            if ((*parsed)[i] >= 0 &&
                std::to_integer<unsigned>(haystack[start + i]) !=
                    static_cast<unsigned>((*parsed)[i])) {
                matches = false;
                break;
            }
        }
        if (matches) {
            result.offsets.push_back(start);
            if (result.offsets.size() > 512) {
                result.offsets.clear();
                result.capped = true;
                return result;
            }
        }
    }
    return result;
}


inline std::uint32_t readU32(const json& value) {
    std::uint64_t number = 0;
    if (value.is_string()) {
        const auto text = value.get<std::string>();
        if (text.empty() || text.front() == '-') throw std::invalid_argument("invalid unsigned value");
        std::size_t consumed = 0;
        number = std::stoull(text, &consumed, 0);
        if (consumed != text.size()) throw std::invalid_argument("invalid unsigned value");
    } else if (value.is_number_unsigned()) {
        number = value.get<std::uint64_t>();
    } else if (value.is_number_integer() && value.get<std::int64_t>() >= 0) {
        number = static_cast<std::uint64_t>(value.get<std::int64_t>());
    } else throw std::invalid_argument("expected unsigned integer or hex string");
    if (number > UINT32_MAX) throw std::invalid_argument("value exceeds 32 bits");
    return static_cast<std::uint32_t>(number);
}

inline json parseProfile(std::string_view text) {
    if (text.empty() || text.size() > maxProfileBytes)
        throw std::invalid_argument("profile must be between 1 byte and 16 MiB");
    auto document = json::parse(text.begin(), text.end());
    if (!document.is_object() || document.at("schema") != 1)
        throw std::invalid_argument("expected profile schema 1");
    const auto& target = document.at("target");
    if (!target.is_object() || target.at("product") != "minecraft-bedrock" ||
        target.at("platform") != "windows" || target.at("arch") != "x64")
        throw std::invalid_argument("expected Minecraft Bedrock Windows x64 profile");
    const auto sha = target.at("sha256").get<std::string>();
    if (sha.size() != 64 || !std::all_of(sha.begin(), sha.end(), [](unsigned char c) {
            return std::isxdigit(c) != 0;
        })) throw std::invalid_argument("target.sha256 must be 64 hexadecimal characters");
    if (target.at("version").get<std::string>().empty())
        throw std::invalid_argument("target.version is empty");
    const auto imageSize = readU32(target.at("size_of_image"));
    const auto textRva = readU32(target.at("text_rva"));
    const auto textSize = readU32(target.at("text_size"));
    if (!imageSize || !textSize || textRva >= imageSize || textSize > imageSize - textRva)
        throw std::invalid_argument("invalid profile PE text range");
    const auto& symbols = document.at("symbols");
    if (!symbols.is_object() || symbols.empty() || symbols.size() > maxSymbols)
        throw std::invalid_argument("expected 1..20000 symbols");
    for (const auto& [name, symbol] : symbols.items()) {
        try {
            if (!symbol.is_object() || readU32(symbol.at("rva")) >= imageSize)
                throw std::invalid_argument("RVA outside image");
            const auto guard = parsePattern(symbol.at("entry_check").get<std::string>());
            if (!guard || std::count_if(guard->begin(), guard->end(), [](int v) { return v >= 0; }) < 4)
                throw std::invalid_argument("entry_check needs at least four fixed bytes");
            if (guard->size() > imageSize - readU32(symbol.at("rva")))
                throw std::invalid_argument("entry_check crosses image boundary");
            const auto signature = symbol.value("signature", std::string{});
            if ((!signature.empty() && !parsePattern(signature)) ||
                (signature.empty() && symbol.value("cross_version_signature_available", true)))
                throw std::invalid_argument("invalid/missing signature or RVA-only flag");
            (void)symbol.value("status", std::string("candidate"));
            (void)symbol.value("abi_verified", false);
            (void)symbol.value("runtime_verified", false);
            (void)symbol.value("source", std::string{});
        } catch (const std::exception& e) {
            throw std::invalid_argument("symbol '" + name + "': " + e.what());
        }
    }
    return document;
}

// Reads only the declared, bounded size, then rejects concurrent growth/shrinkage.
inline bool readBounded(const fs::path& path, std::string& result,
                        std::size_t limit, std::string& error) {
    std::error_code ec;
    const auto size = fs::file_size(path, ec);
    if (ec || size > limit) {
        error = ec ? "file is missing or inaccessible" : "file exceeds size limit";
        return false;
    }
    std::ifstream in(path, std::ios::binary);
    if (!in) { error = "file could not be opened"; return false; }
    result.resize(static_cast<std::size_t>(size));
    if (size) in.read(result.data(), static_cast<std::streamsize>(size));
    const bool incomplete = size && !in;
    const auto extra = in.peek();
    if (incomplete || in.bad() || extra != std::char_traits<char>::eof()) {
        error = "file read was incomplete";
        return false;
    }
    error.clear();
    return true;
}

inline std::vector<std::string> commandLines(std::string text) {
    if (text.starts_with("\xEF\xBB\xBF")) text.erase(0, 3);
    std::vector<std::string> result;
    std::istringstream stream(text);
    for (std::string line; std::getline(stream, line);) {
        const auto first = line.find_first_not_of(" \t\r");
        if (first == std::string::npos || line[first] == '#') continue;
        const auto last = line.find_last_not_of(" \t\r");
        result.push_back(line.substr(first, last - first + 1));
    }
    return result;
}

struct InputFile {
    fs::path path;
    bool profile = false;
    std::string text;
    std::string error;
};

class Inbox {
public:
    using Clock = std::chrono::steady_clock;
    // Re-read settled JSON even when a replacement preserved size and mtime.
    std::vector<InputFile> poll(const std::vector<fs::path>& directories,
                               Clock::time_point now, bool rescanProfiles = false) {
        std::vector<InputFile> ready;
        std::set<fs::path> seen;
        for (const auto& directory : directories) {
            std::error_code ec;
            for (fs::directory_iterator it(directory, ec), end; !ec && it != end; it.increment(ec)) {
                const auto path = it->path();
                auto extension = path.extension().string();
                std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char c) {
                    return static_cast<char>(std::tolower(c));
                });
                if (extension != ".json" && extension != ".txt") continue;
                if (!fs::is_regular_file(it->symlink_status(ec))) { ec.clear(); continue; }
                seen.insert(path);
                const auto stamp = it->last_write_time(ec);
                if (ec) { ec.clear(); continue; }
                const auto size = it->file_size(ec);
                if (ec) { ec.clear(); continue; }
                auto [position, inserted] = states_.try_emplace(path);
                auto& state = position->second;
                const bool profile = extension == ".json";
                if (rescanProfiles && profile) state.delivered = false;
                if (inserted || state.stamp != stamp || state.size != size) {
                    state.stamp = stamp;
                    state.size = size;
                    state.changed = now;
                    state.nextRead = now;
                    state.delivered = false;
                    continue;
                }
                if (now - state.changed < std::chrono::milliseconds(750) ||
                    (now < state.nextRead && !(rescanProfiles && profile))) continue;
                if (ready.size() >= 16) continue; // Bound memory/work per poll; other files stay pending.
                state.nextRead = now + std::chrono::seconds(1);
                InputFile input{path, profile, {}, {}};
                if (readBounded(path, input.text, profile ? maxProfileBytes : 4096, input.error)) {
                    const auto afterStamp = fs::last_write_time(path, ec);
                    if (ec) { ec.clear(); continue; }
                    const auto afterSize = fs::file_size(path, ec);
                    if (ec || stamp != afterStamp || size != afterSize) { ec.clear(); continue; }
                }
                const auto fingerprint = hash(input.error.empty() ? input.text : input.error);
                if (state.delivered && state.fingerprint == fingerprint) continue;
                state.delivered = true;
                state.fingerprint = fingerprint;
                ready.push_back(std::move(input));
            }
        }
        for (auto it = states_.begin(); it != states_.end();) {
            if (!seen.contains(it->first)) it = states_.erase(it);
            else ++it;
        }
        std::sort(ready.begin(), ready.end(), [](const InputFile& a, const InputFile& b) {
            if (a.profile != b.profile) return a.profile;
            return a.path < b.path;
        });
        return ready;
    }
    void retry(const fs::path& path) {
        const auto it = states_.find(path);
        if (it != states_.end()) it->second.delivered = false;
    }
private:
    struct State {
        fs::file_time_type stamp{};
        std::uintmax_t size{};
        Clock::time_point changed{}, nextRead{};
        std::uint64_t fingerprint{};
        bool delivered = false;
    };
    static std::uint64_t hash(std::string_view text) {
        std::uint64_t value = 14695981039346656037ull;
        for (unsigned char c : text) { value ^= c; value *= 1099511628211ull; }
        return value;
    }
    std::map<fs::path, State> states_;
};
} // namespace checksig
