#include "dx11_hook_internal.h"
#include "hook/runtime/api_translator_modules.h"
#include "hook/runtime/dxup_api_label.h"
#include "hook/runtime/module_set_generation.h"
#include "common/logging/log_meter.h"
#include "dx11_overlay_gpu_timer.h"
#include "hook/metrics/perf_logger.h"


// The D3D11 overlay's API label. It asks the loader for up to seven modules and reads version
// resources, so it is kept until a module loads or unloads or the evidence it names changes
// (Mirror's Edge on DXUP at 630 FPS, 20261018_151515).
static const std::string& ResolveDX11OverlayApiLabel(unsigned d3d11Minor) {
    thread_local std::string t_label;
    thread_local unsigned long long t_modules = 0;
    thread_local unsigned t_minor = ~0u;
    thread_local unsigned long long t_evidence = ~0ull;
    const unsigned long long modules = LoadedModuleSetGeneration();
    const unsigned long long evidence = DxupLabelEvidence();
    if (modules != 0 && modules == t_modules && d3d11Minor == t_minor && evidence == t_evidence)
        return t_label;
    // DXUP's D3D9/D3D8/DirectDraw frontends present through this swapchain: name the game's API. They
    // render through DXUP's own d3d11/dxgi, or through Windows' when only the frontend DLL sits next to
    // the game - asking for DXUP's d3d11/dxgi alone made Mirror's Edge with DXUP's d3d9.dll read "DX11"
    // (20261022_162106). DXUP is known by its version resource: ReShade, dgVoodoo or Special K under the
    // same file names keep the D3D11 label.
    std::string dxupLabel = DxupOverlayApiLabel(d3d11Minor);
    t_label = !dxupLabel.empty() ? std::move(dxupLabel)
                                 : ce::graphics_api_identity::D3D11Label(d3d11Minor, IsDXVKD3D10OrD3D11Loaded());
    t_modules = modules;
    t_minor = d3d11Minor;
    t_evidence = evidence;
    return t_label;
}

