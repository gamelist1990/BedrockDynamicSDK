#pragma once
#include <bedrock/detail/native_profile_functions.hpp>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <array>
#include <span>
#include <vector>
#include <cstdio>

namespace bedrock {
// Read-only process memory access. Never changes page protection or touches
// PAGE_GUARD. Call from a worker and retain owned bytes, not native pointers.
class MemoryInspector {
  public:
    struct Range {
        std::uintptr_t address{};
        std::size_t bytes{};
    };
    static bool readable(DWORD protection) noexcept {
        if (protection & (PAGE_GUARD | PAGE_NOACCESS))
            return false;
        const auto access = protection & 0xff;
        return access == PAGE_READONLY || access == PAGE_READWRITE || access == PAGE_WRITECOPY ||
               access == PAGE_EXECUTE_READ || access == PAGE_EXECUTE_READWRITE ||
               access == PAGE_EXECUTE_WRITECOPY;
    }
    static bool copy(std::uintptr_t address, std::span<std::byte> output) noexcept {
        if (output.empty())
            return true;
        if (!address || address > UINTPTR_MAX - output.size())
            return false;
        std::size_t copied{};
        while (copied < output.size()) {
            MEMORY_BASIC_INFORMATION region{};
            if (!VirtualQuery(reinterpret_cast<void *>(address + copied), &region,
                              sizeof(region)) ||
                region.State != MEM_COMMIT || !readable(region.Protect))
                return false;
            const auto end =
                reinterpret_cast<std::uintptr_t>(region.BaseAddress) + region.RegionSize;
            if (end <= address + copied)
                return false;
            const auto bytes =
                std::min(output.size() - copied, static_cast<std::size_t>(end - address - copied));
            SIZE_T done{};
            if (!ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<void *>(address + copied),
                                   output.data() + copied, bytes, &done) ||
                done != bytes)
                return false;
            copied += bytes;
        }
        return true;
    }
    static nlohmann::json inspect(Range request, std::size_t rowBytes = 256) {
        if (!request.bytes || request.bytes > 1024 * 1024 || !request.address ||
            request.address > UINTPTR_MAX - request.bytes || !rowBytes || rowBytes > 4096)
            throw std::invalid_argument(
                "memory range: 1..1048576 bytes, rows 1..4096, no x64 overflow");
        if ((request.bytes + rowBytes - 1) / rowBytes > 4096)
            throw std::invalid_argument("memory range exceeds 4096 rows; increase row_bytes");
        nlohmann::json rows = nlohmann::json::array();
        std::size_t offset{}, read{};
        while (offset < request.bytes) {
            MEMORY_BASIC_INFORMATION region{};
            const auto address = request.address + offset;
            if (!VirtualQuery(reinterpret_cast<void *>(address), &region, sizeof(region)))
                break;
            const auto end =
                reinterpret_cast<std::uintptr_t>(region.BaseAddress) + region.RegionSize;
            if (end <= address)
                break;
            const auto count = std::min(
                {request.bytes - offset, rowBytes, static_cast<std::size_t>(end - address)});
            nlohmann::json row = {{"address", hex(address)},
                                  {"size", count},
                                  {"state", region.State},
                                  {"protection", region.Protect}};
            std::vector<std::byte> bytes(count);
            const bool success =
                region.State == MEM_COMMIT && readable(region.Protect) && copy(address, bytes);
            row["readable"] = success;
            if (success) {
                read += count;
                row["bytes"] = bytesHex(bytes);
            } else
                row["reason"] = "unmapped, guarded, unreadable or changed while copying";
            rows.push_back(std::move(row));
            offset += count;
        }
        return {{"address", hex(request.address)},
                {"requested_bytes", request.bytes},
                {"read_bytes", read},
                {"unreadable_bytes", request.bytes - read},
                {"status", read == request.bytes ? "complete"
                           : read                ? "partial"
                                                 : "unreadable"},
                {"read_only", true},
                {"regions", std::move(rows)}};
    }
    static std::string hex(std::uint64_t value) {
        char buffer[24]{};
        sprintf_s(buffer, "0x%llx", static_cast<unsigned long long>(value));
        return buffer;
    }
    static std::string bytesHex(std::span<const std::byte> bytes) {
        static constexpr char digits[] = "0123456789abcdef";
        std::string result;
        result.reserve(bytes.size() * 3);
        for (auto b : bytes) {
            if (!result.empty())
                result += ' ';
            const auto value = std::to_integer<unsigned>(b);
            result += digits[value >> 4];
            result += digits[value & 15];
        }
        return result;
    }
};
struct LoadedSection {
    std::string name;
    std::uintptr_t address{};
    std::size_t size{};
    DWORD characteristics{};
};
// PE section boundaries are read from the actual loaded module, not per-Mod
// offsets. The caller's profile must already match this module's identity.
inline std::vector<LoadedSection> loadedMainSections() {
    const auto base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    IMAGE_DOS_HEADER dos{};
    if (!native_profile::read(reinterpret_cast<void *>(base), 0, dos) ||
        dos.e_magic != IMAGE_DOS_SIGNATURE || dos.e_lfanew <= 0 || dos.e_lfanew > 0x100000)
        throw std::runtime_error("invalid loaded DOS header");
    IMAGE_NT_HEADERS64 nt{};
    if (!native_profile::read(reinterpret_cast<void *>(base), dos.e_lfanew, nt) ||
        nt.Signature != IMAGE_NT_SIGNATURE || nt.FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64 ||
        nt.FileHeader.NumberOfSections > 96 ||
        nt.OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC)
        throw std::runtime_error("invalid loaded AMD64 PE header");
    std::vector<LoadedSection> result;
    const auto table = base + dos.e_lfanew + sizeof(DWORD) + sizeof(IMAGE_FILE_HEADER) +
                       nt.FileHeader.SizeOfOptionalHeader;
    for (unsigned i = 0; i < nt.FileHeader.NumberOfSections; ++i) {
        IMAGE_SECTION_HEADER section{};
        if (!native_profile::read(reinterpret_cast<void *>(table), i * sizeof(section), section))
            throw std::runtime_error("unreadable loaded section header");
        const auto size = std::max(section.Misc.VirtualSize, section.SizeOfRawData);
        if (!size || std::uint64_t(section.VirtualAddress) + size > nt.OptionalHeader.SizeOfImage)
            continue;
        std::size_t length{};
        while (length < 8 && section.Name[length])
            ++length;
        result.push_back({std::string(reinterpret_cast<const char *>(section.Name), length),
                          base + section.VirtualAddress, size, section.Characteristics});
    }
    return result;
}
} // namespace bedrock
