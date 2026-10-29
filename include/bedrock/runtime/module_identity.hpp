#pragma once
#include <bedrock/detail/native_profile_functions.hpp>
#include <string>
#ifndef BEDROCK_SDK_BUILD_ID
#define BEDROCK_SDK_BUILD_ID "unrecorded"
#endif
namespace bedrock {
// Identity of the module containing an explicitly supplied function address.
// Works when the loader assigned a generated directory or loaded several Mods.
inline std::string moduleIdentity(const void* address) {
    HMODULE module{};
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
            GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(address), &module)) return "module=unavailable";
    std::wstring path(32768, L'\0');
    const auto count = GetModuleFileNameW(module, path.data(), static_cast<DWORD>(path.size()));
    if (!count || count >= path.size()) return "module=unavailable";
    const auto bytes = WideCharToMultiByte(CP_UTF8, 0, path.data(), count, nullptr, 0, nullptr, nullptr);
    std::string result(bytes, '\0');
    WideCharToMultiByte(CP_UTF8, 0, path.data(), count, result.data(), bytes, nullptr, nullptr);
    return "build=" BEDROCK_SDK_BUILD_ID " module=" + result;
}
} // namespace bedrock