static void DrawDX10Overlay(IDXGISwapChain* pSwapChain, HWND currentHwnd, int frameCount) {
    ID3D10Device* device = nullptr;
    if (FAILED(pSwapChain->GetDevice(__uuidof(ID3D10Device), (void**)&device))) {
        if (FAILED(pSwapChain->GetDevice(__uuidof(ID3D10Device1), (void**)&device))) {
            static std::atomic<int> s_getDeviceFailureLogCount{0};
            const int logCount = s_getDeviceFailureLogCount.fetch_add(1, std::memory_order_relaxed);
            if (logCount < 5) {
                HookLogImportant("DX10: DrawOverlay failed to get ID3D10Device/Device1 from swapchain %p", pSwapChain);
            }
            return;
        }
    }

    // Capture/Hook on the real device seen in Present
    static ID3D10Device* s_HookedDevice = nullptr;
    static HWND s_LastDX10OverlayHwnd = nullptr;
    if (s_HookedDevice != device) {
        dx11_hook_g_overlayViews10.Clear([](ID3D10RenderTargetView* view) { view->Release(); });
        dx11_hook_g_mainRenderTargetView10 = nullptr;
        s_LastDX10OverlayHwnd = currentHwnd;
        if (g_OverlayAdapter.IsInitialized()) {
            HookLogImportant("DX10: Device changed, reinitializing overlay backend");
            g_OverlayAdapter.Shutdown();
        }
        // Initialize System Metrics
        IDXGIDevice* dxgiDevice = nullptr;

        if (SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&dxgiDevice)))) {
            IDXGIAdapter* adapter = nullptr;
            if (SUCCEEDED(dxgiDevice->GetAdapter(&adapter))) {
                DXGI_ADAPTER_DESC adapterDesc;
                if (SUCCEEDED(adapter->GetDesc(&adapterDesc))) {
                    // NOLINTNEXTLINE(bugprone-narrowing-conversions) - intentional narrowing; value is range-bounded by the surrounding API/geometry contract
                    SystemMetricsCollector::Get().Initialize(adapterDesc.AdapterLuid.LowPart,
                                                             adapterDesc.AdapterLuid.HighPart);
                }
                adapter->Release();
            }
            dxgiDevice->Release();
        }

        s_HookedDevice = device;
    }

    if (s_LastDX10OverlayHwnd && s_LastDX10OverlayHwnd != currentHwnd && g_OverlayAdapter.IsInitialized()) {
        HookLogImportant("DX10: HWND changed from %p to %p, reinitializing overlay backend", s_LastDX10OverlayHwnd,
                         currentHwnd);
        g_OverlayAdapter.Shutdown();
    }
    s_LastDX10OverlayHwnd = currentHwnd;

    DXGI_SWAP_CHAIN_DESC desc = {};
    if (FAILED(pSwapChain->GetDesc(&desc))) {
        static std::atomic<int> s_getDescFailureLogCount{0};
        const int logCount = s_getDescFailureLogCount.fetch_add(1, std::memory_order_relaxed);
        if (logCount < 5) {
            HookLogImportant("DX10: DrawOverlay failed to get swapchain desc for %p", pSwapChain);
        }
        device->Release();
        return;
    }

    const UINT bufferIndex = kDX11ApplicationBackBuffer;
    dx11_hook_g_mainRenderTargetView10 = dx11_hook_g_overlayViews10.Find(pSwapChain, bufferIndex);
    if (!dx11_hook_g_mainRenderTargetView10) {
        ID3D10RenderTargetView* createdRTV = nullptr;
        ID3D10Texture2D* backbuffer = nullptr;
        HRESULT hr = pSwapChain->GetBuffer(bufferIndex, IID_PPV_ARGS(&backbuffer));
        if (SUCCEEDED(hr) && backbuffer) {
            hr = device->CreateRenderTargetView(backbuffer, nullptr, &createdRTV);
            backbuffer->Release();
        }
        if (SUCCEEDED(hr) && createdRTV &&
            !dx11_hook_g_overlayViews10.Store(pSwapChain, bufferIndex, createdRTV,
                                              [](ID3D10RenderTargetView* view) { view->Release(); })) {
            createdRTV->Release();
            createdRTV = nullptr;
            hr = E_INVALIDARG;
        }
        dx11_hook_g_mainRenderTargetView10 = createdRTV;
        if (FAILED(hr) || !dx11_hook_g_mainRenderTargetView10) {
            static std::atomic<int> s_rtvFailureLogCount{0};
            const int logCount = s_rtvFailureLogCount.fetch_add(1, std::memory_order_relaxed);
            if (logCount < 10) {
                HookLogImportant("DX10: Failed to create overlay RTV for swapchain %p buffer=%u hr=0x%08X", pSwapChain,
                                 bufferIndex, (unsigned)hr);
            }
            device->Release();
            return;
        }
        HookLogImportant("DX10: Overlay RTV ready for swapchain %p buffer=%u (retained views=%zu)", pSwapChain,
                         bufferIndex, dx11_hook_g_overlayViews10.Count());
    }

    // Render the overlay
    if (!g_OverlayAdapter.IsInitialized()) {
        HookLog("DX10: Initializing OverlayAdapter...");
        g_OverlayAdapter.SetHwnd(currentHwnd);
        g_OverlayAdapter.InitDX10(device);
    }

    if (g_OverlayAdapter.IsInitialized()) {
        g_OverlayAdapter.SetMetrics(DXGIShared::GetPerformanceMetrics());
        g_OverlayAdapter.SetIPCClient(g_IPC);
        const bool is10_1 = ResolveD3D10Is10_1(device, pSwapChain);
        const std::string apiLabel = ce::graphics_api_identity::D3D10Label(is10_1, IsDXVKD3D10OrD3D11Loaded());
        g_OverlayAdapter.SetGraphicsAPI(apiLabel.c_str(), "active D3D10 swapchain device");
        const auto presentationEncoding =
            DXGIShared::ResolveSwapChainPresentationEncoding(pSwapChain, desc.BufferDesc.Format);
        g_OverlayAdapter.SetHDR(ce::presentation_color::IsHDR(presentationEncoding),
                                static_cast<int>(desc.BufferDesc.Format));

        RECT rect;
        if (GetClientRect(currentHwnd, &rect)) {
            int width = rect.right - rect.left;
            int height = rect.bottom - rect.top;
            if (width > 0 && height > 0) {
                ID3D10RenderTargetView* previousRTV = nullptr;
                ID3D10DepthStencilView* previousDSV = nullptr;
                device->OMGetRenderTargets(1, &previousRTV, &previousDSV);
                ID3D10RenderTargetView* overlayRTV = dx11_hook_g_mainRenderTargetView10;
                device->OMSetRenderTargets(1, &overlayRTV, nullptr);
                g_OverlayAdapter.RenderOverlay(width, height);
                device->OMSetRenderTargets(1, &previousRTV, previousDSV);
                if (previousRTV) {
                    previousRTV->Release();
                }
                if (previousDSV) {
                    previousDSV->Release();
                }
            }
        }
    }

    device->Release();
}
static bool IsReadableMemoryDX11(const void* ptr, size_t size) {
    if (!ptr)
        return false;
    MEMORY_BASIC_INFORMATION mbi;
    if (VirtualQuery(ptr, &mbi, sizeof(mbi)) == 0)
        return false;
    if (mbi.State != MEM_COMMIT)
        return false;
    if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD))
        return false;
    return (mbi.Protect &
            (PAGE_READONLY | PAGE_READWRITE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_WRITECOPY)) != 0;
}

