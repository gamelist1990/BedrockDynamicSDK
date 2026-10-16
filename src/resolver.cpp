#include "bedrock/resolver.hpp"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cctype>
#include <limits>
#include <stdexcept>

namespace bedrock {
namespace {
std::string lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}
bool sha256hex(const std::string& s) {
    if (s.size() != 64) return false;
    return std::all_of(s.begin(), s.end(),
                       [](unsigned char c) { return std::isxdigit(c) != 0; });
}
std::uint32_t toRva(const nlohmann::json& j) {
    std::uint64_t n = j.is_string() ? std::stoull(j.get<std::string>(), nullptr, 0)
                                  : j.get<std::uint64_t>();
    if (n > std::numeric_limits<std::uint32_t>::max())
        throw std::invalid_argument("RVA exceeds 32 bits");
    return static_cast<std::uint32_t>(n);
}
} // namespace

Resolver::BytePattern Resolver::parsePattern(std::string_view pattern) {
    if (pattern.size() > 3 * 512) throw std::invalid_argument("signature too large");
    BytePattern result;
    std::size_t start = 0;
    while (start < pattern.size()) {
        while (start < pattern.size() && std::isspace(static_cast<unsigned char>(pattern[start]))) ++start;
        if (start == pattern.size()) break;
        auto end = pattern.find_first_of(" \t\r\n", start);
        if (end == std::string_view::npos) end = pattern.size();
        auto tok = pattern.substr(start, end - start);
        if (tok == "?" || tok == "??") {
            result.bytes.push_back(0);
            result.mask.push_back(0);
        } else if (tok.size() == 2 &&
                   std::isxdigit(static_cast<unsigned char>(tok[0])) &&
                   std::isxdigit(static_cast<unsigned char>(tok[1]))) {
            auto hex = [](char c) -> unsigned {
                if (c >= '0' && c <= '9') return static_cast<unsigned>(c - '0');
                if (c >= 'A' && c <= 'F') return static_cast<unsigned>(c - 'A' + 10);
                return static_cast<unsigned>(c - 'a' + 10);
            };
            result.bytes.push_back(static_cast<std::uint8_t>(hex(tok[0])*16 + hex(tok[1])));
            result.mask.push_back(1);
        } else {
            throw std::invalid_argument("invalid pattern token");
        }
        start = end;
    }
    if (result.bytes.size() > 512) throw std::invalid_argument("too many pattern bytes");
    return result;
}

bool Resolver::matches(const std::byte* bytes, const BytePattern& pat) {
    for (std::size_t i = 0; i < pat.bytes.size(); ++i)
        if (pat.mask[i] && static_cast<std::uint8_t>(bytes[i]) != pat.bytes[i])
            return false;
    return true;
}

std::optional<std::size_t> Resolver::findUnique(std::span<const std::byte> haystack,
                                                const BytePattern& pat) {
    if (pat.bytes.empty() || pat.bytes.size() > haystack.size()) return std::nullopt;
    // Find the longest fixed run as a fast anchor.
    std::size_t anchorAt = 0, anchorLen = 0;
    for (std::size_t i = 0; i < pat.mask.size();) {
        if (!pat.mask[i]) { ++i; continue; }
        std::size_t j = i;
        while (j < pat.mask.size() && pat.mask[j]) ++j;
        if (j - i > anchorLen) { anchorAt = i; anchorLen = j - i; }
        i = j;
    }
    if (anchorLen < 4) return std::nullopt;
    std::optional<std::size_t> hit;
    const auto* begin = reinterpret_cast<const std::uint8_t*>(haystack.data());
    const auto* end = begin + haystack.size();
    auto* seek = begin + anchorAt;
    while (seek + anchorLen <= end) {
        const auto* found = std::search(seek, end,
                                       pat.bytes.begin()+anchorAt,
                                       pat.bytes.begin()+anchorAt+anchorLen);
        if (found == end) break;
        const std::size_t start = static_cast<std::size_t>(found-begin)-anchorAt;
        if (start+pat.bytes.size() <= haystack.size() &&
            matches(haystack.data()+start, pat)) {
            if (hit.has_value()) return std::nullopt; // ambiguous
            hit = start;
        }
        seek = found + 1;
    }
    return hit;
}

