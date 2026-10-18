#include "tests/flow/flow_test_support.h"

#include <d3d10_1.h>
#include <d3d11.h>
#include <dxgi.h>
#include <wrl/client.h>

// CE's cpu_prerender_limit on a real WARP D3D10 swapchain, through the entry point both DXGI
// Present detours call (hook/d3d11/dx11_hook_prerender.cpp). Limits 1-6 set the D3D10 device's
// DXGI frame latency. Until 2026-10-18 the detours admitted D3D11 swapchains only, and the entry
// point asked for ID3D11Device first, which a D3D10 swapchain on Windows 10+ answers as well
// (D3D10-on-D3D11): the D3D10 path never ran.
namespace {
using Microsoft::WRL::ComPtr;

struct D3D10Game {
    D3D10Game() {
        window = CreateWindowExW(0, L"STATIC", L"ce_flow_d3d10_prerender", WS_POPUP, 0, 0, 64, 64, nullptr, nullptr,
                                 GetModuleHandleW(nullptr), nullptr);
        // The system runtime directly: the flow process's own imports may resolve to CE's hooks.
        // d3d10.dll has no WARP driver (E_FAIL); d3d10_1.dll's device is a D3D10 device as well.
        const HMODULE runtime = LoadLibraryExW(L"d3d10_1.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        const auto create = runtime ? reinterpret_cast<decltype(&D3D10CreateDeviceAndSwapChain1)>(
                                          GetProcAddress(runtime, "D3D10CreateDeviceAndSwapChain1"))
                                    : nullptr;
        if (!window || !create) {
            return;
        }
        DXGI_SWAP_CHAIN_DESC desc = {};
        desc.BufferDesc.Width = 64;
        desc.BufferDesc.Height = 64;
        desc.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        desc.BufferCount = 1;
        desc.OutputWindow = window;
        desc.Windowed = TRUE;
        desc.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;  // blt model: no frame-latency waitable object
        hr = create(nullptr, D3D10_DRIVER_TYPE_WARP, nullptr, 0, D3D10_FEATURE_LEVEL_10_0, D3D10_1_SDK_VERSION, &desc,
                    &swapchain, &device);
        if (SUCCEEDED(hr)) {
            hr = device.As(&dxgiDevice);
        }
        if (FAILED(hr)) {
            swapchain.Reset();
            device.Reset();
        }
    }
    ~D3D10Game() {
        dxgiDevice.Reset();
        swapchain.Reset();
        device.Reset();
        if (window) {
            DestroyWindow(window);
        }
    }
    bool Ready() const {
        return swapchain && device && dxgiDevice;
    }
    UINT FrameLatency() const {
        UINT latency = 0;
        return SUCCEEDED(dxgiDevice->GetMaximumFrameLatency(&latency)) ? latency : 0;
    }

    HWND window = nullptr;
    HRESULT hr = E_FAIL;
    ComPtr<ID3D10Device1> device;
    ComPtr<IDXGISwapChain> swapchain;
    ComPtr<IDXGIDevice1> dxgiDevice;
};

TEST(FlowD3D10Prerender, ClassifiedD3D10SwapchainsGetTheDeviceFrameLatency) {
    ce::flow::FlowGame game(ce::flow::CurrentTestName());
    const auto apply = reinterpret_cast<void (*)(IDXGISwapChain*, float, bool)>(
        GetProcAddress(GetModuleHandleW(L"capture_hook_x64.dll"), "CEFlow_ApplyPrerenderLimit"));
    const auto release = reinterpret_cast<void (*)()>(
        GetProcAddress(GetModuleHandleW(L"capture_hook_x64.dll"), "CEFlow_D3D11PrerenderRelease"));
    ASSERT_NE(apply, nullptr);
    ASSERT_NE(release, nullptr);
    D3D10Game d3d10;
    ASSERT_TRUE(d3d10.Ready()) << "hr=0x" << std::hex << static_cast<uint32_t>(d3d10.hr);
    ComPtr<ID3D11Device> layered;
    const bool answersD3D11 = SUCCEEDED(d3d10.swapchain->GetDevice(IID_PPV_ARGS(&layered))) && layered;
    layered.Reset();
    RecordProperty("swapchain_answers_id3d11device", answersD3D11 ? "1" : "0");

    ASSERT_TRUE(SUCCEEDED(d3d10.dxgiDevice->SetMaximumFrameLatency(3)));
    for (int limit : {1, 2, 5, 6}) {
        apply(d3d10.swapchain.Get(), static_cast<float>(limit), true);
        EXPECT_EQ(d3d10.FrameLatency(), static_cast<UINT>(limit))
            << "limit " << limit << (answersD3D11 ? " (the swapchain also answers ID3D11Device)" : "");
    }
    // Disabled (-1) leaves the game's latency alone.
    apply(d3d10.swapchain.Get(), -1.0f, true);
    EXPECT_EQ(d3d10.FrameLatency(), 6u);
    release();
}

}  // namespace
