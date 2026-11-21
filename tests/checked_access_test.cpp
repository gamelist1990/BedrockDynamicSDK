// checked_access_test.cpp — Unit tests for the memory-misuse guard primitives.
//
// Verifies: null address, address 0x1, unmapped high address,
// valid stack/heap round-trip, oversized length crossing page boundary,
// call-target validation accept/reject.
#include <bedrock/detail/checked_access.hpp>

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <vector>

using bedrock::detail::checkedRead;
using bedrock::detail::checkedWrite;
using bedrock::detail::validateReadRange;
using bedrock::detail::validateWriteRange;
using bedrock::detail::validateRange;
using bedrock::detail::validateCallTarget;

static int checks = 0;
static int failures = 0;

#define CHECK(condition, message)                                                          \
    do {                                                                                   \
        ++checks;                                                                          \
        if (!(condition)) {                                                                \
            ++failures;                                                                    \
            std::cerr << "FAIL check #" << checks << ": " << message << "\n";              \
        }                                                                                  \
    } while (0)

static int callTargetProbe() { return 42; }

int main() {
    // Keep the probe reachable so the linker cannot discard it.
    CHECK(callTargetProbe() == 42, "call target probe callable");
    // --- Null address ---
    {
        auto [valid, bytes] = validateReadRange(nullptr, 16);
        CHECK(valid == nullptr, "null address must be invalid");
        CHECK(bytes == 0, "null address must report zero bytes");
    }

    // --- Address 0x1 ---
    {
        auto [valid, bytes] = validateReadRange(reinterpret_cast<const void*>(0x1), 16);
        CHECK(valid == nullptr, "address 0x1 must be invalid");
    }

    // --- Unmapped high address ---
    {
        auto [valid, bytes] = validateReadRange(reinterpret_cast<const void*>(0xFFFFFFFFFF000000ULL), 16);
        CHECK(valid == nullptr, "unmapped high address must be invalid");
    }

    // --- Overflow check: bytes so large that start+bytes overflows ---
    {
        auto [valid, bytes] = validateReadRange(reinterpret_cast<const void*>(UINTPTR_MAX - 8), 16);
        CHECK(valid == nullptr, "range crossing UINTPTR_MAX must be invalid");
    }

    // --- Valid stack/heap buffer round-trip (read+write) ---
    {
        // Stack buffer.
        alignas(8) char stackBuf[64] = {0};
        auto [v, b] = validateReadRange(stackBuf, sizeof(stackBuf));
        CHECK(v != nullptr, "stack buffer must be valid");
        CHECK(b == sizeof(stackBuf), "stack buffer must report full size");

        // Read back a value through checkedRead.
        std::int32_t val = 0xDEADBEEF;
        std::memcpy(stackBuf, &val, sizeof(val));
        std::int32_t out = 0;
        CHECK(checkedRead(stackBuf, 0, out), "checkedRead on stack buffer must succeed");
        CHECK(out == 0xDEADBEEF, "checkedRead must return the stored value");

        // Write through checkedWrite.
        std::int32_t newVal = 0x12345678;
        CHECK(checkedWrite(stackBuf, 0, newVal), "checkedWrite on stack buffer must succeed");
        std::int32_t verify = 0;
        std::memcpy(&verify, stackBuf, sizeof(verify));
        CHECK(verify == 0x12345678, "checkedWrite must store the value");

        // Heap buffer via checkedRead with offset.
        std::vector<std::byte> heapBuf(128);
        std::int64_t magic = 0xCAFEBABECAFEBABELL;
        std::memcpy(heapBuf.data() + 32, &magic, sizeof(magic));
        std::int64_t out2 = 0;
        CHECK(checkedRead(heapBuf.data(), 32, out2), "checkedRead on heap buffer with offset must succeed");
        CHECK(out2 == magic, "checkedRead offset must read the correct value");
    }

    // --- Range crossing from a committed page into a decommitted page ---
    {
        // All-or-nothing contract: if any page of the range is unusable,
        // the whole range is rejected (callers never see partial ranges).
        void* region = VirtualAlloc(nullptr, 8192, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        CHECK(region != nullptr, "VirtualAlloc two pages for crossing test");
        if (region) {
            auto* second = static_cast<void*>(static_cast<char*>(region) + 4096);
            CHECK(VirtualFree(second, 4096, MEM_DECOMMIT) != 0, "decommit second page");
            auto [okV, okB] = validateReadRange(region, 4096);
            CHECK(okV != nullptr && okB == 4096, "single committed page validates");
            auto [v, b] = validateReadRange(region, 8192);
            CHECK(v == nullptr && b == 0,
                  "range crossing into a decommitted page must be rejected wholesale");
            VirtualFree(region, 0, MEM_RELEASE);
        }
    }

    // --- Oversized length crossing into unmapped page ---
    {
        // Try to validate a huge range starting near the end of address space.
        char buf[8] = {0};
        auto [v, b] = validateReadRange(buf, UINTPTR_MAX - reinterpret_cast<std::uintptr_t>(buf));
        // Even if buf itself is valid, asking for UINTPTR_MAX-range should fail
        // because pages beyond the buffer are not committed.
        CHECK(v == nullptr || b == 0 || b <= sizeof(buf), "oversized length into unmapped page must be rejected");
    }

    // --- Call-target validation: accept a real function, reject data ---
    {
        const auto moduleBase = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
        const auto fnAddr = reinterpret_cast<std::uintptr_t>(&callTargetProbe);
        CHECK(validateCallTarget(fnAddr - moduleBase, moduleBase, 0x1000000),
              "call target at a real function in .text must be accepted");
        static std::uint32_t dataProbe = 0;
        const auto dataAddr = reinterpret_cast<std::uintptr_t>(&dataProbe);
        CHECK(!validateCallTarget(dataAddr - moduleBase, moduleBase, 0x1000000),
              "call target at a data variable must be rejected (not executable)");
        // RVA 0 is the PE header page: committed IMAGE but not executable.
        CHECK(!validateCallTarget(0, moduleBase, 0x1000000),
              "call target RVA=0 (PE headers) must be rejected");
    }

    // --- Call-target validation: reject (out-of-range RVA) ---
    {
        const auto moduleBase = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
        // A RVA far beyond any reasonable image size must be rejected.
        CHECK(!validateCallTarget(0xFFFFFFFFFFFFFFFFULL, moduleBase, 0x1000),
              "call target far beyond module size must be rejected");
        CHECK(!validateCallTarget(0x10000000ULL, moduleBase, 0x1000),
              "call target RVA beyond module size must be rejected");
    }

    // --- Read-only page: readable but not writable ---
    {
        void* page = VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        CHECK(page != nullptr, "VirtualAlloc for read-only test");
        if (page) {
            static_cast<std::uint32_t*>(page)[0] = 0xDEADBEEFu;
            DWORD oldProtect = 0;
            CHECK(VirtualProtect(page, 4096, PAGE_READONLY, &oldProtect) != 0,
                  "VirtualProtect to READONLY");
            auto [rvalid, rbytes] = validateReadRange(page, sizeof(std::uint32_t));
            CHECK(rvalid != nullptr && rbytes >= sizeof(std::uint32_t),
                  "read-only page must validate for reads");
            std::uint32_t value = 0;
            CHECK(checkedRead(page, 0, value) && value == 0xDEADBEEFu,
                  "checkedRead succeeds on read-only page");
            auto [wvalid, wbytes] = validateWriteRange(page, sizeof(std::uint32_t));
            CHECK(wvalid == nullptr, "read-only page must not validate for writes");
            CHECK(!checkedWrite(page, 0, std::uint32_t{0x1234u}),
                  "checkedWrite fails on read-only page");
            VirtualFree(page, 0, MEM_RELEASE);
        }
    }

    // Summary.
    std::cout << "checked_access_test: checks=" << checks << " failures=" << failures << "\n";
    return failures == 0 ? 0 : 1;
}