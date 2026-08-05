#include "dx12_hook_internal.h"


// This hooks the factory vtable directly in the DXGI module
void InstallGlobalVTableHooks() {
HookLog("DX12: InstallGlobalVTableHooks called");

// CRITICAL: Install global factory vtable hooks to catch swapchain creation
// even for factories created before our IAT hooks were installed.
// This ensures ALL swapchains get wrapped regardless of timing.

HMODULE hDXGI = GetModuleHandleA("dxgi.dll");
if (!hDXGI) {
    HookLog("DX12: DXGI module not loaded, skipping factory vtable hooks");
    return;
}

// Get CreateDXGIFactory1 export to create a temp factory
typedef HRESULT(WINAPI * PFN_CreateDXGIFactory1)(REFIID, void**);
PFN_CreateDXGIFactory1 pCreateFactory = (PFN_CreateDXGIFactory1)GetProcAddress(hDXGI, "CreateDXGIFactory1");
if (!pCreateFactory) {
    HookLog("DX12: CreateDXGIFactory1 not found");
    return;
}

// Create a temp factory to get its vtable
IDXGIFactory2* pFactory = nullptr;
HRESULT hr = pCreateFactory(IID_PPV_ARGS(&pFactory));
if (FAILED(hr) || !pFactory) {
    HookLog("DX12: Failed to create temp factory for vtable extraction");
    return;
}

// Get the vtable - ALL IDXGIFactory instances share this vtable
void** vtable = *(void***)pFactory;
HookLog("DX12: Factory vtable at %p", vtable);

// Save the real CreateSwapChainForHwnd address BEFORE vtable patching
void* realCreateSCForHwndAddr = vtable[15];
dx12_hook_s_realCreateSCForHwndAddr = realCreateSCForHwndAddr;

// Hook CreateSwapChain (vtable[10] for IDXGIFactory)
// Hook CreateSwapChainForHwnd (vtable[15] for IDXGIFactory2)
if (VTableHook::Create(reinterpret_cast<void*>(&vtable[10]), (LPVOID)DetourCreateSwapChainGlobal, (LPVOID*)&dx12_hook_oCreateSwapChainGlobal)) {
    HookLog("DX12: Hooked global CreateSwapChain at vtable[10]");
}

if (VTableHook::Create(reinterpret_cast<void*>(&vtable[15]), (LPVOID)DetourCreateSwapChainForHwndGlobal,
                       (LPVOID*)&dx12_hook_oCreateSwapChainForHwndGlobal)) {
    HookLog("DX12: Hooked global CreateSwapChainForHwnd at vtable[15]");
}

pFactory->Release();

// Also hook IDXGIFactory4 and IDXGIFactory6 vtables to catch games that
// QueryInterface for higher factory versions (different vtable pointers).
// CreateSwapChainForHwnd is at the same slot (15) in all factory versions
// because IDXGIFactory4 inherits from IDXGIFactory3 → IDXGIFactory2.
IDXGIFactory4* pFactory4 = nullptr;
if (SUCCEEDED(pCreateFactory(IID_PPV_ARGS(&pFactory4)))) {
    void** vtable4 = *(void***)pFactory4;
    HookLog("DX12: IDXGIFactory4 available, vtable=%p (IDXGIFactory2=%p, same=%d)", vtable4, vtable,
            (int)(vtable4 == vtable));
    if (vtable4 != vtable) {  // Different vtable pointer
        VTableHook::Create(reinterpret_cast<void*>(&vtable4[10]), (LPVOID)DetourCreateSwapChainGlobal, nullptr);
        VTableHook::Create(reinterpret_cast<void*>(&vtable4[15]), (LPVOID)DetourCreateSwapChainForHwndGlobal, nullptr);
        HookLog("DX12: Hooked IDXGIFactory4 vtable[10] and vtable[15]");
    }
    pFactory4->Release();
} else {
    HookLog("DX12: IDXGIFactory4 not available");
}

IDXGIFactory6* pFactory6 = nullptr;
if (SUCCEEDED(pCreateFactory(IID_PPV_ARGS(&pFactory6)))) {
    void** vtable6 = *(void***)pFactory6;
    HookLog("DX12: IDXGIFactory6 available, vtable=%p (IDXGIFactory2=%p, same=%d)", vtable6, vtable,
            (int)(vtable6 == vtable));
    if (vtable6 != vtable) {  // Different vtable pointer
        VTableHook::Create(reinterpret_cast<void*>(&vtable6[10]), (LPVOID)DetourCreateSwapChainGlobal, nullptr);
        VTableHook::Create(reinterpret_cast<void*>(&vtable6[15]), (LPVOID)DetourCreateSwapChainForHwndGlobal, nullptr);
        HookLog("DX12: Hooked IDXGIFactory6 vtable[10] and vtable[15]");
    }
    pFactory6->Release();
} else {
    HookLog("DX12: IDXGIFactory6 not available");
}

// Install inline hook on CreateSwapChainForHwnd in dxgi.dll.
// VTable hooks only patch a single vtable and miss calls through
// Streamline's SL proxy factory (different COM vtable). Inline hooks
// patch the actual function code and catch ALL callers.
if (realCreateSCForHwndAddr && !dx12_hook_s_oCreateSCForHwndInline) {
    void* trampoline = nullptr;
    if (InlineHook::Install(realCreateSCForHwndAddr, (void*)DetourCreateSwapChainForHwndInline, &trampoline)) {
        dx12_hook_s_oCreateSCForHwndInline = (PFN_CreateSwapChainForHwnd)trampoline;
        HookLog("DX12: Installed INLINE hook on CreateSwapChainForHwnd at %p", realCreateSCForHwndAddr);
    } else {
        HookLog("DX12: FAILED to install inline hook on CreateSwapChainForHwnd");
    }
}

// Install DEEP hook on CreateSwapChainForHwnd.
// When Streamline hooks CreateSwapChainForHwnd at byte 0 and uses a saved
// trampoline for internal calls (bypassing both our vtable and inline hooks),
// the deep hook patches the function body past Streamline's JMP so ALL
// callers are intercepted — including Streamline's linkSwapchainToCmdQueue.
// The full wrapper pre-releases stale swapchains AND post-tracks new ones,
// ensuring SL's shadow swapchains are tracked for subsequent releases.
if (realCreateSCForHwndAddr) {
    void* trampoline = InlineHook::InstallDeepHook(realCreateSCForHwndAddr, (void*)DeepHookCreateSwapChainForHwnd);
    if (trampoline) {
        dx12_hook_s_deepHookTrampoline = (PFN_CreateSwapChainForHwnd)trampoline;
        HookLog("DX12: Installed DEEP hook on CreateSwapChainForHwnd at %p (trampoline=%p)",
                realCreateSCForHwndAddr, trampoline);
    } else {
        HookLog("DX12: Deep hook not needed or failed for CreateSwapChainForHwnd");
    }
}

HookLog("DX12: Global factory vtable hooks installed");
}


