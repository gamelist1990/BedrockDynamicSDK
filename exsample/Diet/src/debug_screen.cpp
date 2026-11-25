// Diet — F3 debug screen implementation (Windows: ImGui through sdk::Overlay).
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <psapi.h>

#include <bedrock/sdk/overlay.hpp>
#include <imgui.h>

#include "diet/debug_screen.hpp"

#include <atomic>
#include <mutex>
#include <string>

namespace diet {
namespace {

// Java's debug-screen text background is 0x90505050 (translucent grey) and its
// text is white with a drop shadow.
constexpr ImU32 kBackground = IM_COL32(0x50, 0x50, 0x50, 0x90);
constexpr ImU32 kText = IM_COL32(255, 255, 255, 255);
constexpr ImU32 kShadow = IM_COL32(0, 0, 0, 120);
constexpr float kFontSize = 14.0f;
constexpr float kLineHeight = 16.0f;
constexpr float kMargin = 2.0f;

std::string narrow(const wchar_t* text) {
    if (!text || !*text) return {};
    const int length = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
    if (length <= 1) return {};
    std::string result(static_cast<std::size_t>(length - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text, -1, result.data(), length, nullptr, nullptr);
    return result;
}

std::string registryString(HKEY root, const wchar_t* key, const wchar_t* value) {
    HKEY handle{};
    if (RegOpenKeyExW(root, key, 0, KEY_QUERY_VALUE, &handle) != ERROR_SUCCESS) return {};
    wchar_t buffer[512]{};
    DWORD bytes = sizeof(buffer) - sizeof(wchar_t);
    DWORD type = 0;
    std::string result;
    if (RegQueryValueExW(handle, value, nullptr, &type, reinterpret_cast<LPBYTE>(buffer),
                         &bytes) == ERROR_SUCCESS &&
        type == REG_SZ) {
        buffer[bytes / sizeof(wchar_t)] = L'\0';
        result = narrow(buffer);
    }
    RegCloseKey(handle);
    // Trim trailing spaces some CPU strings carry.
    while (!result.empty() && result.back() == ' ') result.pop_back();
    return result;
}

std::string wineVersion() {
    // Wine exposes wine_get_version from ntdll. Absent on real Windows, so a
    // failure means "Windows" and the RtlGetVersion build is reported instead.
    if (HMODULE ntdll = GetModuleHandleW(L"ntdll.dll")) {
        using GetVersionFn = const char* (*)();
        if (auto getVersion = reinterpret_cast<GetVersionFn>(
                reinterpret_cast<void*>(GetProcAddress(ntdll, "wine_get_version")))) {
            std::string value = getVersion ? getVersion() : "";
            if (!value.empty()) return value;
        }
        using RtlGetVersionFn = LONG(WINAPI*)(PRTL_OSVERSIONINFOW);
        if (auto rtlGetVersion = reinterpret_cast<RtlGetVersionFn>(
                reinterpret_cast<void*>(GetProcAddress(ntdll, "RtlGetVersion")))) {
            RTL_OSVERSIONINFOW info{};
            info.dwOSVersionInfoSize = sizeof(info);
            if (rtlGetVersion(&info) == 0) {
                return std::to_string(info.dwMajorVersion) + "." +
                       std::to_string(info.dwMinorVersion) + "." +
                       std::to_string(info.dwBuildNumber);
            }
        }
    }
    return {};
}

std::string gpuDescription() {
    // DXGI is already loaded by the renderer; the adapter description is the
    // only honest GPU name available without touching game state.
    if (HMODULE dxgi = GetModuleHandleW(L"dxgi.dll")) {
        using CreateFactory1Fn = HRESULT(WINAPI*)(REFIID, void**);
        auto createFactory = reinterpret_cast<CreateFactory1Fn>(
            reinterpret_cast<void*>(GetProcAddress(dxgi, "CreateDXGIFactory1")));
        if (!createFactory) return {};
        struct Factory;  // opaque: only vtable calls below
        struct Adapter;
        struct Desc1 {
            wchar_t description[128];
            unsigned vendorId, deviceId, subSysId, revision;
            size_t dedicatedVideoMemory, dedicatedSystemMemory, sharedSystemMemory;
            unsigned adapterLuidLow, adapterLuidHigh;
            unsigned flags;
        };
        Factory* factory = nullptr;
        const GUID iidFactory1 = {0x770aae78, 0xf26f, 0x4dba,
                                  {0xa8, 0x29, 0x25, 0x3c, 0x83, 0xd1, 0xb3, 0x87}};
        if (FAILED(createFactory(iidFactory1, reinterpret_cast<void**>(&factory))) || !factory)
            return {};
        // IDXGIFactory1::EnumAdapters1 is vtable index 12.
        void** vtable = *reinterpret_cast<void***>(factory);
        using EnumAdapters1Fn = HRESULT(WINAPI*)(void*, UINT, Adapter**);
        Adapter* adapter = nullptr;
        const HRESULT enumerated =
            reinterpret_cast<EnumAdapters1Fn>(vtable[12])(factory, 0, &adapter);
        std::string result;
        if (SUCCEEDED(enumerated) && adapter) {
            void** adapterVtable = *reinterpret_cast<void***>(adapter);
            using GetDesc1Fn = HRESULT(WINAPI*)(void*, Desc1*);
            Desc1 desc{};
            if (SUCCEEDED(reinterpret_cast<GetDesc1Fn>(adapterVtable[10])(adapter, &desc)))
                result = narrow(desc.description);
            using ReleaseFn = ULONG(WINAPI*)(void*);
            reinterpret_cast<ReleaseFn>(adapterVtable[2])(adapter);
        }
        using ReleaseFn = ULONG(WINAPI*)(void*);
        reinterpret_cast<ReleaseFn>(vtable[2])(factory);
        return result;
    }
    return {};
}

struct SystemCache {
    bool initialised{};
    F3SystemInfo info{};
};
SystemCache systemCache;
std::mutex systemCacheMutex;

void refreshMemory(F3SystemInfo& info) {
    MEMORYSTATUSEX memory{};
    memory.dwLength = sizeof(memory);
    if (GlobalMemoryStatusEx(&memory)) {
        info.memoryValid = true;
        info.memoryTotalMb = memory.ullTotalPhys / (1024ull * 1024ull);
        info.memoryUsedMb = (memory.ullTotalPhys - memory.ullAvailPhys) / (1024ull * 1024ull);
        info.memoryPercent = memory.dwMemoryLoad;
    }
    PROCESS_MEMORY_COUNTERS_EX counters{};
    if (GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters),
                             sizeof(counters))) {
        info.allocatedValid = true;
        info.allocatedMb = counters.PrivateUsage / (1024ull * 1024ull);
        info.allocatedPercent = info.memoryTotalMb
                                    ? 100.0 * static_cast<double>(info.allocatedMb) /
                                          static_cast<double>(info.memoryTotalMb)
                                    : 0.0;
    }
}

}  // namespace

