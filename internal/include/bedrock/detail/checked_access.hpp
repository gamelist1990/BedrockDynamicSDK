#pragma once
#include <windows.h>
#include <cstddef>
#include <cstdint>
#include <utility>

namespace bedrock::detail {

//
// Memory-misuse guard primitives for the SDK native layer.
//
// Strategy (per design memo):
//   1. VirtualQuery-based range validation BEFORE touching memory
//   2. SEH __try/__except as the second line of defence
//   3. SEH code lives in small functions with POD-only parameters/locals
//      (MSVC C2712 constraint — strict even where clang-cl is lenient).
//
// All validation logic is pure (no SEH) so it is unit-testable without fault
// injection where possible.

using ValidRange = std::pair<const void*, std::size_t>;

//
// Walk memory regions covering [start, start+bytes): every page of the
// range must be committed, non-guard, non-noaccess, and readable (or
// writable, for write validation). Returns {pointer, bytes} when the
// whole range is usable, {nullptr, 0} otherwise.
//
inline bool protectionReadable(DWORD protect) noexcept {
    switch (protect & 0xFFu) {
        case PAGE_READONLY:
        case PAGE_READWRITE:
        case PAGE_WRITECOPY:
        case PAGE_EXECUTE_READ:
        case PAGE_EXECUTE_READWRITE:
        case PAGE_EXECUTE_WRITECOPY:
            return true;
        default:
            return false;
    }
}

inline bool protectionWritable(DWORD protect) noexcept {
    switch (protect & 0xFFu) {
        case PAGE_READWRITE:
        case PAGE_WRITECOPY:
        case PAGE_EXECUTE_READWRITE:
        case PAGE_EXECUTE_WRITECOPY:
            return true;
        default:
            return false;
    }
}

inline ValidRange validateRange(const void* pointer, std::size_t bytes, bool write) noexcept {
    if (!pointer || !bytes) return {nullptr, 0};
    const auto start = reinterpret_cast<std::uintptr_t>(pointer);
    if (start > UINTPTR_MAX - bytes) return {nullptr, 0};
    const auto wantedEnd = start + bytes;
    for (std::uintptr_t cursor = start; cursor < wantedEnd;) {
        MEMORY_BASIC_INFORMATION region{};
        if (!VirtualQuery(reinterpret_cast<const void*>(cursor), &region, sizeof(region)))
            return {nullptr, 0};
        if (region.State != MEM_COMMIT) return {nullptr, 0};
        if (region.Protect & (PAGE_GUARD | PAGE_NOACCESS)) return {nullptr, 0};
        if (write ? !protectionWritable(region.Protect)
                  : !protectionReadable(region.Protect))
            return {nullptr, 0};
        const auto regionEnd = reinterpret_cast<std::uintptr_t>(region.BaseAddress) +
                               region.RegionSize;
        if (regionEnd <= cursor) return {nullptr, 0};
        cursor = regionEnd;
    }
    return {pointer, bytes};
}

//
// validateReadRange - the range must be committed & readable.
//
inline ValidRange validateReadRange(const void* pointer, std::size_t bytes) noexcept {
    return validateRange(pointer, bytes, false);
}

//
// validateWriteRange - the range must be committed & writable.
//
inline ValidRange validateWriteRange(const void* pointer, std::size_t bytes) noexcept {
    return validateRange(pointer, bytes, true);
}

//
// checkedRead - VirtualQuery range validation (validateReadRange) then SEH.
// POD-only SEH body (MSVC C2712).
//
template <class T>
inline bool checkedRead(const void* pointer, std::size_t offset, T& result) noexcept {
    if (!pointer) return false;
    const auto base = static_cast<const std::byte*>(pointer) + offset;
    auto [validBase, validBytes] = validateReadRange(base, sizeof(T));
    if (!validBase || validBytes < sizeof(T)) return false;
    __try {
        result = *reinterpret_cast<const T*>(validBase);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

//
// checkedWrite - VirtualQuery range validation (validateWriteRange) then SEH.
// POD-only SEH body (MSVC C2712).
//
template <class T>
inline bool checkedWrite(void* pointer, std::size_t offset, const T& value) noexcept {
    if (!pointer) return false;
    auto base = static_cast<std::byte*>(pointer) + offset;
    auto [validBase, validBytes] = validateWriteRange(reinterpret_cast<const void*>(base), sizeof(T));
    if (!validBase || validBytes < sizeof(T)) return false;
    __try {
        *reinterpret_cast<T*>(base) = value;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

//
// validateCallTarget - verify that a call target derived from a profile RVA
// lies inside the main module's executable (.text) range before calling it
// through a guarded path. Pure validation (no SEH), returns bool.
//
inline bool validateCallTarget(std::uintptr_t targetRva, std::uintptr_t moduleBase, std::size_t moduleSize) noexcept {
    if (!moduleSize || targetRva > moduleSize) return false;
    const auto addr = moduleBase + targetRva;
    MEMORY_BASIC_INFORMATION info{};
    if (!VirtualQuery(reinterpret_cast<const void*>(addr), &info, sizeof(info))) return false;
    if (info.State != MEM_COMMIT || info.Type != MEM_IMAGE) return false;
    const DWORD protection = info.Protect & 0xFFu;
    return protection == PAGE_EXECUTE ||
           protection == PAGE_EXECUTE_READ ||
           protection == PAGE_EXECUTE_READWRITE ||
           protection == PAGE_EXECUTE_WRITECOPY;
}

} // namespace bedrock::detail