// Install inline hooks on Present/Present1 via temp swapchain creation.
// Inline hooks patch the function code in memory, creating a trampoline that
// bypasses the hook entirely. This solves the re-entry problem with vtable
// hooks. presentOnly: if true, only install Present hooks (defer ResizeBuffers
// for Strange Brigade)
void HookSwapchainVTableViaTempSwapchain(bool presentOnly) {
HMODULE hDXGI = GetModuleHandleA("dxgi.dll");
HMODULE hD3D12 = GetModuleHandleA("d3d12.dll");
if (!hDXGI || !hD3D12)
    return;

typedef HRESULT(WINAPI * PFN_CreateDXGIFactory1)(REFIID, void**);
typedef HRESULT(WINAPI * PFN_D3D12CreateDevice)(IUnknown*, D3D_FEATURE_LEVEL, REFIID, void**);

PFN_CreateDXGIFactory1 pCreateFactory = (PFN_CreateDXGIFactory1)GetProcAddress(hDXGI, "CreateDXGIFactory1");
PFN_D3D12CreateDevice pD3D12CreateDevice = (PFN_D3D12CreateDevice)GetProcAddress(hD3D12, "D3D12CreateDevice");
if (!pCreateFactory || !pD3D12CreateDevice)
    return;

IDXGIFactory2* pFactory = nullptr;
if (FAILED(pCreateFactory(IID_PPV_ARGS(&pFactory))) || !pFactory)
    return;

ID3D12Device* pDevice = nullptr;
if (FAILED(pD3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&pDevice))) || !pDevice) {
    pFactory->Release();
    return;
}

