#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <dwmapi.h>
#include <imgui.h>
#include <backends/imgui_impl_win32.h>
#include <backends/imgui_impl_dx11.h>
#include "bedrock/gui_host.hpp"
#include "bedrock/gui.hpp"
#include <algorithm>
#include <exception>
#include <string>

namespace bedrock::gui {
namespace {
struct WindowState {
    UINT width = 0;
    UINT height = 0;
    bool closed = false;
};

LRESULT CALLBACK guiWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (ImGui::GetCurrentContext() &&
        ImGui_ImplWin32_WndProcHandler(hwnd, msg, wp, lp))
        return TRUE;
    auto* state = reinterpret_cast<WindowState*>(
        GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (msg) {
        case WM_NCCREATE: {
            const auto* creation = reinterpret_cast<LPCREATESTRUCTW>(lp);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                reinterpret_cast<LONG_PTR>(creation->lpCreateParams));
            return TRUE;
        }
        case WM_SIZE:
            if (state && wp != SIZE_MINIMIZED) {
                state->width = LOWORD(lp);
                state->height = HIWORD(lp);
            }
            return 0;
        case WM_SYSCOMMAND:
            if ((wp & 0xfff0) == SC_KEYMENU) return 0;
            break;
        case WM_CLOSE:
            DestroyWindow(hwnd);
            return 0;
        case WM_DESTROY:
            if (state) state->closed = true;
            return 0; // Never PostQuitMessage: this can run inside a game.
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

struct Dx11 {
    ID3D11Device* device{};
    ID3D11DeviceContext* context{};
    IDXGISwapChain* chain{};
    ID3D11RenderTargetView* target{};
    ~Dx11() { reset(); }
    void resetRenderTarget() noexcept {
        if (target) { target->Release(); target = nullptr; }
    }
    bool createRenderTarget() {
        resetRenderTarget();
        ID3D11Texture2D* backbuffer = nullptr;
        if (FAILED(chain->GetBuffer(0, IID_PPV_ARGS(&backbuffer)))) return false;
        const HRESULT result = device->CreateRenderTargetView(backbuffer, nullptr, &target);
        backbuffer->Release();
        return SUCCEEDED(result);
    }
    bool initialize(HWND hwnd) {
        DXGI_SWAP_CHAIN_DESC desc{};
        desc.BufferCount = 2;
        desc.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        desc.OutputWindow = hwnd;
        desc.SampleDesc.Count = 1;
        desc.Windowed = TRUE;
        desc.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
        const D3D_FEATURE_LEVEL levels[] = {
            D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0
        };
        D3D_FEATURE_LEVEL selected{};
        for (D3D_DRIVER_TYPE type : {D3D_DRIVER_TYPE_HARDWARE, D3D_DRIVER_TYPE_WARP}) {
            if (SUCCEEDED(D3D11CreateDeviceAndSwapChain(
                    nullptr, type, nullptr, 0, levels, 2, D3D11_SDK_VERSION,
                    &desc, &chain, &device, &selected, &context)))
                return createRenderTarget();
            reset();
        }
        return false;
    }
    void reset() noexcept {
        resetRenderTarget();
        if (chain) { chain->Release(); chain = nullptr; }
        if (context) { context->Release(); context = nullptr; }
        if (device) { device->Release(); device = nullptr; }
    }
    bool resize(UINT w, UINT h) {
        resetRenderTarget();
        if (FAILED(chain->ResizeBuffers(0, w, h, DXGI_FORMAT_UNKNOWN, 0)))
            return false;
        return createRenderTarget();
    }
};

} // namespace

bool GuiHost::run(const HostOptions& options, FrameHandler render,
                  std::string& error) {
    if (!render || options.width < 250 || options.height < 180 ||
        options.width > 8000 || options.height > 8000) {
        error = "Invalid GUI callback or initial window size";
        return false;
    }

    // Every GuiHost owns an independent context but ImGui assumes all calls to
    // this context occur on its owning UI thread.
    WindowState state{};
    HINSTANCE instance = GetModuleHandleW(nullptr);
    constexpr wchar_t klass[] = L"BedrockDynamicSDKGuiHost";
    WNDCLASSEXW wc{sizeof(wc)};
    wc.style = CS_CLASSDC;
    wc.lpfnWndProc = guiWndProc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = klass;
    const ATOM atom = RegisterClassExW(&wc);
    if (!atom && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        error = "RegisterClassExW failed";
        return false;
    }
    const DWORD exStyle = WS_EX_APPWINDOW | (options.alwaysOnTop ? WS_EX_TOPMOST : 0);
    RECT dimensions{0, 0, options.width, options.height};
    AdjustWindowRectEx(&dimensions, WS_OVERLAPPEDWINDOW, FALSE, exStyle);
    HWND hwnd = CreateWindowExW(
        exStyle, klass, options.title.c_str(), WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT,
        dimensions.right - dimensions.left,
        dimensions.bottom - dimensions.top,
        nullptr, nullptr, instance, &state);
    if (!hwnd) {
        error = "CreateWindowExW failed";
        if (atom) UnregisterClassW(klass, instance);
        return false;
    }
    const BOOL dark = TRUE;
    DwmSetWindowAttribute(hwnd, 20, &dark, sizeof(dark));
    Dx11 dx;
    if (!dx.initialize(hwnd)) {
        error = "Direct3D11 could not create a device/swap chain";
        DestroyWindow(hwnd);
        if (atom) UnregisterClassW(klass, instance);
        return false;
    }

    IMGUI_CHECKVERSION();
    ImGuiContext* context = ImGui::CreateContext();
    ImGui::SetCurrentContext(context);
    auto& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    io.IniFilename = nullptr; // no config in Minecraft user directory
    io.LogFilename = nullptr;
    io.Fonts->AddFontDefault();
    Theme::dark();
    const bool platformOk = ImGui_ImplWin32_Init(hwnd);
    const bool backendOk = platformOk && ImGui_ImplDX11_Init(dx.device, dx.context);
    if (!backendOk) {
        error = "Dear ImGui backend initialization failed";
        if (platformOk) ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext(context);
        DestroyWindow(hwnd);
        if (atom) UnregisterClassW(klass, instance);
        return false;
    }

    ShowWindow(hwnd, SW_SHOWDEFAULT);
    UpdateWindow(hwnd);
    bool okay = true;
    UINT actualWidth = options.width, actualHeight = options.height;
    while (!state.closed) {
        MSG message{};
        while (PeekMessageW(&message, hwnd, 0, 0, PM_REMOVE)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        if (state.closed) break;
        if (IsIconic(hwnd)) { Sleep(25); continue; }
        if (state.width && state.height &&
            (state.width != actualWidth || state.height != actualHeight)) {
            actualWidth = state.width;
            actualHeight = state.height;
            if (!dx.resize(actualWidth, actualHeight)) {
                error = "Failed to resize DirectX11 backbuffer";
                okay = false; break;
            }
        }
        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();
        try {
            render();
        } catch (const std::exception& ex) {
            error = std::string("GUI callback exception: ") + ex.what();
            okay = false; break;
        } catch (...) {
            error = "Unknown exception in GUI callback";
            okay = false; break;
        }
        ImGui::Render();
        const float clear[] = {0.035f, 0.047f, 0.073f, 1.0f};
        dx.context->OMSetRenderTargets(1, &dx.target, nullptr);
        dx.context->ClearRenderTargetView(dx.target, clear);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        const HRESULT present = dx.chain->Present(options.vsync ? 1 : 0, 0);
        if (FAILED(present)) {
            error = "DirectX11 Present failed or display device was lost";
            okay = false; break;
        }
    }

    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext(context);
    dx.reset();
    if (!state.closed) DestroyWindow(hwnd);
    if (atom) UnregisterClassW(klass, instance);
    if (okay) error.clear();
    return okay;
}
} // namespace bedrock::gui
#endif