void DrawDX11Overlay(IDXGISwapChain* pSwapChain) {
    dx11_hook_g_UnsafeSwapChainObserved = false;
    // CRITICAL: Skip all rendering during shutdown to prevent crashes
    // when D3D device is destroyed while we're trying to use it
    if (HookIsShuttingDown()) {
        return;
    }

    // CRITICAL: Null pointer check
    if (!pSwapChain) {
        return;
    }

    static HWND lastHwnd = NULL;
    static int frameCount = 0;
    static IDXGISwapChain* s_lastNvPresentOverlaySwapChain = nullptr;
    static int64_t s_lastNvPresentOverlayUs = 0;

    frameCount++;

    // SAFETY: Verify the swapchain pointer is valid before accessing it
    if (!IsReadableMemoryDX11(pSwapChain, sizeof(void*))) {
        EarlyLog("DX11: Swapchain memory not readable at frame %d — skipping CE work for this Present", frameCount);
        dx11_hook_g_UnsafeSwapChainObserved = true;
        return;
    }

    DXGI_SWAP_CHAIN_DESC desc;
    if (FAILED(pSwapChain->GetDesc(&desc))) {
        EarlyLog("DX11: GetDesc failed at frame %d — bailing", frameCount);
        return;
    }
    HWND currentHwnd = desc.OutputWindow;

    const bool nvPresentLoaded = g_FGCompat.IsNvPresentLoaded();
    // This Present's chain belongs to a present interposer (NVIDIA Smooth
    // Motion's NvPresent64), not to the application. CE may draw on it - the
    // frame is already generated and every dxgi-entry overlay has already
    // composited - but it must not RETAIN anything the interposer owns, and it
    // must not borrow state the interposer left behind. See
    // ce::overlay_compat::PresentInterposerCompositeRoute.
    const bool interposerPrivateOutputChain = DXGIShared::IsPresentOnPresentInterposerPrivateOutputChain();
    if (nvPresentLoaded && ShouldSkipWindowForNvPresent(currentHwnd)) {
        return;
    }

    // NVIDIA Smooth Motion can trigger paired Present callbacks for the same
    // frame in quick succession. Only suppress near-immediate duplicates on the
    // same swapchain to avoid dropping legitimate output frames.
    //
    // NOT on a present interposer's private OUTPUT chain. That chain is
    // presented once per frame the driver actually puts on screen, and the
    // driver's metering deliberately places a generated frame close behind its
    // real partner - so a sub-millisecond gap there is the second DISPLAYED
    // frame, not a duplicate callback. Suppressing it drops the overlay from
    // that frame entirely, which is the flicker: Witcher 3 session
    // 20260919_160555 skipped 2453 of 6962 presents (35%) this way, every one of
    // them reaching the screen without the overlay. The rule for this chain is
    // already stated in present-interposers.md - "the overlay is drawn on every
    // displayed frame, real and generated alike".
    if (nvPresentLoaded && !interposerPrivateOutputChain) {
        static constexpr int64_t kNvPresentExactDuplicateUs = 500;
        int64_t nowUs = PerfLogger::GetQpcUs();
        bool hasPreviousSample = (s_lastNvPresentOverlayUs > 0 && nowUs > s_lastNvPresentOverlayUs);
        int64_t deltaUs = hasPreviousSample ? (nowUs - s_lastNvPresentOverlayUs) : 0;
        bool sameSwapChain = (s_lastNvPresentOverlaySwapChain == pSwapChain);

        bool skipExactDuplicate = sameSwapChain && hasPreviousSample && deltaUs < kNvPresentExactDuplicateUs;

        if (skipExactDuplicate) {
            return;
        }
        s_lastNvPresentOverlaySwapChain = pSwapChain;
        s_lastNvPresentOverlayUs = nowUs;
    }

    // Skip overlay if the window is being destroyed — the D3D device may already
    // be partially torn down, making GetDevice/GetBuffer unsafe.
    if (!currentHwnd || !IsWindow(currentHwnd)) {
        EarlyLog(
            "DX11: Window invalid at frame %d (hwnd=%p, IsWindow=%d) — "
            "skipping CE work for this Present",
            frameCount, currentHwnd, currentHwnd ? IsWindow(currentHwnd) : 0);
        // Swapchain/window recreation is transient in many games and injectors.
        // Abort this call without globally disabling hooks; a later valid
        // swapchain must be able to recover the overlay.
        dx11_hook_g_UnsafeSwapChainObserved = true;
        return;
    }

    const DXGIShared::APIType swapChainApi = DetectSwapChainAPITypeForDX11Hook(pSwapChain);
    if (swapChainApi == DXGIShared::APIType::D3D10) {
        dx11_hook_g_DetectedAPI = GetDX11HookOverlayAPIName(swapChainApi);
        DrawDX10Overlay(pSwapChain, currentHwnd, frameCount);
        return;
    }
    if (swapChainApi != DXGIShared::APIType::D3D11) {
        static std::atomic<int> s_unexpectedApiLogCount{0};
        if (s_unexpectedApiLogCount.fetch_add(1, std::memory_order_relaxed) < 5) {
            EarlyLog("DX11: DrawOverlay skipping swapchain classified as %s", GetDX11HookBaseAPIName(swapChainApi));
        }
        return;
    }
    dx11_hook_g_DetectedAPI = GetDX11HookOverlayAPIName(swapChainApi);

    if (frameCount % 60 == 0) {
        EarlyLog("DX: DrawOverlay frame %d on SC %p (HWND %p, %ux%u)", frameCount, pSwapChain, currentHwnd,
                 desc.BufferDesc.Width, desc.BufferDesc.Height);
    }

    // Acquire device/context — use cached (AddRef'd) pointers for subsequent
    // frames. Calling pSwapChain->GetDevice() every frame is unsafe during
    // shutdown (the swapchain's internal device ref can be freed before our
    // Present hook stops being called).
    if (!dx11_hook_g_pd3dDevice) {
        // First frame: cache the DX11 device with AddRef.
        ID3D11Device* device11 = NULL;
        HRESULT hrDevice = pSwapChain->GetDevice(IID_PPV_ARGS(&device11));
        if (FAILED(hrDevice) || !device11) {
            EarlyLog("%s: FAILED to get D3D11 device (hr=0x%08X)", dx11_hook_g_DetectedAPI, hrDevice);
            return;
        }
        EarlyLog("%s: Identified as D3D11 device %p", dx11_hook_g_DetectedAPI, device11);

        // Initialize System Metrics (one-time)
        IDXGIDevice* dxgiDevice = nullptr;
        if (SUCCEEDED(device11->QueryInterface(IID_PPV_ARGS(&dxgiDevice)))) {
            IDXGIAdapter* adapter = nullptr;
            if (SUCCEEDED(dxgiDevice->GetAdapter(&adapter))) {
                DXGI_ADAPTER_DESC adapterDesc;
                if (SUCCEEDED(adapter->GetDesc(&adapterDesc))) {
                    // NOLINTNEXTLINE(bugprone-narrowing-conversions) - intentional narrowing; value is range-bounded by the surrounding API/geometry contract
                    SystemMetricsCollector::Get().Initialize(adapterDesc.AdapterLuid.LowPart,
                                                             adapterDesc.AdapterLuid.HighPart);
                }
                adapter->Release();
            }
            dxgiDevice->Release();
        }

        // Cache device — GetDevice already AddRef'd, so we keep that ref
        dx11_hook_g_pd3dDevice = device11;
        // Cache context with AddRef
        dx11_hook_g_pd3dDevice->GetImmediateContext(&dx11_hook_g_pd3dDeviceContext);
    }

    ID3D11Device* device = dx11_hook_g_pd3dDevice;
    ID3D11DeviceContext* context = dx11_hook_g_pd3dDeviceContext;
    if (dx11_hook_g_D3D11IdentitySwapChain != pSwapChain) {
        ID3D11Device* identityDevice = nullptr;
        if (SUCCEEDED(pSwapChain->GetDevice(IID_PPV_ARGS(&identityDevice))) && identityDevice) {
            if (dx11_hook_g_D3D11IdentityDevice)
                dx11_hook_g_D3D11IdentityDevice->Release();
            dx11_hook_g_D3D11IdentityDevice = identityDevice;
            dx11_hook_g_D3D11IdentitySwapChain = pSwapChain;
        }
    }
    ID3D11Device* activeIdentityDevice =
        (dx11_hook_g_D3D11IdentitySwapChain == pSwapChain && dx11_hook_g_D3D11IdentityDevice) ? dx11_hook_g_D3D11IdentityDevice : device;
    DX11Hook_RegisterDeviceIdentity(activeIdentityDevice, "active D3D11 swapchain device");
    const unsigned d3d11Minor = ResolveD3D11MinorUse(activeIdentityDevice);
    const std::string& apiLabel = ResolveDX11OverlayApiLabel(d3d11Minor);

    if (g_OverlayAdapter.IsInitialized() && currentHwnd != dx11_hook_g_CachedHwnd) {
        HookLog("DX11: HWND changed, shutting down OverlayAdapter");
        g_OverlayAdapter.Shutdown();
    }

    if (!g_OverlayAdapter.IsInitialized() || currentHwnd != dx11_hook_g_CachedHwnd) {
        if (g_OverlayAdapter.IsInitialized()) {
            g_OverlayAdapter.Shutdown();
        }
        dx11_hook_g_CachedHwnd = currentHwnd;
        lastHwnd = currentHwnd;

        g_OverlayAdapter.SetHwnd(currentHwnd);

        if (g_OverlayAdapter.InitDX11(device, context)) {
            g_OverlayAdapter.SetHwnd(currentHwnd);
            EarlyLog("DX11: OverlayAdapter initialized for HWND %p", currentHwnd);
        } else {
            EarlyLog("DX11: OverlayAdapter::InitDX11 FAILED for HWND %p", currentHwnd);
        }
    }

    // Detect HWND change (multi-window apps)
    if (currentHwnd != lastHwnd) {
        EarlyLog(
            "DX11: HWND changed from %p to %p. Overlay might not be visible "
            "on new window without re-init.",
            lastHwnd, currentHwnd);
        lastHwnd = currentHwnd;
    }

    const auto presentationEncoding =
        DXGIShared::ResolveSwapChainPresentationEncoding(pSwapChain, desc.BufferDesc.Format);
    const bool isHDR = ce::presentation_color::IsHDR(presentationEncoding);
    g_OverlayAdapter.SetHDR(isHDR, (int)desc.BufferDesc.Format);

    // Propagate HDR state to media engine via shared memory
    if (SharedMemoryLayout* sharedMemory = GetHookSharedMemory()) {
        sharedMemory->SetIsHDR(isHDR);
    }

    g_OverlayAdapter.SetMetrics(DXGIShared::GetPerformanceMetrics());
    g_OverlayAdapter.SetIPCClient(g_IPC);
    g_OverlayAdapter.SetDroppedFrames(dx11_hook_g_DX11Capture.droppedFrames.load(std::memory_order_relaxed));
    g_OverlayAdapter.SetGraphicsAPI(apiLabel.c_str(), "active D3D11 swapchain device");

    if (interposerPrivateOutputChain && dx11_hook_g_overlayViews.Count() != 0) {
        // The retained views belong to whichever chain CE drew on before the
        // interposer took over presentation. Holding them keeps that chain's back
        // buffers alive for as long as the interposer route lasts, which is
        // exactly the pin this route exists to avoid.
        EarlyLog("%s: Releasing %zu retained swapchain RTV(s) — this Present is on a present interposer's private chain",
                 dx11_hook_g_DetectedAPI, dx11_hook_g_overlayViews.Count());
        dx11_hook_g_overlayViews.Clear([](ID3D11RenderTargetView* view) { view->Release(); });
    }
    dx11_hook_g_mainRenderTargetView = nullptr;

    ID3D11RenderTargetView* overlayRTV = nullptr;
    bool usingBoundRTV = false;
    // Set when the RTV below belongs to this Present alone and must be released
    // before returning, instead of living in dx11_hook_g_mainRenderTargetView.
    ID3D11RenderTargetView* transientOverlayRTV = nullptr;

    // Outside an interposer's private chain, a render target the game left
    // bound at Present is the frame it just finished, which is a better overlay
    // target than a guessed buffer index on a FLIP chain. On an interposer's
    // private chain it is one of the interpolator's own intermediates, so
    // drawing there writes the overlay into driver-internal state rather than
    // onto the frame.
    if (nvPresentLoaded && context &&
        ce::overlay_compat::MayAdoptBoundRenderTargetAsOverlayTarget(interposerPrivateOutputChain)) {
        context->OMGetRenderTargets(1, &overlayRTV, NULL);
        if (overlayRTV) {
            usingBoundRTV = true;
        }
    }

    if (!usingBoundRTV) {
        // The application's back buffer (kDX11ApplicationBackBuffer), which is
        // also what CaptureFrame reads, so the recording carries the overlay.
        // Its view is retained per swapchain (BackBufferViewCache).

        // A retained RTV pins one of the swapchain's back buffers. That is fine
        // on a chain whose ResizeBuffers CE hooks (CleanupDX11Resources drops
        // everything first), and it is NOT fine on an interposer's private
        // chain: that one is hooked presentOnly, so the interposer recreates it
        // through a ResizeBuffers CE never observes while CE still holds a
        // reference to the old back buffer. Build and drop the view inside this
        // Present there instead.
        const bool retainRenderTargetView = !interposerPrivateOutputChain;
        if (retainRenderTargetView)
            dx11_hook_g_mainRenderTargetView = dx11_hook_g_overlayViews.Find(pSwapChain, kDX11ApplicationBackBuffer);
        if (!retainRenderTargetView || !dx11_hook_g_mainRenderTargetView) {
            ID3D11Texture2D* backbuffer = nullptr;
            HRESULT hr = pSwapChain->GetBuffer(kDX11ApplicationBackBuffer, IID_PPV_ARGS(&backbuffer));
            const bool gotBackBuffer = SUCCEEDED(hr) && backbuffer;
            ID3D11RenderTargetView* createdRTV = nullptr;
            if (gotBackBuffer) {
                hr = device->CreateRenderTargetView(backbuffer, NULL, &createdRTV);
                backbuffer->Release();
            }
            // Once per change, not per Present: an interposer's chain builds a view on every Present,
            // and a refused back buffer wrote two lines per Present (DXUP, 20261022_162106).
            static ce::log_meter::ChangeGate s_viewGate;
            const auto verdict = s_viewGate.Observe(ce::log_meter::FieldKey(
                pSwapChain, desc.BufferDesc.Width, desc.BufferDesc.Height, retainRenderTargetView, gotBackBuffer, hr));
            if (FAILED(hr) || !createdRTV) {
                if (verdict) {
                    EarlyLog("%s: %s FAILED for the overlay view of SwapChain %p (%ux%u) hr=0x%08X - no overlay on "
                             "this Present%s",
                             dx11_hook_g_DetectedAPI, gotBackBuffer ? "CreateRenderTargetView" : "GetBuffer(0)",
                             pSwapChain, desc.BufferDesc.Width, desc.BufferDesc.Height, static_cast<unsigned>(hr),
                             ce::log_meter::SuppressedNote(verdict.suppressed).c_str());
                }
                return;
            }
            if (retainRenderTargetView &&
                dx11_hook_g_overlayViews.Store(pSwapChain, kDX11ApplicationBackBuffer, createdRTV,
                                               [](ID3D11RenderTargetView* view) { view->Release(); })) {
                dx11_hook_g_mainRenderTargetView = createdRTV;
            } else {
                transientOverlayRTV = createdRTV;
            }
            if (verdict) {
                EarlyLog("%s: %s overlay RTV created for SwapChain %p (%ux%u, retained views=%zu)%s",
                         dx11_hook_g_DetectedAPI, retainRenderTargetView ? "retained" : "transient", pSwapChain,
                         desc.BufferDesc.Width, desc.BufferDesc.Height, dx11_hook_g_overlayViews.Count(),
                         ce::log_meter::SuppressedNote(verdict.suppressed).c_str());
            }
        }

        overlayRTV = dx11_hook_g_mainRenderTargetView ? dx11_hook_g_mainRenderTargetView : transientOverlayRTV;
    }

    // Every early return from here on must drop the transient view; nothing CE
    // creates on an interposer's chain may outlive this Present.
    auto transientRTVGuard = ce::make_scope_guard([&transientOverlayRTV]() {
        if (transientOverlayRTV) {
            transientOverlayRTV->Release();
            transientOverlayRTV = nullptr;
        }
    });

    // Once per change of device, context or swapchain (and every 10000 frames), not per frame:
    // BioShock Infinite wrote two lines per frame here, 92% of its hook log (20261017_000225).
    // Not keyed by the view: a FLIP chain's views alternate every Present.
    static ce::log_meter::ChangeGate s_preRenderGate;
    if (const auto verdict = s_preRenderGate.ObserveOrEvery(ce::log_meter::FieldKey(device, context, pSwapChain),
                                                            static_cast<uint32_t>(frameCount), 10000)) {
        EarlyLog("DX11: [frame %d] pre-render device=%p context=%p rtv=%p%s", frameCount, device, context, overlayRTV,
                 ce::log_meter::SuppressedNote(verdict.suppressed).c_str());
    }

    // The backend binds the target and a full-size viewport inside its own save and restore of the
    // application's state, all eight render targets included. This path used to save and rebind
    // the first one only, unbinding the others, and to save the viewports a second time.
    g_OverlayAdapter.SetDX11RenderTarget(overlayRTV);
    auto renderTargetGuard = ce::make_scope_guard([]() { g_OverlayAdapter.SetDX11RenderTarget(nullptr); });

    if (frameCount <= 3) {
        EarlyLog("DX11: [frame %d] calling RenderOverlay", frameCount);
    }

    // Render Custom Overlay
    // The overlay's GPU time for the perf CSV (overlay_gpu_us), only while it is written.
    const bool timeGpu = PerfLogger::Get().IsEnabled();
    if (timeGpu) {
        ce::dx11_overlay_gpu::BeginOverlay(device, context);
    }
    dx11_hook_g_InOverlayRender = true;
    // NOLINTNEXTLINE(bugprone-narrowing-conversions) - intentional narrowing; value is range-bounded by the surrounding API/geometry contract
    g_OverlayAdapter.RenderOverlay(desc.BufferDesc.Width, desc.BufferDesc.Height);
    dx11_hook_g_InOverlayRender = false;
    if (timeGpu) {
        ce::dx11_overlay_gpu::EndOverlay(context);
    }

    if (usingBoundRTV && overlayRTV) {
        overlayRTV->Release();
    }
}

#ifdef CE_FLOW_TEST
// The D3D11 overlay's API label, and its draw on one Present of `swapChain`.
extern "C" __declspec(dllexport) void CEFlow_DX11OverlayApiLabel(unsigned d3d11Minor, char* out, size_t size) {
    if (out && size)
        snprintf(out, size, "%s", ResolveDX11OverlayApiLabel(d3d11Minor).c_str());
}
extern "C" __declspec(dllexport) void CEFlow_DrawDX11Overlay(IDXGISwapChain* swapChain) {
    DrawDX11Overlay(swapChain);
}
extern "C" __declspec(dllexport) size_t CEFlow_DX11OverlayRetainedViews() {
    return dx11_hook_g_overlayViews.Count();
}
extern "C" __declspec(dllexport) void CEFlow_CleanupDX11Resources() {
    CleanupDX11Resources(true);
}
#endif