// Hook CreateSampler on the device vtable
// All D3D12 devices share the same vtable, so this hooks ALL devices
DX12_HookDeviceVTable(pDevice);

D3D12_COMMAND_QUEUE_DESC queueDesc = {};
queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
ID3D12CommandQueue* pQueue = nullptr;
if (FAILED(pDevice->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&pQueue))) || !pQueue) {
    pDevice->Release();
    pFactory->Release();
    return;
}

// Create a minimal hidden window
WNDCLASSEXW wc = {sizeof(wc)};
wc.lpfnWndProc = DefWindowProcW;
wc.hInstance = GetModuleHandleW(nullptr);
wc.lpszClassName = L"CE_Temp";
RegisterClassExW(&wc);

HWND hwnd = CreateWindowExW(0, L"CE_Temp", L"", WS_POPUP, 0, 0, 2, 2, nullptr, nullptr, wc.hInstance, nullptr);

// Create temp swapchain
DXGI_SWAP_CHAIN_DESC1 scd = {};
scd.Width = 2;
scd.Height = 2;
scd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
scd.SampleDesc.Count = 1;
scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
scd.BufferCount = 2;
scd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;

// Mark that we're creating a temp swapchain for hook installation.
// This prevents the CreateSwapChainForHwnd hooks from capturing the temp
// queue as g_SwapchainQueue or tracking the temp swapchain.
dx12_hook_g_CreatingTempSwapchain.store(true, std::memory_order_release);

IDXGISwapChain1* pSwapChain = nullptr;
HRESULT hr = E_FAIL;

// CRITICAL: Call the ORIGINAL CreateSwapChainForHwnd to get an unwrapped
// swapchain We must use oCreateSwapChainForHwndGlobal directly to bypass our
// wrapper If the original is not available, skip vtable hook installation
if (dx12_hook_oCreateSwapChainForHwndGlobal) {
    // Call original directly - bypasses our wrapper
    hr = dx12_hook_oCreateSwapChainForHwndGlobal(pFactory, pQueue, hwnd, &scd, nullptr, nullptr, &pSwapChain);
    if (SUCCEEDED(hr) && pSwapChain) {
        HookLog(
            "DX12: Created temp swapchain via original "
            "CreateSwapChainForHwnd (unwrapped)");
    }
} else {
    HookLog(
        "DX12: oCreateSwapChainForHwndGlobal not available, skipping "
        "Present vtable hooks");
}

dx12_hook_g_CreatingTempSwapchain.store(false, std::memory_order_release);

if (SUCCEEDED(hr) && pSwapChain) {
    HookLog("DX12: Installing Present inline hooks via temp swapchain");
    if (DXGIShared::InstallPresentInlineHooks(pSwapChain)) {
        HookLog("DX12: Present inline hooks installed successfully");
    } else {
        HookLog("DX12: Failed to install Present inline hooks");
    }
    pSwapChain->Release();
} else {
    HookLog("DX12: Failed to create temp swapchain (hr=0x%08X)", hr);
}

