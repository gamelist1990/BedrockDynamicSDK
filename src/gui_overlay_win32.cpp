#ifdef _WIN32
// DXGI-based in-game ImGui adapter for Windows x64 Minecraft.
// This file is original integration code; no BetterRenderDragon source copied.
// No reliance on undocumented Minecraft object layouts or game function ABIs.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d11.h>
#include <d3d11on12.h>
#include <d3d12.h>
#include <dxgi1_5.h>
#include <wrl/client.h>
#include <MinHook.h>
#include <imgui.h>
#include <backends/imgui_impl_dx11.h>
#include <backends/imgui_impl_win32.h>
#include "bedrock/gui.hpp"
#include "bedrock/gui_overlay.hpp"
#include <atomic>
#include <array>
#include <cstdint>
#include <cwctype>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(
    HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam);

namespace bedrock::gui {
namespace {
using Microsoft::WRL::ComPtr;
using PresentFn = HRESULT (STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
using ResizeFn = HRESULT (STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);
using Resize1Fn = HRESULT (STDMETHODCALLTYPE*)(
    IDXGISwapChain3*, UINT, UINT, UINT, DXGI_FORMAT, UINT,
    const UINT*, IUnknown* const*);
using QueueFn = void (STDMETHODCALLTYPE*)(ID3D12CommandQueue*, UINT, ID3D12CommandList* const*);

std::recursive_mutex mutex;
std::atomic<bool> installed{false};
std::atomic<bool> enabled{false};
std::atomic<unsigned> toggleKey{0x79};
std::atomic<bool> captureInput{true};
Overlay::RenderCallback renderCallback = nullptr;
void* renderUserdata = nullptr;
bool minhookOwned = false;
bool backendInitialized = false;
bool usingDx12 = false;
HWND gameWindow = nullptr;
WNDPROC oldWndProc = nullptr;
ComPtr<IDXGISwapChain> activeChain;
ComPtr<ID3D11Device> device11;
ComPtr<ID3D11DeviceContext> context11;
ComPtr<ID3D11On12Device> device11on12;
ComPtr<ID3D12CommandQueue> capturedQueue;
ComPtr<ID3D11RenderTargetView> renderTarget;
struct WrappedBuffer {
    ComPtr<ID3D11Resource> dx11;
    ComPtr<ID3D12Resource> dx12;
};
std::vector<WrappedBuffer> wrapped;
ImGuiContext* contextImGui = nullptr;
std::vector<void*> targets;

PresentFn original11 = nullptr;
PresentFn original12 = nullptr;
ResizeFn resize11 = nullptr;
ResizeFn resize12 = nullptr;
Resize1Fn resize12_1 = nullptr;
QueueFn execute12 = nullptr;

void trace(const char* line) { OutputDebugStringA(line); }

bool minecraftProcess() {
    wchar_t path[32768]{};
    const DWORD n = GetModuleFileNameW(nullptr, path, static_cast<DWORD>(std::size(path)));
    if (!n || n >= std::size(path)) return false;
    const std::wstring_view value(path, n);
    const auto slash = value.find_last_of(L"\\/");
    const auto filename = value.substr(slash == std::wstring_view::npos ? 0 : slash + 1);
    std::wstring lower(filename);
    for (auto& c : lower) c = static_cast<wchar_t>(std::towlower(c));
    return lower == L"minecraft.windows.exe";
}

template<class T>
T method(void* object, std::size_t index) noexcept {
    return reinterpret_cast<T>((*reinterpret_cast<void***>(object))[index]);
}

bool makeDx11Dummy(HWND hwnd, void** present, void** resize) {
    DXGI_SWAP_CHAIN_DESC desc{};
    desc.BufferCount = 2;
    desc.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.OutputWindow = hwnd;
    desc.SampleDesc.Count = 1;
    desc.Windowed = TRUE;
    desc.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<IDXGISwapChain> chain;
    D3D_FEATURE_LEVEL feature{};
    const D3D_FEATURE_LEVEL requested[] = {D3D_FEATURE_LEVEL_11_0};
    const HRESULT result = D3D11CreateDeviceAndSwapChain(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
        requested, 1, D3D11_SDK_VERSION, &desc,
        &chain, &device, &feature, &context);
    if (FAILED(result)) return false;
    *present = method<void*>(chain.Get(), 8); // IDXGISwapChain::Present
    *resize = method<void*>(chain.Get(), 13); // IDXGISwapChain::ResizeBuffers
    return *present && *resize;
}
bool makeDx12Dummy(HWND hwnd, void** present, void** resize, void** resize1, void** execute) {
    ComPtr<ID3D12Device> dev;
    if (FAILED(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0,
                                 IID_PPV_ARGS(&dev)))) return false;
    D3D12_COMMAND_QUEUE_DESC queueDesc{};
    queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ComPtr<ID3D12CommandQueue> queue;
    if (FAILED(dev->CreateCommandQueue(&queueDesc,IID_PPV_ARGS(&queue)))) return false;
    ComPtr<IDXGIFactory2> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return false;
    DXGI_SWAP_CHAIN_DESC1 desc{};
    desc.Width = 320;
    desc.Height = 240;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.BufferCount = 2;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.SampleDesc.Count = 1;
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    ComPtr<IDXGISwapChain1> chain1;
    if (FAILED(factory->CreateSwapChainForHwnd(queue.Get(), hwnd, &desc,
                                                nullptr, nullptr, &chain1))) return false;
    ComPtr<IDXGISwapChain3> chain3;
    if (FAILED(chain1.As(&chain3))) return false;
    *present = method<void*>(chain3.Get(), 8);
    *resize = method<void*>(chain3.Get(), 13);
    *resize1 = method<void*>(chain3.Get(), 39);  // IDXGISwapChain3::ResizeBuffers1
    *execute = method<void*>(queue.Get(), 10);  // ID3D12CommandQueue::ExecuteCommandLists
    return *present && *resize && *resize1 && *execute;
}

void cleanupRenderer() {
    if (contextImGui) {
        ImGui::SetCurrentContext(contextImGui);
        if (backendInitialized) {
            ImGui_ImplDX11_Shutdown();
            ImGui_ImplWin32_Shutdown();
        }
        ImGui::DestroyContext(contextImGui);
        contextImGui = nullptr;
    }
    backendInitialized = false;
    renderTarget.Reset();
    wrapped.clear();
    context11.Reset();
    device11on12.Reset();
    device11.Reset();
    activeChain.Reset();
    usingDx12 = false;
}
LRESULT CALLBACK overlayWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    std::lock_guard<std::recursive_mutex> lock(mutex);
    if (installed.load()) {
        const auto key = toggleKey.load();
        if (key && msg == WM_KEYDOWN && wp == key && !(lp & (1LL << 30))) {
            enabled.store(!enabled.load());
            return 0;
        }
        if (enabled.load() && captureInput.load() && contextImGui) {
            ImGui::SetCurrentContext(contextImGui);
            const auto handled = ImGui_ImplWin32_WndProcHandler(hwnd, msg, wp, lp);
            const auto& io = ImGui::GetIO();
            if (handled || ((msg >= WM_MOUSEFIRST && msg <= WM_MOUSELAST) && io.WantCaptureMouse) ||
                ((msg >= WM_KEYFIRST && msg <= WM_KEYLAST) && io.WantCaptureKeyboard))
                return 1;
        }
    }
    return oldWndProc ? CallWindowProcW(oldWndProc, hwnd, msg, wp, lp)
                      : DefWindowProcW(hwnd, msg, wp, lp);
}
void removeWndProc() {
    if (!gameWindow || !oldWndProc) return;
    if (IsWindow(gameWindow) &&
        reinterpret_cast<WNDPROC>(GetWindowLongPtrW(gameWindow, GWLP_WNDPROC)) == overlayWndProc)
        SetWindowLongPtrW(gameWindow, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(oldWndProc));
    gameWindow = nullptr;
    oldWndProc = nullptr;
}
bool createView(IDXGISwapChain* chain) {
    ComPtr<ID3D11Texture2D> texture;
    if (FAILED(chain->GetBuffer(0, IID_PPV_ARGS(&texture)))) return false;
    return SUCCEEDED(device11->CreateRenderTargetView(texture.Get(), nullptr, &renderTarget));
}
bool initRenderer(IDXGISwapChain* chain) {
    DXGI_SWAP_CHAIN_DESC desc{};
    if (FAILED(chain->GetDesc(&desc)) || !IsWindow(desc.OutputWindow))
        return false;
    usingDx12 = false;
    ComPtr<ID3D12Device> device12;
    if (SUCCEEDED(chain->GetDevice(IID_PPV_ARGS(&device12)))) {
        if (!capturedQueue) return false; // Wait for real ExecuteCommandLists
        ComPtr<ID3D12Device> queueDevice;
        if (FAILED(capturedQueue->GetDevice(IID_PPV_ARGS(&queueDevice))) ||
            queueDevice.Get() != device12.Get())
            return false; // Do not mix unrelated GPU queues
        UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
        IUnknown* queues[] = {capturedQueue.Get()};
        if (FAILED(D3D11On12CreateDevice(device12.Get(), flags, nullptr, 0,
                                         queues, 1, 0, &device11,
                                         &context11, nullptr))) return false;
        if (FAILED(device11.As(&device11on12))) return false;
        ComPtr<IDXGISwapChain3> chain3;
        if (FAILED(chain->QueryInterface(IID_PPV_ARGS(&chain3)))) return false;
        DXGI_SWAP_CHAIN_DESC desc12{};
        if (FAILED(chain3->GetDesc(&desc12)) || desc12.BufferCount == 0 ||
            desc12.BufferCount > 8) return false;
        wrapped.resize(desc12.BufferCount);
        for (UINT i = 0; i < desc12.BufferCount; ++i) {
            if (FAILED(chain3->GetBuffer(i,IID_PPV_ARGS(&wrapped[i].dx12)))) return false;
            D3D11_RESOURCE_FLAGS bind{};
            bind.BindFlags = D3D11_BIND_RENDER_TARGET;
            if (FAILED(device11on12->CreateWrappedResource(
                    wrapped[i].dx12.Get(), &bind,
                    D3D12_RESOURCE_STATE_RENDER_TARGET,
                    D3D12_RESOURCE_STATE_PRESENT,
                    IID_PPV_ARGS(&wrapped[i].dx11)))) return false;
        }
        usingDx12 = true;
    } else if (FAILED(chain->GetDevice(IID_PPV_ARGS(&device11)))) {
        return false;
    }
    if (!usingDx12 && !createView(chain)) return false;
    device11->GetImmediateContext(&context11);
    IMGUI_CHECKVERSION();
    contextImGui = ImGui::CreateContext();
    ImGui::SetCurrentContext(contextImGui);
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_DockingEnable;
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;
    io.MouseDrawCursor = true; // visible when Minecraft normally hides cursor
    ImGui::GetIO().Fonts->AddFontDefault();
    Theme::dark();
    if (!ImGui_ImplWin32_Init(desc.OutputWindow)) return false;
    if (!ImGui_ImplDX11_Init(device11.Get(), context11.Get())) {
        ImGui_ImplWin32_Shutdown();
        return false;
    }
    backendInitialized = true;
    activeChain = chain;
    gameWindow = desc.OutputWindow;
    // Subclass only after the ImGui backend initialized.
    SetLastError(0);
    const LONG_PTR previous = SetWindowLongPtrW(gameWindow, GWLP_WNDPROC,
                                               reinterpret_cast<LONG_PTR>(overlayWndProc));
    if (previous) oldWndProc = reinterpret_cast<WNDPROC>(previous);
    return true;
}
void drawImGui(IDXGISwapChain* chain) {
    if (!enabled.load() || !renderCallback) return;
    ImGui::SetCurrentContext(contextImGui);
    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();
    try {
        renderCallback(renderUserdata);
    } catch (...) {
        trace("BedrockGuiOverlay: render callback threw; hiding UI\n");
        enabled.store(false);
    }
    ImGui::Render();
    if (usingDx12) {
        ComPtr<IDXGISwapChain3> chain3;
        if (FAILED(chain->QueryInterface(IID_PPV_ARGS(&chain3)))) return;
        const UINT index = chain3->GetCurrentBackBufferIndex();
        if (index >= wrapped.size()) return;
        ID3D11Resource* texture = wrapped[index].dx11.Get();
        device11on12->AcquireWrappedResources(&texture,1);
        ComPtr<ID3D11RenderTargetView> view;
        if (SUCCEEDED(device11->CreateRenderTargetView(texture,nullptr,&view))) {
            ID3D11RenderTargetView* output = view.Get();
            context11->OMSetRenderTargets(1, &output, nullptr);
            ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        }
        device11on12->ReleaseWrappedResources(&texture,1);
        context11->Flush();
    } else {
        ID3D11RenderTargetView* view = renderTarget.Get();
        context11->OMSetRenderTargets(1,&view,nullptr);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    }
}
PresentFn selectPresent(IDXGISwapChain* chain) {
    ComPtr<ID3D12Device> device;
    const bool dx12 = SUCCEEDED(chain->GetDevice(IID_PPV_ARGS(&device)));
    return (dx12 && original12) ? original12 : original11;
}
ResizeFn selectResize(IDXGISwapChain* chain) {
    ComPtr<ID3D12Device> device;
    const bool dx12 = SUCCEEDED(chain->GetDevice(IID_PPV_ARGS(&device)));
    return (dx12 && resize12) ? resize12 : resize11;
}
HRESULT STDMETHODCALLTYPE onPresent(IDXGISwapChain* chain, UINT sync, UINT flags) {
    const auto original = selectPresent(chain);
    if (installed.load()) {
        std::lock_guard<std::recursive_mutex> lock(mutex);
        // Initialize even while hidden, so WndProc sees the toggle shortcut.
        if (installed.load()) {
            if (!backendInitialized || activeChain.Get() != chain) {
                removeWndProc();
                cleanupRenderer();
                if (!initRenderer(chain)) {
                    removeWndProc();
                    cleanupRenderer();
                }
            }
            if (enabled.load() && backendInitialized)
                drawImGui(chain);
        }
    }
    return original ? original(chain,sync,flags) : DXGI_ERROR_INVALID_CALL;
}
HRESULT STDMETHODCALLTYPE onResize(IDXGISwapChain* chain, UINT buffers, UINT w,
                                    UINT h, DXGI_FORMAT fmt, UINT flags) {
    auto original = selectResize(chain);
    {
        std::lock_guard<std::recursive_mutex> lock(mutex);
        if (activeChain.Get() == chain) {
            removeWndProc();
            cleanupRenderer(); // free buffer references BEFORE ResizeBuffers
        }
    }
    return original ? original(chain,buffers,w,h,fmt,flags) : DXGI_ERROR_INVALID_CALL;
}
HRESULT STDMETHODCALLTYPE onResize1(IDXGISwapChain3* chain, UINT buffers, UINT w,
                                     UINT h, DXGI_FORMAT fmt, UINT flags,
                                     const UINT* nodes, IUnknown* const* queues) {
    {
        std::lock_guard<std::recursive_mutex> lock(mutex);
        if (activeChain.Get() == chain) {
            removeWndProc();
            cleanupRenderer();
        }
    }
    return resize12_1 ? resize12_1(chain,buffers,w,h,fmt,flags,nodes,queues)
                      : DXGI_ERROR_INVALID_CALL;
}
void STDMETHODCALLTYPE onExecute(ID3D12CommandQueue* queue, UINT count, ID3D12CommandList* const* lists) {
    if (installed.load() && queue) {
        std::lock_guard<std::recursive_mutex> lock(mutex);
        if (!capturedQueue && queue->GetDesc().Type == D3D12_COMMAND_LIST_TYPE_DIRECT)
            capturedQueue = queue;
    }
    if (execute12) execute12(queue,count,lists);
}
bool hookMethod(void* target, void* detour, void** original) {
    if (!target) return false;
    const auto status = MH_CreateHook(target,detour,original);
    if (status != MH_OK) return false;
    targets.push_back(target);
    return true;
}
struct DummyWindow {
    HWND hwnd{};
    HINSTANCE instance{};
    ATOM klass{};
    DummyWindow() {
        instance = GetModuleHandleW(nullptr);
        WNDCLASSEXW wc{sizeof(wc)};
        wc.lpfnWndProc = DefWindowProcW;
        wc.hInstance = instance;
        wc.lpszClassName = L"BedrockDynamicSDKInvisibleDXGIProbe";
        klass = RegisterClassExW(&wc);
        hwnd = CreateWindowExW(0,wc.lpszClassName,L"",WS_OVERLAPPED,
                              0,0,320,240,nullptr,nullptr,instance,nullptr);
    }
    ~DummyWindow() {
        if (hwnd) DestroyWindow(hwnd);
        if (klass) UnregisterClassW(L"BedrockDynamicSDKInvisibleDXGIProbe",instance);
    }
};
} // namespace

