#pragma once

#include <bedrock/detail/client_access.hpp>

#include <algorithm>
#include <cctype>

namespace bedrock::detail {

struct ActorIdentity {
    std::string nameTag;
    std::string name;
    bool isPlayer{};
};

inline std::string stripActorFormatting(std::string_view text) {
    std::string result;
    result.reserve(text.size());
    for (std::size_t i = 0; i < text.size();) {
        const auto c = static_cast<unsigned char>(text[i]);
        if (c == 0xC2 && i + 2 < text.size() &&
            static_cast<unsigned char>(text[i + 1]) == 0xA7) {
            i += 3;
        } else if (c == 0xA7 && i + 1 < text.size()) {
            i += 2;
        } else {
            result.push_back(text[i++]);
        }
    }
    return result;
}

inline ActorIdentity readActorIdentity(const ClientAccess& access, void* actor) {
    if (!actor || !access.isActorValid(actor)) return {};
    ActorIdentity result;
    if (auto tag = access.nameTag(actor)) result.nameTag = std::move(*tag);
    result.name = stripActorFormatting(result.nameTag);
    if (result.name.empty()) return result;
    auto* level = access.worldOf(actor);
    if (!level) return result;
    std::string best;
    for (const auto& rawName : access.playerListNames(level)) {
        auto name = stripActorFormatting(rawName);
        if (name.size() < 2 || name.size() <= best.size()) continue;
        if (std::search(result.name.begin(), result.name.end(), name.begin(), name.end(),
                        [](unsigned char a, unsigned char b) {
                            return std::tolower(a) == std::tolower(b);
                        }) != result.name.end()) best = std::move(name);
    }
    if (!best.empty()) {
        result.name = std::move(best);
        result.isPlayer = true;
    }
    return result;
}

} // namespace bedrock::detail
