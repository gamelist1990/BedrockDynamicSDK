#pragma once
#if !defined(_WIN32) || !defined(_MSC_VER) || !defined(_WIN64)
#error Native profile functions require Windows x64 MSVC.
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <bedrock/profiles/profile_manager.hpp>
#include <nlohmann/json.hpp>
#include <cstring>
#include <cstddef>
#include <cstdint>
#include <stdexcept>

namespace bedrock::native_profile {
template<class T> bool read(const void* pointer, std::size_t offset, T& output) noexcept {
    if (!pointer) return false;
    __try { std::memcpy(&output, static_cast<const std::byte*>(pointer) + offset, sizeof(T)); return true; }
    __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
}
inline std::uint32_t number(const nlohmann::json& value) {
    const auto result = value.is_string() ? std::stoull(value.get<std::string>(), nullptr, 0) : value.get<std::uint64_t>();
    if (result > UINT32_MAX) throw std::runtime_error("native profile integer overflow");
    return static_cast<std::uint32_t>(result);
}
// Fill a missing adapter layout only from an identical executable definition.
inline std::string_view layoutProfile(std::string_view selected,std::string_view embedded,const char* layout) {
    const auto root=nlohmann::json::parse(selected);
    if(root.contains("layouts")&&root.at("layouts").contains(layout))return selected;
    const auto fallback=nlohmann::json::parse(embedded);
    for(const auto* field:{"sha256","text_sha256","size_of_image","text_rva","text_size"})
        if(root.at("target").at(field)!=fallback.at("target").at(field))
            throw std::runtime_error("embedded adapter belongs to another executable");
    if(!fallback.at("layouts").contains(layout))throw std::runtime_error(std::string("missing SDK layout: ")+layout);
    return embedded;
}
inline const nlohmann::json& functionDefinition(const nlohmann::json& root,const nlohmann::json& reference) {
    if(reference.contains("symbol")&&!reference.contains("rva"))return root.at("symbols").at(reference.at("symbol").get<std::string>());
    return reference;
}
inline void* checked(const nlohmann::json& root, const nlohmann::json& reference) {
    const auto& function=functionDefinition(root,reference);
    const auto base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    const auto rva = number(function.at("rva"));
    const auto begin = number(root.at("target").at("text_rva"));
    const auto size = number(root.at("target").at("text_size"));
    if (rva < begin || std::uint64_t{rva} >= std::uint64_t{begin} + size)
        throw std::runtime_error("native function outside executable text");
    const auto guard = function.at("entry_check").get<std::string>();
    if (guard.size() < 23 || guard.size() > 191 || (guard.size() + 1) % 3)
        throw std::runtime_error("invalid native entry guard");
    auto* target = reinterpret_cast<void*>(base + rva);
    for (std::size_t i = 0; i < guard.size(); i += 3) {
        unsigned char actual{};
        if (!read(target, i / 3, actual) || actual != std::stoul(guard.substr(i, 2), nullptr, 16))
            throw std::runtime_error("native entry guard mismatch");
    }
    return target;
}
} // namespace bedrock::native_profile