// Hook ExecuteCommandLists on the temp queue's vtable.
// All DX12 command queues share the same vtable, so this hooks ALL queues
// (including the game's pre-existing queue). When ECL fires, it calls
// DX12_SetCommandQueue which captures the game's actual queue pointer.
DX12_HookQueueVTable(pQueue);

// Cleanup
if (hwnd)
    DestroyWindow(hwnd);
UnregisterClassW(L"CE_Temp", wc.hInstance);
pQueue->Release();
pDevice->Release();
pFactory->Release();
}


void DrawOverlay(ID3D12GraphicsCommandList* cmdList, bool isRealFrame, UINT bufferIdx, D3D12_CPU_DESCRIPTOR_HANDLE* rtvOverride) {
// CRITICAL FIX: Lock mutex to prevent concurrent access during overlay
// shutdown/reinit
std::lock_guard<std::recursive_mutex> lock(dx12_hook_g_OverlayMutex);

if (!dx12_hook_g_State.overlayInit || !cmdList)
    return;

static std::atomic<int> s_drawOverlayLogCount{0};
const bool logThisDraw = s_drawOverlayLogCount.fetch_add(1, std::memory_order_relaxed) < 10;
if (logThisDraw) {
    HookLogImportant(
        "DX12: DrawOverlay begin (cmdList=%p, bufferIdx=%u, realFrame=%d, overlayInit=%d, syncInit=%d)", cmdList,
        bufferIdx, isRealFrame ? 1 : 0, dx12_hook_g_State.overlayInit ? 1 : 0, dx12_hook_g_State.syncInit ? 1 : 0);
}

// CRITICAL FIX: Always set IPC client regardless of frame type.
// RenderOverlay() guards on ipc being non-null, so if this was only set
// on real frames, overlay would never render when isRealFrame is false.
g_OverlayAdapter.SetIPCClient(g_IPC);
g_OverlayAdapter.SetReserveInactiveFGSpace(ShouldReserveInactiveFGOverlaySpaceNow());
const auto metricsBinding = ce::dx12_overlay_policy::DecideOverlayMetricsBinding(isRealFrame);
if (metricsBinding.bindMetrics) {
    g_OverlayAdapter.SetMetrics(DXGIShared::GetPerformanceMetrics());
}

if (metricsBinding.refreshFrameMetadata) {
    static const bool s_isVKD3D = []() {
        return GetModuleHandleA("d3d12core.dll") &&
               (GetModuleHandleA("libvkd3d-1.dll") || GetModuleHandleA("vkd3d.dll"));
    }();
    const char* api = s_isVKD3D ? "DX12 (VKD3D)" : "DX12";
    g_OverlayAdapter.SetGraphicsAPI(api);
    // HDR state is set during overlay init (ProcessFrame) by querying the
    // display output's actual color space — not here, to avoid the false
    // positive of R10G10B10A2_UNORM being treated as HDR in SDR mode.
}

// Set Render Target for Custom Overlay
// When rtvOverride is set (offscreen compositing path), use it instead of the backbuffer RTV.
D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle;
if (rtvOverride) {
    rtvHandle = *rtvOverride;
} else {
    // CRITICAL FIX: Add null check for rtvDescHeap to prevent crash
    if (!dx12_hook_g_State.rtvDescHeap) {
        HookLog("DrawOverlay: rtvDescHeap is null, skipping overlay");
        return;
    }
    rtvHandle = dx12_hook_g_State.rtvDescHeap->GetCPUDescriptorHandleForHeapStart();
    rtvHandle.ptr += bufferIdx * dx12_hook_g_State.rtvDescriptorSize;
}

g_OverlayAdapter.SetDX12RenderTarget(cmdList, (void*)rtvHandle.ptr);
g_OverlayAdapter.SetDX12UploadSlotFence(
    dx12_hook_g_State.fence, ce::dx12_overlay_policy::DecideOverlayUploadSlotGuardValue(
                       DXGIShared::g_StreamlineFGRunning.load(std::memory_order_acquire) || g_FGCompat.IsFGActive(),
                       dx12_hook_g_State.fence != nullptr, dx12_hook_g_State.currentFenceValue));

// Render overlay content
g_OverlayAdapter.RenderOverlay(dx12_hook_g_State.cachedWidth, dx12_hook_g_State.cachedHeight);
if (logThisDraw) {
    HookLogImportant("DX12: DrawOverlay end (bufferIdx=%u)", bufferIdx);
}
}


