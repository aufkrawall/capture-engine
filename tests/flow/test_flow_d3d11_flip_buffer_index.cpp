#include "tests/flow/flow_test_support.h"

#include "tests/flow/fakes/fake_dxgi_forwarders.h"

#include <d3d11.h>
#include <dxgi1_4.h>
#include <wrl/client.h>

#include <string>

// Which back buffer of a D3D10/D3D11 chain CE draws on: always buffer 0 (kDX11ApplicationBackBuffer,
// hook/d3d11/dx11_back_buffer_view_cache.h). What Windows' own D3D11 reports for a FLIP chain, on a real
// WARP device, and how CE's overlay treats a translator's chain whose GetCurrentBackBufferIndex rotates
// although only buffer 0 is the application's: DXUP's D3D12-backed D3D11 (Mirror's Edge, 20261018_143747
// and 20261022_162106). DXVK's always reports 0 (upstream D3D11SwapChain::GetImageIndex).
namespace {
using Microsoft::WRL::ComPtr;

struct FlipChain {
    HWND window = nullptr;
    ComPtr<ID3D11Device> device;
    ComPtr<IDXGISwapChain> swapchain;
    HRESULT hr = E_FAIL;

    FlipChain(DXGI_SWAP_EFFECT effect, UINT bufferCount) {
        window = CreateWindowExW(0, L"STATIC", L"ce_flow_d3d11_flip_index", WS_POPUP, 0, 0, 64, 64, nullptr, nullptr,
                                 GetModuleHandleW(nullptr), nullptr);
        // The system runtime directly: the flow process's own imports may resolve to CE's hooks.
        const HMODULE runtime = LoadLibraryExW(L"d3d11.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        const auto create = runtime ? reinterpret_cast<decltype(&D3D11CreateDeviceAndSwapChain)>(
                                          GetProcAddress(runtime, "D3D11CreateDeviceAndSwapChain"))
                                    : nullptr;
        if (!window || !create)
            return;
        DXGI_SWAP_CHAIN_DESC desc = {};
        desc.BufferDesc.Width = 64;
        desc.BufferDesc.Height = 64;
        desc.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        desc.BufferCount = bufferCount;
        desc.OutputWindow = window;
        desc.Windowed = TRUE;
        desc.SwapEffect = effect;
        hr = create(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &desc, &swapchain,
                    &device, nullptr, nullptr);
    }
    ~FlipChain() {
        swapchain.Reset();
        device.Reset();
        if (window)
            DestroyWindow(window);
    }
};

struct FlipObservation {
    HRESULT hr = E_FAIL;
    std::string indices;
    HRESULT secondBuffer = E_FAIL;  // GetBuffer(1)
    bool distinctBuffers = false;
};

FlipObservation ObserveFlipChain(DXGI_SWAP_EFFECT effect) {
    FlipObservation result;
    FlipChain chain(effect, 3);
    result.hr = chain.hr;
    ComPtr<IDXGISwapChain3> swapchain3;
    if (SUCCEEDED(result.hr))
        result.hr = chain.swapchain.As(&swapchain3);
    if (SUCCEEDED(result.hr)) {
        ComPtr<ID3D11Texture2D> first;
        ComPtr<ID3D11Texture2D> second;
        chain.swapchain->GetBuffer(0, IID_PPV_ARGS(&first));
        result.secondBuffer = chain.swapchain->GetBuffer(1, IID_PPV_ARGS(&second));
        result.distinctBuffers = first && second && first.Get() != second.Get();
        for (int present = 0; present < 6; ++present) {
            result.indices += std::to_string(swapchain3->GetCurrentBackBufferIndex());
            chain.swapchain->Present(0, 0);
        }
    }
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

// DXUP 1.1.0's D3D11 chain: GetCurrentBackBufferIndex is the D3D12 chain's beneath it (0, 1, 0, ...)
// while, like Windows' FLIP_DISCARD, only buffer 0 is handed out. The real WARP chain below refuses
// GetBuffer(1) the same way. The test advances the index itself: presenting the real chain would run
// CE's own Present hook on it, a second D3D11 chain for the overlay to alternate with.
class RotatingIndexChain final : public ce::flow::fake::ForwardingSwapChain {
public:
    using ForwardingSwapChain::ForwardingSwapChain;

    UINT STDMETHODCALLTYPE GetCurrentBackBufferIndex() override { return presents_ % 2; }
    void Advance() { ++presents_; }
    HRESULT STDMETHODCALLTYPE GetBuffer(UINT index, REFIID riid, void** surface) override {
        const HRESULT hr = ForwardingSwapChain::GetBuffer(index, riid, surface);
        ++getBuffers;
        refused += FAILED(hr) ? 1u : 0u;
        return hr;
    }

    unsigned getBuffers = 0;
    unsigned refused = 0;

private:
    UINT presents_ = 0;
};

// CE asked such a chain for the index it reported: every second Present lost the overlay (GetBuffer(1)
// refused) and logged two lines, 3388 of 6788 Presents in 20261022_162106. The overlay draws on the
// application's buffer 0 on every Present and keeps one view of it.
TEST(FlowD3D11FlipBufferIndex, OverlayDrawsOnBufferZeroOfATranslatorsRotatingChain) {
    ce::flow::FlowGame game(ce::flow::CurrentTestName());
    const HMODULE hook = GetModuleHandleW(L"capture_hook_x64.dll");
    const auto draw = reinterpret_cast<void (*)(IDXGISwapChain*)>(GetProcAddress(hook, "CEFlow_DrawDX11Overlay"));
    const auto retainedViews = reinterpret_cast<size_t (*)()>(GetProcAddress(hook, "CEFlow_DX11OverlayRetainedViews"));
    const auto cleanup = reinterpret_cast<void (*)()>(GetProcAddress(hook, "CEFlow_CleanupDX11Resources"));
    ASSERT_TRUE(draw && retainedViews && cleanup);

    FlipChain real(DXGI_SWAP_EFFECT_FLIP_DISCARD, 2);
    ASSERT_TRUE(SUCCEEDED(real.hr)) << std::hex << real.hr;
    ComPtr<IDXGISwapChain4> real4;
    ASSERT_TRUE(SUCCEEDED(real.swapchain.As(&real4)));
    ComPtr<RotatingIndexChain> chain;
    chain.Attach(new RotatingIndexChain(real4.Get()));

    for (int present = 0; present < 6; ++present) {
        draw(chain.Get());
        chain->Advance();
    }
    EXPECT_EQ(chain->refused, 0u) << "CE asked for a back buffer the chain does not hand out; logs: "
                                  << game.LogDirectory();
    EXPECT_EQ(chain->getBuffers, 1u) << "one view of buffer 0, kept across Presents; logs: " << game.LogDirectory();
    EXPECT_EQ(retainedViews(), 1u);

    cleanup();
    EXPECT_EQ(retainedViews(), 0u);
}

}  // namespace
