// Exercises the D3D12 -> D3D11On12 overlay path in a controlled swap-chain.
// Runs on Windows CI, preferably with WARP if no physical GPU is available.
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_5.h>
#include <wrl/client.h>
#include <imgui.h>
#include <bedrock/gui_overlay.hpp>
#include <atomic>
#include <iostream>
#include <string>

using Microsoft::WRL::ComPtr;
static std::atomic<int> callbackFrames{0};
static void callback(void*) {
    ImGui::Begin("DX12 in-game overlay test");
    ImGui::TextUnformatted("ImGui rendered through D3D11On12!");
    ImGui::End();
    callbackFrames.fetch_add(1);
}
int main() {
    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc=DefWindowProcW;
    wc.hInstance=GetModuleHandleW(nullptr);
    wc.lpszClassName=L"BedrockOverlayDX12Smoke";
    if (!RegisterClassExW(&wc)) return 1;
    HWND hwnd=CreateWindowW(wc.lpszClassName,L"DX12 smoke",WS_OVERLAPPEDWINDOW,
                             0,0,640,480,nullptr,nullptr,wc.hInstance,nullptr);
    if (!hwnd) return 2;

    ComPtr<IDXGIFactory4> factory;
    HRESULT hr=CreateDXGIFactory1(IID_PPV_ARGS(&factory));
    if (FAILED(hr)) return 3;
    ComPtr<ID3D12Device> device;
    hr=D3D12CreateDevice(nullptr,D3D_FEATURE_LEVEL_11_0,IID_PPV_ARGS(&device));
    if (FAILED(hr)) {
        ComPtr<IDXGIAdapter> warp;
        if (SUCCEEDED(factory->EnumWarpAdapter(IID_PPV_ARGS(&warp))))
            hr=D3D12CreateDevice(warp.Get(),D3D_FEATURE_LEVEL_11_0,IID_PPV_ARGS(&device));
    }
    if (FAILED(hr)) {
        std::cerr<<"D3D12 device unavailable on this runner; cannot verify overlay\n";
        DestroyWindow(hwnd);
        return 12;
    }
    D3D12_COMMAND_QUEUE_DESC queueDesc{};
    queueDesc.Type=D3D12_COMMAND_LIST_TYPE_DIRECT;
    ComPtr<ID3D12CommandQueue> queue;
    if (FAILED(device->CreateCommandQueue(&queueDesc,IID_PPV_ARGS(&queue)))) return 4;
    DXGI_SWAP_CHAIN_DESC1 desc{};
    desc.Width=640;
    desc.Height=480;
    desc.BufferCount=2;
    desc.BufferUsage=DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.Format=DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count=1;
    desc.SwapEffect=DXGI_SWAP_EFFECT_FLIP_DISCARD;
    ComPtr<IDXGISwapChain1> chain1;
    if (FAILED(factory->CreateSwapChainForHwnd(queue.Get(),hwnd,&desc,nullptr,nullptr,&chain1)))
        return 5;
    ComPtr<IDXGISwapChain3> chain;
    if (FAILED(chain1.As(&chain))) return 6;

    std::string error;
    bedrock::gui::OverlayOptions options{};
    options.requireMinecraftProcess=false;
    if (!bedrock::gui::Overlay::install(&callback,nullptr,error,options)) {
        std::cerr<<"Overlay DX12 install failed: "<<error<<"\n";
        return 7;
    }

    // Title-screen regression: a static main menu may present frames without
    // submitting NEW command lists. Queue::Signal still identifies the DIRECT
    // queue, so the overlay must render without entering a world/pause menu.
    ComPtr<ID3D12Fence> fence;
    if (FAILED(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&fence))))
        return 8;
    if (FAILED(queue->Signal(fence.Get(),1))) return 9;

    for (int i=0;i<4;++i) chain->Present(0,0);
    const bool invoked=callbackFrames.load()>=1;
    const int before=callbackFrames.load();
    bedrock::gui::Overlay::show(false);
    chain->Present(0,0);
    const bool hidden=callbackFrames.load()==before;
    bedrock::gui::Overlay::show(true);
    chain->Present(0,0);
    const bool restored=callbackFrames.load()>before;

    std::cout<<"DX12 frames="<<callbackFrames.load()<<" invoked="<<invoked
             <<" hidden="<<hidden<<" restored="<<restored<<"\n";
    bedrock::gui::Overlay::shutdown();
    chain.Reset();
    chain1.Reset();
    queue.Reset();
    device.Reset();
    DestroyWindow(hwnd);
    UnregisterClassW(wc.lpszClassName,wc.hInstance);
    return (invoked && hidden && restored) ? 0 : 11;
}
#endif