bool Overlay::install(RenderCallback render, void* userdata,
                      std::string& error, OverlayOptions options) {
    std::lock_guard<std::recursive_mutex> lock(mutex);
    if (installed.load()) { error="Already installed"; return false; }
    if (!render) { error="Missing draw callback"; return false; }
    if (options.requireMinecraftProcess && !minecraftProcess()) {
        error="This in-game adapter may only run in Minecraft.Windows.exe";
        return false;
    }
    DummyWindow dummy;
    if (!dummy.hwnd) { error="Cannot create hidden DXGI probe window"; return false; }

    void* d11Present=nullptr; void* d11Resize=nullptr;
    void* d12Present=nullptr; void* d12Resize=nullptr;
    void* d12Resize1=nullptr; void* d12Execute=nullptr;
    const bool supported11 = makeDx11Dummy(dummy.hwnd,&d11Present,&d11Resize);
    const bool supported12 = makeDx12Dummy(dummy.hwnd,&d12Present,&d12Resize,&d12Resize1,&d12Execute);
    if (!supported11 && !supported12) {
        error="Could not discover any DXGI swap-chain implementation";
        return false;
    }
    const MH_STATUS result = MH_Initialize();
    if (result != MH_OK && result != MH_ERROR_ALREADY_INITIALIZED) {
        error="MH_Initialize failed";
        return false;
    }
    minhookOwned = result == MH_OK;
    bool okay = true;
    if (supported11)
        okay = hookMethod(d11Present,(void*)&onPresent,(void**)&original11) &&
               hookMethod(d11Resize,(void*)&onResize,(void**)&resize11);
    if (okay && supported12) {
        if (d12Present == d11Present) original12=original11;
        else okay = hookMethod(d12Present,(void*)&onPresent,(void**)&original12);
        if (okay) {
            if (d12Resize == d11Resize) resize12=resize11;
            else okay = hookMethod(d12Resize,(void*)&onResize,(void**)&resize12);
        }
        if (okay) okay = hookMethod(d12Resize1,(void*)&onResize1,(void**)&resize12_1) &&
                         hookMethod(d12Execute,(void*)&onExecute,(void**)&execute12);
    }
    if (okay) for (void* target : targets) {
        if (MH_QueueEnableHook(target) != MH_OK) { okay = false; break; }
    }
    // Store callback before enabling hooks to avoid a zero-function race.
    renderCallback=render;
    renderUserdata=userdata;
    enabled.store(options.initiallyVisible);
    toggleKey.store(options.toggleVirtualKey);
    captureInput.store(options.captureInput);
    installed.store(okay);
    if (okay && MH_ApplyQueued() == MH_OK) {
        error.clear();
        trace(supported12 ? "BedrockGuiOverlay: DX12/DX11 Present hooks enabled\n"
                          : "BedrockGuiOverlay: DX11 Present hook enabled\n");
        return true;
    }
    installed.store(false);
    enabled.store(false);
    // Undo any hooks queued by this module. Never uninitialize hooks owned by another module.
    for (void* target : targets) { MH_DisableHook(target); MH_RemoveHook(target); }
    targets.clear();
    if (minhookOwned) MH_Uninitialize();
    minhookOwned=false;
    renderCallback=nullptr;
    renderUserdata=nullptr;
    error="Could not install/enable all DXGI hooks";
    return false;
}
void Overlay::show(bool value) noexcept { enabled.store(value); }
bool Overlay::visible() noexcept { return enabled.load(); }
bool Overlay::running() noexcept { return installed.load(); }
void Overlay::shutdown() noexcept {
    std::lock_guard<std::recursive_mutex> lock(mutex);
    if (!installed.exchange(false)) return;
    enabled.store(false);
    // Keep the callback & code resident until MinHook no longer redirects execution.
    for (void* target : targets) MH_DisableHook(target);
    for (void* target : targets) MH_RemoveHook(target);
    targets.clear();
    removeWndProc();
    cleanupRenderer();
    capturedQueue.Reset();
    if (minhookOwned) MH_Uninitialize();
    minhookOwned=false;
    renderCallback=nullptr;
    renderUserdata=nullptr;
}
} // namespace bedrock::gui
#endif