// Ensure offscreen render target exists and matches backbuffer dimensions/format.
// Used for the copy-render-copy overlay compositing path that avoids
// OMSetRenderTargets(swapchain) + SetDescriptorHeaps GPU pipeline stalls.
bool EnsureOffscreenRT(ID3D12Device* device, UINT width, UINT height, DXGI_FORMAT format) {
if (dx12_hook_g_State.offscreenRT && dx12_hook_g_State.offscreenWidth == width && dx12_hook_g_State.offscreenHeight == height &&
    dx12_hook_g_State.offscreenFormat == format) {
    return true;
}

// Release old resources if dimensions/format changed
if (dx12_hook_g_State.offscreenRT) {
    dx12_hook_g_State.offscreenRT->Release();
    dx12_hook_g_State.offscreenRT = nullptr;
}
if (dx12_hook_g_State.offscreenRtvHeap) {
    dx12_hook_g_State.offscreenRtvHeap->Release();
    dx12_hook_g_State.offscreenRtvHeap = nullptr;
}

// Create RTV descriptor heap for offscreen target
D3D12_DESCRIPTOR_HEAP_DESC rtvDesc = {};
rtvDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
rtvDesc.NumDescriptors = 1;
HRESULT hr = device->CreateDescriptorHeap(&rtvDesc, IID_PPV_ARGS(&dx12_hook_g_State.offscreenRtvHeap));
if (FAILED(hr)) {
    HookLog("DX12: Failed to create offscreen RTV heap hr=0x%08X", hr);
    return false;
}

// NOLINTNEXTLINE(bugprone-invalid-enum-default-initialization) - zero-initialized placeholder; enum fields are assigned before use
D3D12_HEAP_PROPERTIES heapProps = {};
heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;
D3D12_RESOURCE_DESC resDesc = {};
resDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
resDesc.Width = width;
resDesc.Height = height;
resDesc.DepthOrArraySize = 1;
resDesc.MipLevels = 1;
resDesc.Format = format;
resDesc.SampleDesc.Count = 1;
resDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

D3D12_CLEAR_VALUE clearVal = {};
clearVal.Format = format;

hr = device->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &resDesc, D3D12_RESOURCE_STATE_COMMON,
                                     &clearVal, IID_PPV_ARGS(&dx12_hook_g_State.offscreenRT));
if (FAILED(hr)) {
    HookLog("DX12: Failed to create offscreen RT %ux%u fmt=%d hr=0x%08X", width, height, format, hr);
    dx12_hook_g_State.offscreenRtvHeap->Release();
    dx12_hook_g_State.offscreenRtvHeap = nullptr;
    return false;
}

device->CreateRenderTargetView(dx12_hook_g_State.offscreenRT, nullptr,
                               dx12_hook_g_State.offscreenRtvHeap->GetCPUDescriptorHandleForHeapStart());

dx12_hook_g_State.offscreenRT->SetName(L"CE_OverlayOffscreenRT");

dx12_hook_g_State.offscreenWidth = width;
dx12_hook_g_State.offscreenHeight = height;
dx12_hook_g_State.offscreenFormat = format;

HookLogImportant("DX12: Created offscreen RT %ux%u fmt=%d for overlay compositing", width, height, format);
return true;
}