F3SystemInfo collectSystemInfo() {
    std::lock_guard lock(systemCacheMutex);
    if (!systemCache.initialised) {
        auto& info = systemCache.info;
        const std::string wine = wineVersion();
        info.runtimeLabel = wine.empty() ? "Windows" : "Wine";
        info.runtimeValue = (wine.empty() ? std::string("unknown") : wine) + " 64bit";
        info.cpuBrand =
            registryString(HKEY_LOCAL_MACHINE, L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0",
                           L"ProcessorNameString");
        SYSTEM_INFO system{};
        GetSystemInfo(&system);
        info.cpuThreads = system.dwNumberOfProcessors;
        info.display = std::to_string(GetSystemMetrics(SM_CXSCREEN)) + "x" +
                       std::to_string(GetSystemMetrics(SM_CYSCREEN));
        info.gpu = gpuDescription();
        systemCache.initialised = true;
    }
    F3SystemInfo copy = systemCache.info;
    refreshMemory(copy);
    return copy;
}

struct DebugScreen::Impl {
    std::mutex mutex;
    bedrock::sdk::Overlay overlay;
    bool installed{};
    std::atomic<bool> visible{false};
    std::atomic<bool> worldActive{true};
    std::atomic<unsigned> paints{0};
    std::shared_ptr<const F3Snapshot> snapshot;

