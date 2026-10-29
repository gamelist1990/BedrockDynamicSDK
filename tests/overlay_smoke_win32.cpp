// Uses a REAL Direct3D11 swap-chain to validate the overlay Present adapter.
// This test runs without Minecraft and with a software WARP device when needed.
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <wrl/client.h>
#include <imgui.h>
#include <bedrock/render/gui_overlay.hpp>
#include <cassert>
#include <atomic>
#include <string>
#include <iostream>

static std::atomic<int> frames{0};
static void draw(void*) {
    ImGui::Begin("Internal overlay smoke");
    ImGui::TextUnformatted("Rendering inside a real DXGI Present!");
    ImGui::End();
    ++frames;
}

int main() {
    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc=DefWindowProcW;
    wc.hInstance=GetModuleHandleW(nullptr);
    wc.lpszClassName=L"BedrockGuiOverlaySmoke";
    assert(RegisterClassExW(&wc) != 0);
    HWND hwnd = CreateWindowW(wc.lpszClassName,L"DXGI smoke",
        WS_OVERLAPPEDWINDOW,0,0,640,480,nullptr,nullptr,wc.hInstance,nullptr);
    assert(hwnd);

    bedrock::gui::OverlayOptions opt{};
    opt.requireMinecraftProcess=false; // Controlled test executable.
    std::string error;
    if (!bedrock::gui::Overlay::install(&draw,nullptr,error,opt)) {
        std::cerr<<"Overlay install failed: "<<error<<"\n";
        DestroyWindow(hwnd);
        return 1;
    }
    DXGI_SWAP_CHAIN_DESC desc{};
    desc.BufferCount=2;
    desc.BufferDesc.Format=DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.BufferUsage=DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.OutputWindow=hwnd;
    desc.SampleDesc.Count=1;
    desc.Windowed=TRUE;
    desc.SwapEffect=DXGI_SWAP_EFFECT_DISCARD;
    Microsoft::WRL::ComPtr<ID3D11Device> device;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context;
    Microsoft::WRL::ComPtr<IDXGISwapChain> chain;
    D3D_FEATURE_LEVEL chosen{};
    D3D_FEATURE_LEVEL supported[]={D3D_FEATURE_LEVEL_11_0};
    HRESULT hr = D3D11CreateDeviceAndSwapChain(nullptr,D3D_DRIVER_TYPE_HARDWARE,
        nullptr,0,supported,1,D3D11_SDK_VERSION,&desc,
        &chain,&device,&chosen,&context);
    if (FAILED(hr)) {
        hr = D3D11CreateDeviceAndSwapChain(nullptr,D3D_DRIVER_TYPE_WARP,
            nullptr,0,supported,1,D3D11_SDK_VERSION,&desc,
            &chain,&device,&chosen,&context);
    }
    if (FAILED(hr)) {
        std::cerr<<"Cannot create D3D11 smoke swap-chain\n";
        bedrock::gui::Overlay::shutdown();
        DestroyWindow(hwnd);
        return 2;
    }
    for (int i=0;i<3;i++) chain->Present(0,0);
    bool drew = frames.load() >= 1;
    bedrock::gui::Overlay::show(false);
    const int before = frames.load();
    chain->Present(0,0);
    const bool hiddenWorked = frames.load() == before;
    bedrock::gui::Overlay::show(true);
    chain->Present(0,0);
    const bool resumeWorked = frames.load() > before;
    bedrock::gui::Overlay::shutdown();
    chain.Reset();
    context.Reset();
    device.Reset();
    DestroyWindow(hwnd);
    UnregisterClassW(wc.lpszClassName,wc.hInstance);
    std::cout<<"Overlay Present frames="<<frames.load()
             <<" hidden="<<hiddenWorked<<" resume="<<resumeWorked<<"\n";
    return (drew && hiddenWorked && resumeWorked) ? 0 : 3;
}
#endif