bool PrewarmPostSLOverlayForFreshStreamlineHandoff(IDXGISwapChain* swapChain, ID3D12CommandQueue* swapchainQueue, const char* context) {
if (!swapChain || !swapchainQueue) {
    return false;
}

DXGI_SWAP_CHAIN_DESC desc = {};
const HRESULT descHr = swapChain->GetDesc(&desc);
if (FAILED(descHr) || desc.BufferCount == 0 || desc.BufferCount > 8) {
    HookLogImportant(
        "DX12: PostSL handoff prewarm refused invalid swapchain description "
        "(source=%s sc=%p queue=%p hr=0x%08X buffers=%u)",
        context ? context : "unknown", swapChain, swapchainQueue, (unsigned)descHr, desc.BufferCount);
    return false;
}

ID3D12Device* queueDevice = nullptr;
const HRESULT deviceHr = swapchainQueue->GetDevice(IID_PPV_ARGS(&queueDevice));
IDXGISwapChain3* swapChain3 = nullptr;
const HRESULT sc3Hr = swapChain->QueryInterface(IID_PPV_ARGS(&swapChain3));
if (FAILED(deviceHr) || !queueDevice || FAILED(sc3Hr) || !swapChain3) {
    HookLogImportant(
        "DX12: PostSL handoff prewarm missing exact queue/swapchain prerequisites "
        "(source=%s sc=%p queue=%p deviceHr=0x%08X sc3Hr=0x%08X)",
        context ? context : "unknown", swapChain, swapchainQueue, (unsigned)deviceHr, (unsigned)sc3Hr);
    if (swapChain3) {
        swapChain3->Release();
    }
    if (queueDevice) {
        queueDevice->Release();
    }
    return false;
}

const ULONGLONG startedMs = GetTickCount64();
bool ready = false;
bool overlayInit = false;
bool syncInit = false;
ID3D12DescriptorHeap* rtvHeap = nullptr;
{
    std::lock_guard<std::recursive_mutex> overlayLock(dx12_hook_g_OverlayMutex);
    const HRESULT deviceReason = queueDevice->GetDeviceRemovedReason();
    if (SUCCEEDED(deviceReason)) {
        // The adapter owns only device/format-scoped objects. Reuse it even when the Streamline proxy uses a
        // different queue, then create the new swapchain RTVs and allocator/fence set without recording or
        // submitting an overlay draw. This completes before slDLSSGSetOptions(ON), so the first generated
        // Present cannot race a backend shutdown/rebuild.
        dx12_hook_g_PreserveOverlayAdapterAcrossResize.store(g_OverlayAdapter.IsInitialized(), std::memory_order_release);
        // NOLINTNEXTLINE(bugprone-narrowing-conversions) - intentional narrowing; value is range-bounded by the surrounding API/geometry contract
        dx12_hook_g_State.cachedWidth = desc.BufferDesc.Width;
        // NOLINTNEXTLINE(bugprone-narrowing-conversions) - intentional narrowing; value is range-bounded by the surrounding API/geometry contract
        dx12_hook_g_State.cachedHeight = desc.BufferDesc.Height;
        dx12_hook_g_State.format = desc.BufferDesc.Format;

        const bool backendReady =
            InitImGui(queueDevice, static_cast<int>(desc.BufferCount), desc.BufferDesc.Format, desc.OutputWindow);
        if (backendReady) {
            CreateRTVs(queueDevice, swapChain3, static_cast<int>(desc.BufferCount));
            if (dx12_hook_g_State.rtvDescHeap) {
                InitOverlaySync(queueDevice, static_cast<int>(desc.BufferCount), swapchainQueue);
            }
        }
        ready = backendReady && dx12_hook_g_State.overlayInit && dx12_hook_g_State.syncInit && dx12_hook_g_State.rtvDescHeap && dx12_hook_g_State.cmdList &&
                !dx12_hook_g_State.allocators.empty();
        if (!ready && !dx12_hook_g_State.rtvDescHeap) {
            // Make the normal first-PostSL bootstrap retry the complete swapchain-scoped setup. Preserve the
            // warm adapter if initialization itself succeeded; InitImGui will reuse it on that retry.
            dx12_hook_g_State.overlayInit = false;
            dx12_hook_g_PreserveOverlayAdapterAcrossResize.store(g_OverlayAdapter.IsInitialized(), std::memory_order_release);
        }
        overlayInit = dx12_hook_g_State.overlayInit;
        syncInit = dx12_hook_g_State.syncInit;
        rtvHeap = dx12_hook_g_State.rtvDescHeap;
    } else {
        HookLogImportant(
            "DX12: PostSL handoff prewarm refused removed device "
            "(source=%s device=%p hr=0x%08X)",
            context ? context : "unknown", queueDevice, (unsigned)deviceReason);
    }
}

HookLogImportant(
    "DX12: PostSL handoff prewarm %s before DLSS enable "
    "(source=%s sc=%p queue=%p device=%p fmt=%d buffers=%u elapsed=%llums init=%d sync=%d rtv=%p)",
    ready ? "READY" : "INCOMPLETE", context ? context : "unknown", swapChain, swapchainQueue, queueDevice,
    static_cast<int>(desc.BufferDesc.Format), desc.BufferCount,
    static_cast<unsigned long long>(GetTickCount64() - startedMs), overlayInit ? 1 : 0, syncInit ? 1 : 0, rtvHeap);

swapChain3->Release();
queueDevice->Release();
return ready;
}