bool Resolver::loadJson(std::string_view source, std::string& error) {
    entries_.clear(); expectedHash_.clear(); expectedTextHash_.clear(); image_ = nullptr; exactBuild_ = false;
    try {
        auto doc = nlohmann::json::parse(source.begin(), source.end());
        if (doc.at("schema").get<int>() != 1) throw std::invalid_argument("unsupported schema");
        const auto& target = doc.at("target");
        if (target.at("platform") != "windows" || target.at("arch") != "x64")
            throw std::invalid_argument("expected windows/x64 database");
        expectedHash_ = lower(target.at("sha256").get<std::string>());
        expectedTextHash_ = lower(target.value("text_sha256", std::string{}));
        if (!sha256hex(expectedHash_) ||
            (!expectedTextHash_.empty() && !sha256hex(expectedTextHash_)))
            throw std::invalid_argument("invalid SHA256");
        targetSize_ = target.at("size_of_image").get<std::uint32_t>();
        targetTextRva_ = toRva(target.at("text_rva"));
        targetTextSize_ = target.value("text_size", std::size_t{0});
        const auto& syms = doc.at("symbols");
        if (!syms.is_object() || syms.size() > 200000)
            throw std::invalid_argument("invalid symbol map size");
        for (auto it=syms.begin(); it!=syms.end(); ++it) {
            const auto& v = it.value();
            Entry e;
            e.rva = toRva(v.at("rva"));
            e.verified = v.value("status", "candidate") == "verified";
            e.signature = parsePattern(v.value("signature", ""));
            e.entryCheck = parsePattern(v.value("entry_check", ""));
            if (e.entryCheck.bytes.empty())
                throw std::invalid_argument("missing entry guard for "+it.key());
            if (e.signature.bytes.empty() && v.value("cross_version_signature_available", true))
                throw std::invalid_argument("empty pattern requires explicit RVA-only flag: "+it.key());
            entries_.emplace(it.key(), std::move(e));
        }
        error.clear();
        return true;
    } catch (const std::exception& exc) {
        entries_.clear();
        error = exc.what();
        return false;
    }
}

bool Resolver::attachImage(const std::byte* moduleBase, std::size_t mappedSize,
                           std::string actualFileSha256, std::uint32_t textRva,
                           std::size_t textSize, std::string& error) {
    image_ = nullptr; exactBuild_ = false;
    if (!moduleBase || !mappedSize || textRva >= mappedSize ||
        textSize > mappedSize - textRva || expectedHash_.empty() ||
        !sha256hex(actualFileSha256)) {
        error = "invalid mapped image, text range, or module SHA256";
        return false;
    }
    image_ = moduleBase;
    imageSize_ = mappedSize;
    textRva_ = textRva;
    textSize_ = textSize;
    exactBuild_ = lower(std::move(actualFileSha256)) == expectedHash_
                  && targetSize_ == mappedSize && targetTextRva_ == textRva;
    error.clear();
    return true;
}

bool Resolver::attachImageByTextHash(
    const std::byte* moduleBase, std::size_t mappedSize,
    std::string actualTextSha256, std::uint32_t textRva,
    std::size_t textSize, std::string& error) {
    // No implicit trust of unknown builds or a prefix-only checksum.
    image_ = nullptr; exactBuild_ = false;
    if (!moduleBase || !mappedSize || textRva >= mappedSize ||
        textSize > mappedSize - textRva ||
        expectedTextHash_.empty() || !sha256hex(actualTextSha256) ||
        targetTextSize_ != textSize || targetTextRva_ != textRva ||
        targetSize_ != mappedSize ||
        lower(std::move(actualTextSha256)) != expectedTextHash_) {
        error = "loaded .text hash or PE layout does not match supported build";
        return false;
    }
    image_ = moduleBase;
    imageSize_ = mappedSize;
    textRva_ = textRva;
    textSize_ = textSize;
    exactBuild_ = true; // exact code identity; .data layout not independently proven.
    error.clear();
    return true;
}

std::optional<Resolution> Resolver::resolve(std::string_view name, bool allowCandidates) const {
    if (!image_) return std::nullopt;
    auto it = entries_.find(std::string{name});
    if (it == entries_.end()) return std::nullopt;
    const auto& e = it->second;
    if (exactBuild_ && (e.verified || allowCandidates) &&
        e.rva < imageSize_ && e.entryCheck.bytes.size() <= imageSize_ - e.rva &&
        matches(image_ + e.rva, e.entryCheck))
        return Resolution{reinterpret_cast<std::uintptr_t>(image_ + e.rva),
                          "exact-rva", true, e.verified};
    if (!allowCandidates) return std::nullopt;
    auto hit = findUnique(std::span<const std::byte>(image_ + textRva_, textSize_), e.signature);
    if (!hit.has_value()) return std::nullopt;
    return Resolution{reinterpret_cast<std::uintptr_t>(image_ + textRva_ + *hit),
                      "aob-candidate", exactBuild_, false};
}
} // namespace bedrock
