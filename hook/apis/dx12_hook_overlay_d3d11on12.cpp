#include "dx12_hook_internal.h"


static bool InitD3D11On12(ID3D12Device* d3d12Dev, ID3D12CommandQueue* queue, IDXGISwapChain* swapChain,
                          UINT bufferCount) {
    if (dx12_hook_g_State.d3d11on12Init)
        return true;

    HookLogImportant("DX12 D3D11On12: Initializing bridge (dev=%p queue=%p bufCnt=%u)", d3d12Dev, queue, bufferCount);

    if (!d3d12Dev || !queue || !swapChain || bufferCount == 0)
        return false;

    // Dynamically load D3D11On12CreateDevice
    HMODULE d3d11Lib = GetModuleHandleA("d3d11.dll");
    if (!d3d11Lib)
        d3d11Lib = ce::security::LoadSystemLibrary(L"d3d11.dll");
    if (!d3d11Lib) {
        HookLogImportant("DX12 D3D11On12: d3d11.dll not available");
        return false;
    }

    using PFN_D3D11On12 = decltype(&D3D11On12CreateDevice);
    auto pfnCreate = (PFN_D3D11On12)GetProcAddress(d3d11Lib, "D3D11On12CreateDevice");
    if (!pfnCreate) {
        HookLogImportant("DX12 D3D11On12: D3D11On12CreateDevice not found");
        return false;
    }

    // Create D3D11on12 device wrapping the game's D3D12 device + queue
    IUnknown* queues[] = {queue};
    ID3D11Device* d3d11Dev = nullptr;
    ID3D11DeviceContext* d3d11Ctx = nullptr;
    HRESULT hr =
        pfnCreate(d3d12Dev, D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, queues, 1, 0, &d3d11Dev, &d3d11Ctx, nullptr);
    if (FAILED(hr) || !d3d11Dev) {
        HookLogImportant("DX12 D3D11On12: D3D11On12CreateDevice failed hr=0x%08X", hr);
        return false;
    }

    // Get the ID3D11On12Device interface for wrapping resources
    ID3D11On12Device* d3d11on12 = nullptr;
    hr = d3d11Dev->QueryInterface(IID_PPV_ARGS(&d3d11on12));
    if (FAILED(hr)) {
        HookLogImportant("DX12 D3D11On12: QI for ID3D11On12Device failed hr=0x%08X", hr);
        d3d11Ctx->Release();
        d3d11Dev->Release();
        return false;
    }

    HookLogImportant("DX12 D3D11On12: Bridge device created successfully");

    // Wrap each D3D12 backbuffer for D3D11 use
    IDXGISwapChain3* sc3 = nullptr;
    hr = swapChain->QueryInterface(IID_PPV_ARGS(&sc3));
    if (FAILED(hr)) {
        HookLogImportant("DX12 D3D11On12: QI for IDXGISwapChain3 failed hr=0x%08X", hr);
        d3d11on12->Release();
        d3d11Ctx->Release();
        d3d11Dev->Release();
        return false;
    }

    std::vector<ID3D11Resource*> wrappedBBs;
    std::vector<ID3D11RenderTargetView*> rtvs;
    bool wrapOk = true;

    for (UINT i = 0; i < bufferCount && wrapOk; i++) {
        ID3D12Resource* d3d12BB = nullptr;
        hr = sc3->GetBuffer(i, IID_PPV_ARGS(&d3d12BB));
        if (FAILED(hr) || !d3d12BB) {
            HookLogImportant("DX12 D3D11On12: GetBuffer(%u) failed hr=0x%08X", i, hr);
            wrapOk = false;
            break;
        }

        D3D11_RESOURCE_FLAGS flags = {};
        flags.BindFlags = D3D11_BIND_RENDER_TARGET;

        ID3D11Resource* wrapped = nullptr;
        hr = d3d11on12->CreateWrappedResource(d3d12BB, &flags, D3D12_RESOURCE_STATE_PRESENT,
                                              D3D12_RESOURCE_STATE_PRESENT, IID_PPV_ARGS(&wrapped));
        d3d12BB->Release();
        if (FAILED(hr) || !wrapped) {
            HookLogImportant("DX12 D3D11On12: CreateWrappedResource(%u) failed hr=0x%08X", i, hr);
            wrapOk = false;
            break;
        }

        ID3D11RenderTargetView* rtv = nullptr;
        hr = d3d11Dev->CreateRenderTargetView(wrapped, nullptr, &rtv);
        if (FAILED(hr)) {
            HookLogImportant("DX12 D3D11On12: CreateRenderTargetView(%u) failed hr=0x%08X", i, hr);
            wrapped->Release();
            wrapOk = false;
            break;
        }

        wrappedBBs.push_back(wrapped);
        rtvs.push_back(rtv);
    }
    sc3->Release();

    if (!wrapOk) {
        for (auto* r : rtvs)
            if (r)
                r->Release();
        for (auto* w : wrappedBBs)
            if (w)
                w->Release();
        d3d11on12->Release();
        d3d11Ctx->Release();
        d3d11Dev->Release();
        return false;
    }

    // Initialize the SL FG overlay adapter with the D3D11on12 device.
    // This creates the DX11Backend (shaders, font texture, blend states).
    if (!dx12_hook_g_SLFGAdapter.InitDX11(d3d11Dev, d3d11Ctx)) {
        HookLogImportant("DX12 D3D11On12: OverlayAdapter.InitDX11 failed");
        for (auto* r : rtvs)
            if (r)
                r->Release();
        for (auto* w : wrappedBBs)
            if (w)
                w->Release();
        d3d11on12->Release();
        d3d11Ctx->Release();
        d3d11Dev->Release();
        return false;
    }

    // Store everything
    dx12_hook_g_State.d3d11on12Device = d3d11Dev;
    dx12_hook_g_State.d3d11on12Context = d3d11Ctx;
    dx12_hook_g_State.d3d11on12 = d3d11on12;
    dx12_hook_g_State.d3d11WrappedBBs = std::move(wrappedBBs);
    dx12_hook_g_State.d3d11RTVs = std::move(rtvs);
    dx12_hook_g_State.d3d11on12Init = true;

    HookLogImportant("DX12 D3D11On12: Initialized (%u buffers wrapped)", bufferCount);
    return true;
}
static bool RenderOverlayViaD3D11On12(int bufferIdx, bool isRealFrame) {
    if (!dx12_hook_g_State.d3d11on12Init || !dx12_hook_g_State.d3d11on12 || !dx12_hook_g_State.d3d11on12Context)
        return false;

    if (bufferIdx < 0 || bufferIdx >= (int)dx12_hook_g_State.d3d11WrappedBBs.size())
        return false;

    auto* wrapped = dx12_hook_g_State.d3d11WrappedBBs[bufferIdx];
    auto* rtv = dx12_hook_g_State.d3d11RTVs[bufferIdx];
    if (!wrapped || !rtv)
        return false;

    static uint64_t s_d3d11on12FrameCount = 0;
    s_d3d11on12FrameCount++;
    if (s_d3d11on12FrameCount <= 5 || (s_d3d11on12FrameCount % 300) == 0) {
        HookLogImportant("DX12 D3D11On12: RenderOverlay frame #%llu (bufIdx=%d)",
                         (unsigned long long)s_d3d11on12FrameCount, bufferIdx);
    }

    // Acquire: internally transitions backbuffer to RENDER_TARGET
    dx12_hook_g_State.d3d11on12->AcquireWrappedResources(&wrapped, 1);

    // Set render target on D3D11 context
    dx12_hook_g_State.d3d11on12Context->OMSetRenderTargets(1, &rtv, nullptr);

    D3D11_VIEWPORT vp = {0.0f, 0.0f, (float)dx12_hook_g_State.cachedWidth, (float)dx12_hook_g_State.cachedHeight, 0.0f, 1.0f};
    dx12_hook_g_State.d3d11on12Context->RSSetViewports(1, &vp);

    // Feed data to the SL FG overlay adapter
    dx12_hook_g_SLFGAdapter.SetIPCClient(g_IPC);
    dx12_hook_g_SLFGAdapter.SetReserveInactiveFGSpace(false);
    const auto metricsBinding = ce::dx12_overlay_policy::DecideOverlayMetricsBinding(isRealFrame);
    if (metricsBinding.bindMetrics) {
        dx12_hook_g_SLFGAdapter.SetMetrics(DXGIShared::GetPerformanceMetrics());
    }
    if (metricsBinding.refreshFrameMetadata) {
        static const bool s_isVKD3D = []() {
            return GetModuleHandleA("d3d12core.dll") &&
                   (GetModuleHandleA("libvkd3d-1.dll") || GetModuleHandleA("vkd3d.dll"));
        }();
        const char* api = s_isVKD3D ? "DX12 (VKD3D)" : "DX12";
        dx12_hook_g_SLFGAdapter.SetGraphicsAPI(api);
    }

    // Render overlay via D3D11 backend (no descriptor heaps!)
    dx12_hook_g_SLFGAdapter.RenderOverlay(dx12_hook_g_State.cachedWidth, dx12_hook_g_State.cachedHeight);

    // Release: internally transitions backbuffer back to PRESENT
    dx12_hook_g_State.d3d11on12->ReleaseWrappedResources(&wrapped, 1);

    // Flush submits all D3D11 commands to the D3D12 queue
    dx12_hook_g_State.d3d11on12Context->Flush();

    return true;
}
void CleanupD3D11On12() {
    // Warm-backend: the x64 descriptor-free adapter is DEVICE-scoped (PSOs,
    // font buffer, vb/ib pool; backbuffer fetched per frame) and survives
    // swapchain teardown so the first present of the next swapchain can draw
    // without a backend rebuild. It is rebuilt only on device/format change

    // (EnsureDescFreeBackendForDeviceAndFormat), x86 Texture2D selection, and
    // DX12Hook::Shutdown. The x86 Texture2D adapter (no DescFree backend
    // tracked) keeps its original swapchain-scoped teardown.
    if (!dx12_hook_g_DescFreeBackend) {
        ShutdownDescFreeBackend("CleanupD3D11On12", true);
    }
    // Clean up SL FG D3D11On12 adapter
    if (dx12_hook_g_SLFGAdapter.IsInitialized()) {
        dx12_hook_g_SLFGAdapter.SetShutdownMode(true);
        dx12_hook_g_SLFGAdapter.Shutdown();
    }
    // Device-level D3D11On12 cleanup happens in g_State.Cleanup()
}