// --- CPU Prerender Limit Support (DX12) ---
void ApplyPrerenderLimitDX12(float limit, bool frameGenerationPresentationActive) {
if (limit < 0.0f)
    return;
// FG runtimes can replace the observed ECL queue with their own internal
// presentation queue. Keep the limiter on the retained application queue
// so its fence stream cannot become part of a runtime Present dependency.
bool usesOriginalGameQueue = false;
ID3D12CommandQueue* currentQueueSnapshot = nullptr;
DX12Context ctx = GetDX12PrerenderContext(frameGenerationPresentationActive, &usesOriginalGameQueue,
                                          &currentQueueSnapshot);
if (!ctx.IsValid())
    return;

std::lock_guard<std::mutex> lock(dx12_hook_g_PrerenderMutex);

if (dx12_hook_g_PrerenderDevice != ctx.device || dx12_hook_g_PrerenderQueue != ctx.queue) {
    for (auto* fence : dx12_hook_g_PrerenderFences) {
        if (fence)
            fence->Release();
    }
    dx12_hook_g_PrerenderFences.clear();
    for (HANDLE event : dx12_hook_g_PrerenderEvents) {
        if (event)
            CloseHandle(event);
    }
    dx12_hook_g_PrerenderEvents.clear();
    dx12_hook_g_PrerenderFrameIndex = 0;
    if (dx12_hook_g_PrerenderDevice)
        dx12_hook_g_PrerenderDevice->Release();
    if (dx12_hook_g_PrerenderQueue)
        dx12_hook_g_PrerenderQueue->Release();
    dx12_hook_g_PrerenderDevice = ctx.device;
    dx12_hook_g_PrerenderQueue = ctx.queue;
    dx12_hook_g_PrerenderDevice->AddRef();
    dx12_hook_g_PrerenderQueue->AddRef();
    HookLogImportant(
        "DX12: Prerender fence stream rebound device=%p queue=%p role=%s currentQueue=%p",
        ctx.device, ctx.queue, usesOriginalGameQueue ? "original-game" : "current-fallback",
        currentQueueSnapshot);
}

// Initialize fence ring buffer if needed
if (dx12_hook_g_PrerenderFences.empty()) {
    for (int i = 0; i < 16; i++) {
        ID3D12Fence* fence = nullptr;
        HANDLE event = CreateEvent(nullptr, FALSE, FALSE, nullptr);
        if (SUCCEEDED(ctx.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)))) {
            dx12_hook_g_PrerenderFences.push_back(fence);
            dx12_hook_g_PrerenderEvents.push_back(event);
        } else if (event) {
            CloseHandle(event);
        }
    }
    HookLog("DX12: Created prerender limit fence ring buffer (size: %d)", (int)dx12_hook_g_PrerenderFences.size());
}