    static void draw(void* userdata);
    void paint();
};

void DebugScreen::Impl::paint() {
    paints.fetch_add(1, std::memory_order_relaxed);
    if (!visible.load(std::memory_order_relaxed)) return;
    if (!worldActive.load(std::memory_order_relaxed)) return;
    std::shared_ptr<const F3Snapshot> current;
    {
        std::lock_guard lock(mutex);
        current = snapshot;
    }
    if (!current) return;
    const F3Layout layout = buildF3Layout(*current);
    if (layout.left.empty() && layout.right.empty()) return;

    ImDrawList* draw = ImGui::GetForegroundDrawList();
    if (!draw) return;
    ImGuiIO& io = ImGui::GetIO();
    ImFont* font = io.FontDefault ? io.FontDefault : ImGui::GetFont();
    if (!font) return;
    const float displayWidth = io.DisplaySize.x;

    const auto drawColumn = [&](const std::vector<std::string>& lines, bool rightAligned) {
        float y = kMargin;
        for (const auto& line : lines) {
            if (line.empty()) continue;
            const ImVec2 size = font->CalcTextSizeA(kFontSize, FLT_MAX, 0.0f, line.c_str());
            const float panelWidth = std::max(6.0f, size.x + 2.0f);
            const float x = rightAligned ? displayWidth - kMargin - panelWidth : kMargin;
            draw->AddRectFilled({x, y}, {x + panelWidth, y + kLineHeight}, kBackground);
            const ImVec2 textPos{x + 1.0f, y + (kLineHeight - kFontSize) * 0.5f - 1.0f};
            // Java draws white text with a shadow; mirror that instead of a
            // flat fill so the screen reads the same at any background.
            draw->AddText(font, kFontSize, {textPos.x + 1.0f, textPos.y + 1.0f}, kShadow,
                          line.c_str());
            draw->AddText(font, kFontSize, textPos, kText, line.c_str());
            y += kLineHeight;
        }
    };
    drawColumn(layout.left, false);
    drawColumn(layout.right, true);
}

void DebugScreen::Impl::draw(void* userdata) {
    auto* self = static_cast<Impl*>(userdata);
    if (self) self->paint();
}

DebugScreen::DebugScreen() : impl_(std::make_unique<Impl>()) {}
DebugScreen::~DebugScreen() = default;

bool DebugScreen::install(std::string& error) {
    if (impl_->installed) return true;
    bedrock::sdk::OverlayOptions options;
    options.toggleVirtualKey = 0;  // F3 is handled by the native key hook.
    options.initiallyVisible = true;
    options.captureInput = false;
    options.nativeCursor = false;
    if (!impl_->overlay.install(&Impl::draw, impl_.get(), options, error)) return false;
    impl_->installed = true;
    return true;
}

void DebugScreen::uninstall() noexcept {
    if (!impl_->installed) return;
    impl_->overlay.uninstall();
    impl_->installed = false;
}

void DebugScreen::setVisible(bool visible) noexcept {
    impl_->visible.store(visible, std::memory_order_relaxed);
}

bool DebugScreen::visible() const noexcept {
    return impl_->visible.load(std::memory_order_relaxed);
}

unsigned DebugScreen::paintCount() const noexcept {
    return impl_->paints.load(std::memory_order_relaxed);
}

void DebugScreen::setWorldActive(bool active) noexcept {
    impl_->worldActive.store(active, std::memory_order_relaxed);
}

void DebugScreen::toggle() noexcept {
    impl_->visible.store(!impl_->visible.load(std::memory_order_relaxed),
                         std::memory_order_relaxed);
}

void DebugScreen::publish(std::shared_ptr<const F3Snapshot> snapshot) noexcept {
    std::lock_guard lock(impl_->mutex);
    impl_->snapshot = std::move(snapshot);
}

}  // namespace diet
