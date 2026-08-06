#include "dx12_hook_internal.h"


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
