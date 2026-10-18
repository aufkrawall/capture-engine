#include "tests/flow/flow_test_support.h"

#include <d3d11.h>
#include <dxgi1_4.h>
#include <wrl/client.h>

#include <string>

// What Windows' own D3D11 reports for a FLIP chain's current back buffer, on a real WARP device.
// CE's D3D10/D3D11 overlay draws on GetBuffer(GetCurrentBackBufferIndex()) and retains one view per
// index (hook/d3d11/dx11_back_buffer_view_cache.h). DXUP's D3D11 rotates that index over distinct
// textures, which made the old single retained view rebuild on every Present (Mirror's Edge,
// 20261018_143747); DXVK's always reports 0 (upstream D3D11SwapChain::GetImageIndex).
namespace {
using Microsoft::WRL::ComPtr;

struct FlipObservation {
    HRESULT hr = E_FAIL;
    std::string indices;
    HRESULT secondBuffer = E_FAIL;  // GetBuffer(1)
    bool distinctBuffers = false;
};

FlipObservation ObserveFlipChain(DXGI_SWAP_EFFECT effect) {
    FlipObservation result;
    const HWND window = CreateWindowExW(0, L"STATIC", L"ce_flow_d3d11_flip_index", WS_POPUP, 0, 0, 64, 64, nullptr,
                                        nullptr, GetModuleHandleW(nullptr), nullptr);
    // The system runtime directly: the flow process's own imports may resolve to CE's hooks.
    const HMODULE runtime = LoadLibraryExW(L"d3d11.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    const auto create = runtime ? reinterpret_cast<decltype(&D3D11CreateDeviceAndSwapChain)>(
                                      GetProcAddress(runtime, "D3D11CreateDeviceAndSwapChain"))
                                : nullptr;
    if (!window || !create)
        return result;
    DXGI_SWAP_CHAIN_DESC desc = {};
    desc.BufferDesc.Width = 64;
    desc.BufferDesc.Height = 64;
    desc.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = 3;
    desc.OutputWindow = window;
    desc.Windowed = TRUE;
    desc.SwapEffect = effect;
    ComPtr<IDXGISwapChain> swapchain;
    ComPtr<ID3D11Device> device;
    result.hr = create(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &desc, &swapchain,
                       &device, nullptr, nullptr);
    ComPtr<IDXGISwapChain3> swapchain3;
    if (SUCCEEDED(result.hr))
        result.hr = swapchain.As(&swapchain3);
    if (SUCCEEDED(result.hr)) {
        ComPtr<ID3D11Texture2D> first;
        ComPtr<ID3D11Texture2D> second;
        swapchain->GetBuffer(0, IID_PPV_ARGS(&first));
        result.secondBuffer = swapchain->GetBuffer(1, IID_PPV_ARGS(&second));
        result.distinctBuffers = first && second && first.Get() != second.Get();
        for (int present = 0; present < 6; ++present) {
            result.indices += std::to_string(swapchain3->GetCurrentBackBufferIndex());
            swapchain->Present(0, 0);
        }
    }
    swapchain3.Reset();
    swapchain.Reset();
    device.Reset();
    DestroyWindow(window);
    return result;
}

// Windows rotates the textures behind index 0 instead of the index (as DXVK does), so CE retains
// one view there and native D3D11 never had the per-Present rebuild. Only buffer 0 is the one to
// draw on: FLIP_DISCARD refuses the others, FLIP_SEQUENTIAL hands out read-only ones.
TEST(FlowD3D11FlipBufferIndex, WindowsD3D11KeepsTheCurrentBackBufferAtIndexZero) {
    ce::flow::FlowGame game(ce::flow::CurrentTestName());
    for (const DXGI_SWAP_EFFECT effect : {DXGI_SWAP_EFFECT_FLIP_DISCARD, DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL}) {
        SCOPED_TRACE(static_cast<int>(effect));
        const FlipObservation observed = ObserveFlipChain(effect);
        ASSERT_TRUE(SUCCEEDED(observed.hr)) << std::hex << observed.hr;
        EXPECT_EQ(observed.indices, "000000");
        if (effect == DXGI_SWAP_EFFECT_FLIP_DISCARD) {
            EXPECT_EQ(observed.secondBuffer, DXGI_ERROR_INVALID_CALL) << std::hex << observed.secondBuffer;
        } else {
            EXPECT_EQ(observed.secondBuffer, S_OK) << std::hex << observed.secondBuffer;
            EXPECT_TRUE(observed.distinctBuffers);
        }
    }
}

}  // namespace
