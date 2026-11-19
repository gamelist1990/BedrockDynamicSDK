#include "diagnostics.hpp"

namespace automine::client::crashrec {

void appendHex(char *&p, const char *end, unsigned long long v, int width = 0);

constexpr int kRingSize = 48;
constexpr int kLineSize = 168;
char g_ring[kRingSize][kLineSize]{};
volatile LONG g_ringNext = 0;
char g_section[64] = "init";
volatile LONG g_ticks = 0;
volatile LONG g_sehFaults = 0;
volatile LONG g_reported = 0;
HANDLE g_file = INVALID_HANDLE_VALUE;

void copyLine(char *dst, const char *src, int cap) {
    int i = 0;
    for (; i < cap - 1 && src[i]; ++i)
        dst[i] = src[i];
    dst[i] = 0;
}
void ringPush(const char *text) {
    const LONG i = InterlockedIncrement(&g_ringNext) - 1;
    copyLine(g_ring[i % kRingSize], text, kLineSize);
}
void setSection(const char *name) {
    copyLine(g_section, name, sizeof(g_section));
}

void append(char *&p, const char *end, const char *s) {
    while (*s && p < end)
        *p++ = *s++;
}
void appendHex(char *&p, const char *end, unsigned long long v, int width) {
    char tmp[32];
    int n = 0;
    if (!v)
        tmp[n++] = '0';
    while (v) {
        const int d = static_cast<int>(v & 0xF);
        tmp[n++] = static_cast<char>(d < 10 ? '0' + d : 'a' + d - 10);
        v >>= 4;
    }
    while (n < width)
        tmp[n++] = '0';
    while (n > 0 && p < end)
        *p++ = tmp[--n];
}
void appendDec(char *&p, const char *end, long long v) {
    if (v < 0) {
        if (p < end)
            *p++ = '-';
        v = -v;
    }
    char tmp[24];
    int n = 0;
    if (!v)
        tmp[n++] = '0';
    while (v) {
        tmp[n++] = static_cast<char>('0' + (v % 10));
        v /= 10;
    }
    while (n > 0 && p < end)
        *p++ = tmp[--n];
}
void moduleOf(char *&p, const char *end, const void *addr) {
    HMODULE h{};
    if (addr &&
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                               GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           static_cast<LPCWSTR>(addr), &h) &&
        h) {
        wchar_t path[260]{};
        const DWORD n = GetModuleFileNameW(h, path, 260);
        const wchar_t *base = path;
        for (DWORD i = 0; i < n; ++i)
            if (path[i] == L'\\' || path[i] == L'/')
                base = path + i + 1;
        for (const wchar_t *s = base; *s && p < end; ++s)
            *p++ = static_cast<char>(*s);
        append(p, end, "+0x");
        appendHex(p, end,
                  static_cast<unsigned long long>(reinterpret_cast<const char *>(addr) -
                                                  reinterpret_cast<const char *>(h)));
    } else
        append(p, end, "?");
}
LONG WINAPI veh(EXCEPTION_POINTERS *ep) {
    const DWORD code = ep && ep->ExceptionRecord ? ep->ExceptionRecord->ExceptionCode : 0;
    const bool fatal = code == EXCEPTION_ACCESS_VIOLATION ||
                       code == 0xC0000374ul || // STATUS_HEAP_CORRUPTION
                       code == EXCEPTION_STACK_OVERFLOW || code == EXCEPTION_ILLEGAL_INSTRUCTION ||
                       code == EXCEPTION_INT_DIVIDE_BY_ZERO || code == 0xC0000006ul;
    if (!fatal || InterlockedExchange(&g_reported, 1))
        return EXCEPTION_CONTINUE_SEARCH;
    if (g_file == INVALID_HANDLE_VALUE || !ep->ContextRecord)
        return EXCEPTION_CONTINUE_SEARCH;
    char buf[16384];
    char *p = buf;
    const char *const end = buf + sizeof(buf) - 64;
    append(p, end, "==== AutoMine crash report ====\ncode=0x");
    appendHex(p, end, code, 8);
    append(p, end, " section=");
    append(p, end, g_section);
    append(p, end, " ticks=");
    appendDec(p, end, g_ticks);
    append(p, end, " sehFaults=");
    appendDec(p, end, g_sehFaults);
    append(p, end, "\n");
    auto *ctx = ep->ContextRecord;
    append(p, end, "rip=0x");
    appendHex(p, end, ctx->Rip, 16);
    append(p, end, " ");
    moduleOf(p, end, reinterpret_cast<const void *>(ctx->Rip));
    append(p, end, "\n");
    append(p, end, "faultAddr=0x");
    appendHex(p, end,
              static_cast<unsigned long long>(
                  reinterpret_cast<uintptr_t>(ep->ExceptionRecord->ExceptionAddress)),
              16);
    append(p, end, " ");
    moduleOf(p, end, ep->ExceptionRecord->ExceptionAddress);
    append(p, end, "\n");
    if (code == EXCEPTION_ACCESS_VIOLATION && ep->ExceptionRecord->NumberParameters >= 2) {
        append(p, end, "av: ");
        append(p, end, ep->ExceptionRecord->ExceptionInformation[0] ? "write" : "read");
        append(p, end, " at 0x");
        appendHex(p, end, ep->ExceptionRecord->ExceptionInformation[1], 16);
        append(p, end, "\n");
    }
    append(p, end, "rsp=0x");
    appendHex(p, end, ctx->Rsp, 16);
    append(p, end, " rbp=0x");
    appendHex(p, end, ctx->Rbp, 16);
    append(p, end, "\nstack:\n");
    MEMORY_BASIC_INFORMATION mbi{};
    if (VirtualQuery(reinterpret_cast<const void *>(ctx->Rsp), &mbi, sizeof(mbi)) &&
        (mbi.State & MEM_COMMIT)) {
        const auto *q = reinterpret_cast<const unsigned long long *>(ctx->Rsp);
        const auto *limit = reinterpret_cast<const unsigned long long *>(
            static_cast<const char *>(mbi.BaseAddress) + mbi.RegionSize);
        if (limit > q + 96)
            limit = q + 96;
        for (; q < limit; ++q) {
            const unsigned long long v = *q;
            HMODULE h{};
            if (v > 0x10000 &&
                GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                       GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                   reinterpret_cast<LPCWSTR>(v), &h) &&
                h) {
                append(p, end, "  0x");
                appendHex(p, end, v, 16);
                append(p, end, " ");
                moduleOf(p, end, reinterpret_cast<const void *>(v));
                append(p, end, "\n");
            }
        }
    }
    append(p, end, "recent notes (oldest first):\n");
    const LONG total = g_ringNext;
    const LONG start = total > kRingSize ? total - kRingSize : 0;
    for (LONG i = start; i < total; ++i) {
        append(p, end, "  ");
        append(p, end, g_ring[i % kRingSize]);
        append(p, end, "\n");
    }
    append(p, end, "==== end ====\n");
    DWORD written{};
    WriteFile(g_file, buf, static_cast<DWORD>(p - buf), &written, nullptr);
    FlushFileBuffers(g_file);
    return EXCEPTION_CONTINUE_SEARCH;
}
void install(const std::filesystem::path &dir) {
    const auto path = dir / L"crash.log";
    g_file = CreateFileW(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ, nullptr, OPEN_ALWAYS,
                         FILE_ATTRIBUTE_NORMAL, nullptr);
    AddVectoredExceptionHandler(1, &veh);
}

Section::Section(const char *name) {
    copyLine(prev, g_section, sizeof(prev));
    setSection(name);
}
Section::~Section() {
    copyLine(g_section, prev, sizeof(g_section));
}

} // namespace automine::client::crashrec