if (dx12_hook_g_PrerenderFences.empty())
    return;

static std::atomic<int> s_prerenderWarnLogs{0};
auto waitForFence = [&](ID3D12Fence* fenceToWait, HANDLE waitEvent, uint64_t waitValue) -> bool {
    if (!fenceToWait || !waitEvent)
        return false;
    if (fenceToWait->GetCompletedValue() >= waitValue)
        return true;

    HRESULT setHr = fenceToWait->SetEventOnCompletion(waitValue, waitEvent);
    if (FAILED(setHr)) {
        if (s_prerenderWarnLogs.fetch_add(1, std::memory_order_relaxed) < 20) {
            HookLog("DX12: Prerender SetEventOnCompletion failed hr=0x%08X value=%llu", setHr, waitValue);
        }
        return false;
    }

    DWORD waitResult = WaitForSingleObject(waitEvent, INFINITE);
    if (waitResult == WAIT_OBJECT_0)
        return true;

    if (s_prerenderWarnLogs.fetch_add(1, std::memory_order_relaxed) < 20) {
        HookLog("DX12: Prerender wait failed result=%lu error=%lu value=%llu", waitResult, GetLastError(),
                waitValue);
    }
    return false;
};

size_t idx = dx12_hook_g_PrerenderFrameIndex % dx12_hook_g_PrerenderFences.size();
ID3D12Fence* fence = dx12_hook_g_PrerenderFences[idx];
HANDLE event = dx12_hook_g_PrerenderEvents[idx];

if (limit == 0.0f) {
    // Strict Serial: Signal and immediately wait
    uint64_t value = dx12_hook_g_PrerenderFrameIndex + 1;
    HRESULT signalHr = ctx.queue->Signal(fence, value);
    if (SUCCEEDED(signalHr)) {
        waitForFence(fence, event, value);
    } else if (s_prerenderWarnLogs.fetch_add(1, std::memory_order_relaxed) < 20) {
        HookLog("DX12: Prerender signal failed hr=0x%08X value=%llu", signalHr, value);
    }
} else {
    const int lookback = std::clamp(static_cast<int>(limit), 1, 6);

    // Signal current frame
    uint64_t signalValue = dx12_hook_g_PrerenderFrameIndex + 1;
    HRESULT signalHr = ctx.queue->Signal(fence, signalValue);
    if (FAILED(signalHr)) {
        if (s_prerenderWarnLogs.fetch_add(1, std::memory_order_relaxed) < 20) {
            HookLog("DX12: Prerender signal failed hr=0x%08X value=%llu", signalHr, signalValue);
        }
        dx12_hook_g_PrerenderFrameIndex++;
        return;
    }

    // Wait on N frames ago
    if (dx12_hook_g_PrerenderFrameIndex >= (uint64_t)lookback) {
        size_t waitIdx = (dx12_hook_g_PrerenderFrameIndex - lookback) % dx12_hook_g_PrerenderFences.size();
        ID3D12Fence* waitFence = dx12_hook_g_PrerenderFences[waitIdx];
        HANDLE waitEvent = dx12_hook_g_PrerenderEvents[waitIdx];
        uint64_t waitValue = (dx12_hook_g_PrerenderFrameIndex - lookback) + 1;

        if (waitFence->GetCompletedValue() < waitValue) {
            waitForFence(waitFence, waitEvent, waitValue);
        }
    }
}

dx12_hook_g_PrerenderFrameIndex++;
}

