#include <combaseapi.h>
#include <d3d12.h>
#include <dbghelp.h>
#include <dxgi1_6.h>
#include <unknwn.h>
#include <windows.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "../../common/frame_timing.h"
#include "../../common/raii_helpers.h"
#include "../capture/shared_capture.h"
#include "../common/cached_overlay_renderer.h"
#include "../common/capture_base.h"
#include "../common/fg_detection.h"
#include "../common/hook_common.h"
#include "../common/input_manager.h"
#include "../common/streamline_compat.h"
// #include "../common/overlay.h"
#include "../common/overlay_adapter.h"
#include "../common/performance_metrics.h"

#include "../common/swapchain_wrapper.h"
#include "../common/system_metrics.h"
#include "../wrappers/d3d12_wrapper_interface.h"
#include "../wrappers/wrapper_hooks.h"
// #include "backends/imgui_impl_dx12.h"
// #include "backends/imgui_impl_win32.h"
#include "../common/freeze_watchdog.h"
#include "../common/overlay_adapter.h"
#include "dx11_hook.h"
#include "dx12_hook.h"
#include "graphics_hook.h"
// #include "imgui.h"
#include "lod_helper.h"

#include "../wrappers/vtable_hook.h"
#include "../wrappers/wrapper_base.h"
#include "dxgi_shared.h"

// Forward declaration for ImGui frame counter reset
void DX12_ResetImGuiFrameCounter();

// ============================================================================
// Crash Dump Support
// ============================================================================
static bool g_CrashDumpInitialized = false;

static std::wstring GetDumpFolderPath()
{
    wchar_t exePath[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    std::filesystem::path exeDir = std::filesystem::path(exePath).parent_path();
    std::filesystem::path dumpDir = exeDir / L"logs";
    return dumpDir.wstring();
}

static void EnsureDumpFolderExists()
{
    std::wstring dumpPath = GetDumpFolderPath();
    CreateDirectoryW(dumpPath.c_str(), nullptr);
}

static std::wstring GenerateDumpFileName()
{
    SYSTEMTIME st;
    GetSystemTime(&st);
    wchar_t name[256];
    swprintf_s(name, L"crash_%04d%02d%02d_%02d%02d%02d_%lu.dmp", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute,
               st.wSecond, GetCurrentProcessId());
    return GetDumpFolderPath() + L"\\" + name;
}

static LONG WINAPI DX12ExceptionFilter(EXCEPTION_POINTERS* pExceptionInfo)
{
    // Only handle access violations and similar serious errors
    DWORD code = pExceptionInfo->ExceptionRecord->ExceptionCode;
    if (code == EXCEPTION_ACCESS_VIOLATION || code == EXCEPTION_ILLEGAL_INSTRUCTION ||
        code == EXCEPTION_INT_DIVIDE_BY_ZERO || code == EXCEPTION_BREAKPOINT || code == 0x40000015)  // Abort
    {
        EnsureDumpFolderExists();
        std::wstring dumpPath = GenerateDumpFileName();

        HANDLE hFile =
            CreateFileW(dumpPath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (hFile != INVALID_HANDLE_VALUE) {
            MINIDUMP_EXCEPTION_INFORMATION mdei;
            mdei.ThreadId = GetCurrentThreadId();
            mdei.ExceptionPointers = pExceptionInfo;
            mdei.ClientPointers = FALSE;

            BOOL result = MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), hFile, MiniDumpWithDataSegs,
                                            &mdei, nullptr, nullptr);

            CloseHandle(hFile);

            if (result) {
                HookLog("CRASH: Dump written to %ws", dumpPath.c_str());
            } else {
                HookLog("CRASH: Failed to write dump (error=%lu)", GetLastError());
            }
        } else {
            HookLog("CRASH: Failed to create dump file (error=%lu)", GetLastError());
        }
    }

    // Pass to next handler (don't swallow the exception)
    return EXCEPTION_CONTINUE_SEARCH;
}

static void InstallCrashDumpHandler()
{
    if (g_CrashDumpInitialized) return;

    // Load dbghelp.dll dynamically
    HMODULE hDbgHelp = LoadLibraryA("dbghelp.dll");
    if (!hDbgHelp) {
        HookLog("CrashDump: Failed to load dbghelp.dll");
        return;
    }

    // Set unhandled exception filter
    SetUnhandledExceptionFilter(DX12ExceptionFilter);
    g_CrashDumpInitialized = true;
    HookLog("CrashDump: Exception handler installed");
}

// ============================================================================
// SpecialK-style Streamline Handling
// ============================================================================

// ============================================================================
// Typedefs for D3D12 functions
typedef void(STDMETHODCALLTYPE* ExecuteCommandListsPtr)(ID3D12CommandQueue*, UINT, ID3D12CommandList* const*);
typedef void(STDMETHODCALLTYPE* CreateSamplerPtr)(ID3D12Device*, const D3D12_SAMPLER_DESC*,
                                                  D3D12_CPU_DESCRIPTOR_HANDLE);
typedef HRESULT(STDMETHODCALLTYPE* CreateCommittedResourcePtr)(ID3D12Device*, const D3D12_HEAP_PROPERTIES*,
                                                               D3D12_HEAP_FLAGS, const D3D12_RESOURCE_DESC*,
                                                               D3D12_RESOURCE_STATES, const D3D12_CLEAR_VALUE*, REFIID,
                                                               void**);
typedef HRESULT(WINAPI* PFN_D3D12_SERIALIZE_ROOT_SIGNATURE)(const D3D12_ROOT_SIGNATURE_DESC*,
                                                            D3D_ROOT_SIGNATURE_VERSION, ID3DBlob**, ID3DBlob**);
typedef HRESULT(WINAPI* PFN_D3D12_SERIALIZE_VERSIONED_ROOT_SIGNATURE)(const D3D12_VERSIONED_ROOT_SIGNATURE_DESC*,
                                                                      ID3DBlob**, ID3DBlob**);

// Global Function Pointers for detours (Visible to other modules)
ExecuteCommandListsPtr oExecuteCommandLists = nullptr;
CreateSamplerPtr oCreateSampler = nullptr;
CreateCommittedResourcePtr oCreateCommittedResource = nullptr;
PFN_D3D12_SERIALIZE_ROOT_SIGNATURE oSerializeRootSignature = nullptr;
PFN_D3D12_SERIALIZE_VERSIONED_ROOT_SIGNATURE oSerializeVersionedRootSignature = nullptr;

// SwapChain Detour Pointers
typedef HRESULT(STDMETHODCALLTYPE* PFN_CreateSwapChain)(IDXGIFactory*, IUnknown*, DXGI_SWAP_CHAIN_DESC*,
                                                        IDXGISwapChain**);
typedef HRESULT(STDMETHODCALLTYPE* PFN_CreateSwapChainForHwnd)(IDXGIFactory2*, IUnknown*, HWND,
                                                               const DXGI_SWAP_CHAIN_DESC1*,
                                                               const DXGI_SWAP_CHAIN_FULLSCREEN_DESC*, IDXGIOutput*,
                                                               IDXGISwapChain1**);

static PFN_CreateSwapChain oCreateSwapChain = nullptr;
static PFN_CreateSwapChainForHwnd oCreateSwapChainForHwnd = nullptr;

// --- DX12 Overlay State Management ---
struct DX12OverlayState {
    static const int ALLOC_POOL_SIZE = 3;
    std::vector<ID3D12CommandAllocator*> allocators;
    ID3D12GraphicsCommandList* cmdList = nullptr;
    int allocIndex = 0;
    ID3D12Fence* fence = nullptr;
    HANDLE fenceEvent = nullptr;
    UINT64 currentFenceValue = 0;
    std::vector<UINT64> fenceValues;
    ID3D12DescriptorHeap* rtvDescHeap = nullptr;
    ID3D12DescriptorHeap* srvDescHeap = nullptr;
    UINT rtvDescriptorSize = 0;
    std::vector<ID3D12Resource*> backBuffers;
    bool imGuiInit = false;
    bool syncInit = false;
    int cachedWidth = 0;
    int cachedHeight = 0;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    UINT bufferCount = 0;
    IDXGISwapChain* cachedSwapChain = nullptr;
    void Cleanup()
    {
        for (auto& bb : backBuffers)
            if (bb) bb->Release();
        backBuffers.clear();
        if (rtvDescHeap) {
            rtvDescHeap->Release();
            rtvDescHeap = nullptr;
        }
        if (srvDescHeap) {
            srvDescHeap->Release();
            srvDescHeap = nullptr;
        }
        for (auto* alloc : allocators)
            if (alloc) alloc->Release();
        allocators.clear();
        if (cmdList) {
            cmdList->Release();
            cmdList = nullptr;
        }
        if (fence) {
            fence->Release();
            fence = nullptr;
        }
        if (fenceEvent) {
            CloseHandle(fenceEvent);
            fenceEvent = nullptr;
        }
        imGuiInit = false;
        syncInit = false;
    }
};

static DX12OverlayState g_State;
static std::recursive_mutex g_ImGuiFrameMutex;
static SharedCaptureD3D12 g_SharedCaptureD3D12;

ID3D12Device* g_Device = nullptr;
ID3D12CommandQueue* g_CommandQueue = nullptr;
std::recursive_mutex g_CommandQueueMutex;

// CRITICAL FIX: Thread-safe accessors for g_Device and g_CommandQueue
// These functions acquire the mutex and return a reference-counted pointer
// to prevent use-after-free when the queue/device is destroyed on another thread
struct DX12Context {
    ID3D12Device* device = nullptr;
    ID3D12CommandQueue* queue = nullptr;

    DX12Context() = default;

    DX12Context(ID3D12Device* d, ID3D12CommandQueue* q) : device(d), queue(q)
    {
        if (device) device->AddRef();
        if (queue) queue->AddRef();
    }

    ~DX12Context()
    {
        if (device) {
            device->Release();
            device = nullptr;
        }
        if (queue) {
            queue->Release();
            queue = nullptr;
        }
    }

    // Disable copy to prevent accidental double-release
    DX12Context(const DX12Context&) = delete;
    DX12Context& operator=(const DX12Context&) = delete;

    // Enable move
    DX12Context(DX12Context&& other) noexcept : device(other.device), queue(other.queue)
    {
        other.device = nullptr;
        other.queue = nullptr;
    }

    DX12Context& operator=(DX12Context&& other) noexcept
    {
        if (this != &other) {
            if (device) device->Release();
            if (queue) queue->Release();
            device = other.device;
            queue = other.queue;
            other.device = nullptr;
            other.queue = nullptr;
        }
        return *this;
    }

    bool IsValid() const { return device != nullptr && queue != nullptr; }
};

// Thread-safe accessor - ALWAYS use this instead of direct g_Device/g_CommandQueue access
static DX12Context GetDX12Context()
{
    std::lock_guard<std::recursive_mutex> lock(g_CommandQueueMutex);
    return DX12Context(g_Device, g_CommandQueue);
}

static std::atomic<uint64_t> g_FrameIndex{0};
static std::atomic<int> g_CommandListsExecutedThisFrame{0};
static std::atomic<uint64_t> g_FGDebugFrameCount{0};

// Last swapchain reference for device change detection
static IDXGISwapChain* g_LastSwapChain = nullptr;

// IPC ready flag
static bool g_IPCReady = false;

ID3D12Resource* g_DummyBackBuffer = nullptr;

// Dedicated overlay queue for Frame Generation compatibility
// Uses CPU-side fence wait (not GPU Wait) to avoid cross-queue deadlocks
static ID3D12CommandQueue* g_OverlayQueue = nullptr;

// LOCK HIERARCHY (MUST be acquired in this order to prevent deadlocks):
// 1. g_OverlayMutex (outermost - protects overlay state)
// 2. g_CommandQueueMutex (protects command queue pointer)
// 3. g_DX12CaptureMutex (innermost - protects capture state)
//
// Rule: When acquiring multiple locks, always acquire in order above.
//       Use std::lock_guard with std::adopt_lock when using try_lock().
static std::recursive_mutex g_OverlayMutex;
static std::recursive_mutex g_InitImGuiMutex;
static std::recursive_mutex g_DX12CaptureMutex;
static std::atomic<bool> g_InSwapchainResizeCleanup{false};

// Frame counter for post-ImGui-init delay (skip first frame to let GPU stabilize)
static std::atomic<int> s_framesSinceInit{0};

// Use pointer to prevent static destructor execution in non-game processes (Explorer fix)
DX12Hook* g_dx12HookInstance = nullptr;

// Cached overlay renderer for zero-overhead interpolated frame rendering
overlay::CachedOverlayRenderer* g_CachedOverlayRenderer = nullptr;
bool g_UseCachedRenderer = false;  // DISABLED: Cached renderer needs proper D3D12 secondary command list implementation

std::recursive_mutex g_DeviceQueuesMutex;
std::map<ID3D12Device*, ID3D12CommandQueue*> g_DeviceQueues;

// CPU Prerender Limit State (DX12)
static std::vector<ID3D12Fence*> g_PrerenderFences;
static std::vector<HANDLE> g_PrerenderEvents;
static uint64_t g_PrerenderFrameIndex = 0;
static std::mutex g_PrerenderMutex;

// Forward Declarations
void STDMETHODCALLTYPE DetourExecuteCommandLists(ID3D12CommandQueue* pThis, UINT NumCommandLists,
                                                 ID3D12CommandList* const* ppCommandLists);
void DX12_HookQueueVTable(ID3D12CommandQueue* queue);
HRESULT STDMETHODCALLTYPE DetourCreateSwapChain(IDXGIFactory* pThis, IUnknown* pDevice, DXGI_SWAP_CHAIN_DESC* pDesc,
                                                IDXGISwapChain** ppSwapChain);
HRESULT STDMETHODCALLTYPE DetourCreateSwapChainForHwnd(IDXGIFactory2* pThis, IUnknown* pDevice, HWND hWnd,
                                                       const DXGI_SWAP_CHAIN_DESC1* pDesc,
                                                       const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* pFDesc, IDXGIOutput* pOut,
                                                       IDXGISwapChain1** ppSC);

// REQUIRED EXPORTS
void DX12_AdjustWrapperResizeDepth(int delta)
{
    if (delta > 0)
        DXGIShared::g_SharedState.wrapperResizeDepth.fetch_add(delta);
    else
        DXGIShared::g_SharedState.wrapperResizeDepth.fetch_sub(-delta);
}

// Forward declaration removed - OverlayAdapter handles cleanup

void DX12_InvalidateSwapchain()
{
    DXGIShared::g_SharedState.swapchainInvalid.store(true, std::memory_order_release);
    HookLog("DX12: Swapchain marked INVALID (FSR/FG transition detected)");
    // Log current state for debugging
    HookLog("DX12: Invalidating - imGuiInit=%d, syncInit=%d, device=%p, queue=%p", g_State.imGuiInit, g_State.syncInit,
            g_Device, g_CommandQueue);

    // CRITICAL FIX: Defer ImGui cleanup to next frame to avoid interfering with FSR swapchain creation
    // FSR FG is sensitive to D3D12 operations during its swapchain setup
    // Don't call ShutdownImGui() here - let ProcessFrame handle it on the next present
    if (g_State.imGuiInit) {
        HookLog("DX12: Deferring ImGui shutdown to next frame (avoiding FSR swapchain conflict)");
        // Just mark as needing reinit - don't actually clean up resources yet
        g_State.imGuiInit = false;
        g_State.syncInit = false;
        DX12_ResetImGuiFrameCounter();
    }
}

void DX12_SignalFSR4SwapchainRecreated()
{
    DXGIShared::g_SharedState.fsr4RecreationPending.store(true, std::memory_order_release);
    HookLog("DX12: FSR4 swapchain recreation signaled");
}

// C Linkage Exports for cross-module calls (e.g. from C clients or GetProcAddress)
extern "C" {
void DX12_SetCommandQueue(ID3D12CommandQueue* pQueue)
{
    if (!pQueue) return;

    // CRITICAL FIX: Only allow DIRECT queues for overlay rendering.
    // Strange Brigade and other DX12 games use Async Compute queues.
    // Submitting overlay (Direct) commands to a Compute queue causes a device lost/crash.
    D3D12_COMMAND_QUEUE_DESC desc = pQueue->GetDesc();
    if (desc.Type != D3D12_COMMAND_LIST_TYPE_DIRECT) {
        // HookLog("DX12: Ignoring non-direct queue (Type=%d)", desc.Type);
        return;
    }

    std::lock_guard<std::recursive_mutex> lock(g_CommandQueueMutex);
    if (g_CommandQueue != pQueue) {
        if (g_CommandQueue) g_CommandQueue->Release();
        g_CommandQueue = pQueue;
        g_CommandQueue->AddRef();
        ID3D12Device* dev = nullptr;
        if (SUCCEEDED(g_CommandQueue->GetDevice(IID_PPV_ARGS(&dev)))) {
            if (g_Device != dev) {
                if (g_Device) g_Device->Release();
                g_Device = dev;
            } else
                dev->Release();
        }
    }
}
void DX12_AdjustWrapperResizeDepth_C(int delta) { DX12_AdjustWrapperResizeDepth(delta); }

// Export for D3D12 wrapper to notify command list execution (frame classification)
void DX12_NotifyCommandLists(UINT numCommandLists)
{
    g_CommandListsExecutedThisFrame.fetch_add(numCommandLists, std::memory_order_relaxed);
}
}

void DX12_OnSwapchainResizeEnd();
void CleanupOverlay();
void CleanupRTVs();
// void ShutdownImGui(); // MIGRATED: OverlayAdapter handles cleanup
void DX12_InvalidateSwapchain();

// Helper to ensure global hook instance exists
void EnsureDX12Hook()
{
    if (!g_dx12HookInstance) {
        g_dx12HookInstance = new DX12Hook();
    }
}

// Forward declarations
static void InstallGlobalVTableHooks();
static void HookSwapchainVTableViaTempSwapchain();

void DX12Hook::Init()
{
    EnsureDX12Hook();  // Self-init check
    static std::recursive_mutex s_InitMutex;
    static bool s_InitDone = false;
    std::lock_guard<std::recursive_mutex> lock(s_InitMutex);
    if (s_InitDone) return;
    s_InitDone = true;

    // CRITICAL FIX: Check if Vulkan is active before installing ANY DXGI hooks
    // Vulkan games using WSI-to-DXGI mapping can freeze if we hook DXGI
    HMODULE hVulkan = GetModuleHandleW(L"vulkan-1.dll");
    if (hVulkan) {
        HookLog("DX12: Vulkan detected (vulkan-1.dll), SKIPPING ALL DXGI hook installation");
        // Still install crash handler for debugging, but don't hook DXGI
        InstallCrashDumpHandler();
        return;
    }

    // Install crash dump handler for debugging
    InstallCrashDumpHandler();

    // Start freeze detection watchdog with appropriate timeout
    // DLSS/FSR FG needs longer timeout (2 min) since frame gen can pause briefly
    // NOTE: We use a long timeout (10 min) by default since we may not get heartbeats
    // if the swapchain was created before our hooks were installed
    // CRITICAL: Use a very long timeout to avoid false positives with UE5/DLSS FG
    // DISABLE watchdog for now - we're getting false positives because Present
    // hooks aren't receiving heartbeats (swapchain created before injection)
    // g_RenderWatchdog.Start(timeout);
    // HookLog("DX12: Freeze watchdog started (%.0f second timeout)", timeout);
    HookLog("DX12: Freeze watchdog DISABLED (swapchain timing issue with UE5/DLSS)");

    // CRITICAL FIX: Install global swapchain vtable hooks by getting the vtable
    // directly from the DXGI module. This avoids creating a temp swapchain which
    // causes deadlocks with Steam overlay + Streamline.
    InstallGlobalVTableHooks();

    HookLog("DX12Hook: Initialized (wrapper + global vtable hooks)");
    HookLog("DX12: NOTE - Watchdog disabled to avoid false positives with pre-existing swapchains");
}

// Original function pointers for global factory hooks
typedef HRESULT(STDMETHODCALLTYPE* PFN_CreateSwapChain)(IDXGIFactory*, IUnknown*, DXGI_SWAP_CHAIN_DESC*,
                                                        IDXGISwapChain**);
typedef HRESULT(STDMETHODCALLTYPE* PFN_CreateSwapChainForHwnd)(IDXGIFactory2*, IUnknown*, HWND,
                                                               const DXGI_SWAP_CHAIN_DESC1*,
                                                               const DXGI_SWAP_CHAIN_FULLSCREEN_DESC*, IDXGIOutput*,
                                                               IDXGISwapChain1**);
static PFN_CreateSwapChain oCreateSwapChainGlobal = nullptr;
static PFN_CreateSwapChainForHwnd oCreateSwapChainForHwndGlobal = nullptr;

// Detour for global CreateSwapChain hook
static HRESULT STDMETHODCALLTYPE DetourCreateSwapChainGlobal(IDXGIFactory* pThis, IUnknown* pDevice,
                                                             DXGI_SWAP_CHAIN_DESC* pDesc, IDXGISwapChain** ppSwapChain)
{
    static std::atomic<int> s_count{0};
    int count = s_count.fetch_add(1);
    HookLog("DX12: DetourCreateSwapChainGlobal CALLED #%d", count);

    // CRITICAL: ALWAYS hook the command queue vtable for frame detection
    // This works for both normal games and DLSS/FSR FG games
    // The pDevice passed to CreateSwapChain is actually the command queue for D3D12
    if (pDevice) {
        ID3D12CommandQueue* q = nullptr;
        if (SUCCEEDED(pDevice->QueryInterface(IID_PPV_ARGS(&q)))) {
            DX12_HookQueueVTable(q);
            HookLog("DX12: Global hook - Command queue vtable hooked");
            q->Release();
        }
    }

    // Call original
    HRESULT hr = oCreateSwapChainGlobal(pThis, pDevice, pDesc, ppSwapChain);

    if (SUCCEEDED(hr) && ppSwapChain && *ppSwapChain) {
        IDXGISwapChain3* sc3 = nullptr;
        if (SUCCEEDED((*ppSwapChain)->QueryInterface(IID_PPV_ARGS(&sc3)))) {
            DXGIShared::InstallHooks(sc3);
            sc3->Release();
            HookLog("DX12: Global hook installed on swapchain");
        }
    }

    return hr;
}

// Detour for global CreateSwapChainForHwnd hook
static HRESULT STDMETHODCALLTYPE DetourCreateSwapChainForHwndGlobal(IDXGIFactory2* pThis, IUnknown* pDevice, HWND hWnd,
                                                                    const DXGI_SWAP_CHAIN_DESC1* pDesc,
                                                                    const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* pFDesc,
                                                                    IDXGIOutput* pOut, IDXGISwapChain1** ppSC)
{
    // CRITICAL: ALWAYS hook the command queue vtable for frame detection
    if (pDevice) {
        ID3D12CommandQueue* q = nullptr;
        if (SUCCEEDED(pDevice->QueryInterface(IID_PPV_ARGS(&q)))) {
            DX12_HookQueueVTable(q);
            HookLog("DX12: Global CreateSwapChainForHwnd - Command queue vtable hooked");
            q->Release();
        }
    }

    HRESULT hr = oCreateSwapChainForHwndGlobal(pThis, pDevice, hWnd, pDesc, pFDesc, pOut, ppSC);

    if (SUCCEEDED(hr) && ppSC && *ppSC) {
        IDXGISwapChain3* sc3 = nullptr;
        if (SUCCEEDED((*ppSC)->QueryInterface(IID_PPV_ARGS(&sc3)))) {
            DXGIShared::InstallHooks(sc3);
            sc3->Release();
            HookLog("DX12: Global CreateSwapChainForHwnd - Hooks installed on swapchain");
        }
    }

    return hr;
}

// Install global hooks on the DXGI factory to catch ALL swapchain creation
// This hooks the factory vtable directly in the DXGI module
static void InstallGlobalVTableHooks()
{
    HookLog("DX12: InstallGlobalVTableHooks called");

    // CRITICAL: Always install factory hooks - even with FSR FG active
    // The Present hooks will send heartbeats for the watchdog and passthrough for FSR FG
    // Only skip ResizeBuffers hooks when FSR FG is detected (see DXGIShared::InstallHooks)
    HMODULE hFSRBackend = GetModuleHandleW(L"ffx_backend_dx12_x64.dll");
    HMODULE hFSRFG = GetModuleHandleW(L"ffx_frameinterpolation_x64.dll");
    if (hFSRBackend || hFSRFG) {
        HookLog("DX12: FSR FG DLLs detected, installing factory hooks for watchdog heartbeats");
    }

    // Get the DXGI module
    HMODULE hDXGI = GetModuleHandleA("dxgi.dll");
    if (!hDXGI) {
        HookLog("DX12: DXGI module not loaded");
        return;
    }

    // Get CreateDXGIFactory1 export
    typedef HRESULT(WINAPI * PFN_CreateDXGIFactory1)(REFIID, void**);
    PFN_CreateDXGIFactory1 pCreateFactory = (PFN_CreateDXGIFactory1)GetProcAddress(hDXGI, "CreateDXGIFactory1");
    if (!pCreateFactory) {
        HookLog("DX12: CreateDXGIFactory1 not found");
        return;
    }

    // Create a factory to get its vtable
    IDXGIFactory2* pFactory = nullptr;
    HRESULT hr = pCreateFactory(IID_PPV_ARGS(&pFactory));
    if (FAILED(hr) || !pFactory) {
        HookLog("DX12: Failed to create factory for vtable extraction hr=0x%08X", hr);
        return;
    }

    // Get the vtable
    void** vtable = *(void***)pFactory;
    HookLog("DX12: Factory vtable at %p", vtable);

    // Hook CreateSwapChain (vtable[10] for IDXGIFactory)
    // Hook CreateSwapChainForHwnd (vtable[15] for IDXGIFactory2)
    if (VTableHook::Create(&vtable[10], (LPVOID)DetourCreateSwapChainGlobal, (LPVOID*)&oCreateSwapChainGlobal)) {
        HookLog("DX12: Hooked global CreateSwapChain at vtable[10]");
    }

    if (VTableHook::Create(&vtable[15], (LPVOID)DetourCreateSwapChainForHwndGlobal,
                           (LPVOID*)&oCreateSwapChainForHwndGlobal)) {
        HookLog("DX12: Hooked global CreateSwapChainForHwnd at vtable[15]");
    }

    // Release the factory - the vtable hooks persist
    pFactory->Release();

    HookLog("DX12: Global vtable hooks installed");

    HookSwapchainVTableViaTempSwapchain();
}

// Hook swapchain vtable by creating a temp swapchain and hooking its vtable
// This is the Special K approach - all swapchains share the same vtable
static void HookSwapchainVTableViaTempSwapchain()
{
    HMODULE hDXGI = GetModuleHandleA("dxgi.dll");
    HMODULE hD3D12 = GetModuleHandleA("d3d12.dll");
    if (!hDXGI || !hD3D12) return;

    typedef HRESULT(WINAPI * PFN_CreateDXGIFactory1)(REFIID, void**);
    typedef HRESULT(WINAPI * PFN_D3D12CreateDevice)(IUnknown*, D3D_FEATURE_LEVEL, REFIID, void**);

    PFN_CreateDXGIFactory1 pCreateFactory = (PFN_CreateDXGIFactory1)GetProcAddress(hDXGI, "CreateDXGIFactory1");
    PFN_D3D12CreateDevice pD3D12CreateDevice = (PFN_D3D12CreateDevice)GetProcAddress(hD3D12, "D3D12CreateDevice");
    if (!pCreateFactory || !pD3D12CreateDevice) return;

    IDXGIFactory2* pFactory = nullptr;
    if (FAILED(pCreateFactory(IID_PPV_ARGS(&pFactory))) || !pFactory) return;

    ID3D12Device* pDevice = nullptr;
    if (FAILED(pD3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&pDevice))) || !pDevice) {
        pFactory->Release();
        return;
    }

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

    IDXGISwapChain1* pSwapChain = nullptr;
    HRESULT hr = pFactory->CreateSwapChainForHwnd(pQueue, hwnd, &scd, nullptr, nullptr, &pSwapChain);

    if (SUCCEEDED(hr) && pSwapChain) {
        void** vtable = *(void***)pSwapChain;
        HookLog("DX12: Temp swapchain vtable=%p, Present=%p", vtable, vtable[8]);

        // Install hooks on the swapchain vtable - this hooks ALL swapchains
        DXGIShared::InstallHooks(pSwapChain);
        HookLog("DX12: Swapchain vtable hooks installed");

        pSwapChain->Release();
    }

    // Cleanup
    if (hwnd) DestroyWindow(hwnd);
    UnregisterClassW(L"CE_Temp", wc.hInstance);
    pQueue->Release();
    pDevice->Release();
    pFactory->Release();
}

void ShutdownImGui()
{
    if (!g_State.imGuiInit) return;

    if (g_OverlayAdapter.IsInitialized()) {
        g_OverlayAdapter.Shutdown();
    }

    // Shutdown cached overlay renderer
    if (g_CachedOverlayRenderer) {
        g_CachedOverlayRenderer->Shutdown();
        delete g_CachedOverlayRenderer;
        g_CachedOverlayRenderer = nullptr;
    }

    if (g_State.srvDescHeap) {
        g_State.srvDescHeap->Release();
        g_State.srvDescHeap = nullptr;
    }
    g_State.imGuiInit = false;
}

bool InitImGui(ID3D12Device* device, int buffers, DXGI_FORMAT format, HWND hwnd)
{
    std::lock_guard<std::recursive_mutex> lock(g_InitImGuiMutex);

    // OverlayAdapter re-init check
    if (g_OverlayAdapter.IsInitialized()) {
        HookLog("InitImGui: OverlayAdapter already initialized, shutting down for re-init");
        g_OverlayAdapter.Shutdown();
    }

    if (g_State.imGuiInit) {
        HookLog("InitImGui: Already initialized, returning early");
        return true;
    }

    HookLog("InitImGui: Proceeding with initialization - buffers=%d, format=%d, hwnd=%p", buffers, format, hwnd);

    g_State.format = format;

    // Use OverlayAdapter instead of ImGui
    if (!g_OverlayAdapter.InitDX12(device, g_OverlayQueue, format)) {
        HookLog("InitImGui: OverlayAdapter.InitDX12 failed!");
        return false;
    }

    // g_SharedOverlay.InitImGui(hwnd); // MIGRATED: Using OverlayAdapter instead
    // OverlayAdapter handles its own initialization

    InputManager::Get().HookWindow(hwnd);

    // We don't need SRV heap for ImGui anymore, OverlayAdapter manages its own resources.
    // But we might need it if we keep ImGui for menus?
    // For now assuming full replacement for overlay.

    g_State.imGuiInit = true;

    // Reset frame delay counter on reinitialization
    extern void DX12_ResetOverlayFrameDelay();
    DX12_ResetOverlayFrameDelay();

    // Initialize cached overlay renderer for Frame Generation support
    // Uses g_OverlayQueue (created in InitOverlaySync) for all rendering
    if (g_UseCachedRenderer && !g_CachedOverlayRenderer) {
        g_CachedOverlayRenderer = new overlay::CachedOverlayRenderer();
        if (!g_CachedOverlayRenderer->Initialize(device, g_OverlayQueue, buffers)) {
            HookLog("DX12: Failed to initialize cached overlay renderer, falling back to standard");
            delete g_CachedOverlayRenderer;
            g_CachedOverlayRenderer = nullptr;
            g_UseCachedRenderer = false;
        } else {
            HookLog("DX12: Cached overlay renderer initialized for %d buffers", buffers);
        }
    }

    return true;
}

void DrawOverlay(ID3D12GraphicsCommandList* cmdList, bool isRealFrame, UINT bufferIdx)
{
    // CRITICAL FIX: Lock mutex to prevent concurrent access during InitImGui shutdown/reinit
    std::lock_guard<std::recursive_mutex> lock(g_InitImGuiMutex);

    if (!g_State.imGuiInit || !cmdList) return;

    // CRITICAL FIX: Verify overlay is ready - REMOVED: ImGui context check
    // Context could have been destroyed between the check above and mutex acquisition
    // if (ImGui::GetCurrentContext() == nullptr) {  // REMOVED: Using custom overlay
    //     HookLog("DrawOverlay: Context is null after mutex acquisition, aborting");
    //     return;
    // }
    // OverlayAdapter handles its own context management

    // Change 6: Remove verbose per-frame logging to improve performance at high FPS
    // Keep this section empty - logging removed

    // Cached Overlay Renderer removed - superseded by CustomOverlay
    // if (g_UseCachedRenderer && g_CachedOverlayRenderer) { ... }

    // Standard overlay rendering (fallback path when cached renderer not available)
    // Change 4: Only update ImGui content on real frames, reuse cached draw data on interpolated frames
    // Standard overlay rendering (fallback path when cached renderer not available)
    if (isRealFrame) {
        g_OverlayAdapter.SetMetrics(DXGIShared::GetPerformanceMetrics());
        g_OverlayAdapter.SetIPCClient(g_IPC);
        const char* api = "DX12";
        if (GetModuleHandleA("d3d12core.dll") && (GetModuleHandleA("libvkd3d-1.dll") || GetModuleHandleA("vkd3d.dll")))
            api = "DX12 (VKD3D)";
        g_OverlayAdapter.SetGraphicsAPI(api);
        bool isHDR =
            (g_State.format == DXGI_FORMAT_R16G16B16A16_FLOAT || g_State.format == DXGI_FORMAT_R10G10B10A2_UNORM);
        g_OverlayAdapter.SetHDR(isHDR);
    }

    // Set Render Target for Custom Overlay
    // We already calculated the CPU descriptor handle, but we need the CPU handle corresponding to the current
    // backbuffer g_State.rtvDescHeap contains descriptors for all backbuffers bufferIdx tells us which one is current

    D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = g_State.rtvDescHeap->GetCPUDescriptorHandleForHeapStart();
    rtvHandle.ptr += bufferIdx * g_State.rtvDescriptorSize;

    g_OverlayAdapter.SetDX12RenderTarget(cmdList, (void*)rtvHandle.ptr);

    // Render
}

void CreateRTVs(ID3D12Device* device, IDXGISwapChain3* swapChain, int bufferCount)
{
    if (g_State.rtvDescHeap) return;

    // DLSS FG FIX: Validate buffer count before creating RTVs
    if (bufferCount <= 0 || bufferCount > 8) {
        HookLog("CreateRTVs: Invalid buffer count %d, limiting to 3", bufferCount);
        bufferCount = 3;
    }

    D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc = {D3D12_DESCRIPTOR_HEAP_TYPE_RTV, (UINT)bufferCount,
                                              D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
    if (FAILED(device->CreateDescriptorHeap(&rtvHeapDesc, IID_PPV_ARGS(&g_State.rtvDescHeap)))) return;
    g_State.bufferCount = bufferCount;
    g_State.cachedSwapChain = swapChain;
    g_State.rtvDescriptorSize = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = g_State.rtvDescHeap->GetCPUDescriptorHandleForHeapStart();
    for (int i = 0; i < bufferCount; i++) {
        ID3D12Resource* bb = nullptr;
        if (SUCCEEDED(swapChain->GetBuffer(i, IID_PPV_ARGS(&bb))) && bb) {
            device->CreateRenderTargetView(bb, nullptr, rtvHandle);
            bb->Release();
        }
        rtvHandle.ptr += g_State.rtvDescriptorSize;
    }
    HookLog("CreateRTVs: Created %d RTVs", bufferCount);
}

void InitOverlaySync(ID3D12Device* device, int bufferCount)
{
    if (g_State.syncInit) return;

    // Create dedicated overlay command queue for FG compatibility
    // Using our own queue prevents interference with game/FG runtime queues.
    // We use CPU-side WaitForSingleObject (not GPU-side Queue->Wait) to avoid deadlocks.
    if (!g_OverlayQueue) {
        D3D12_COMMAND_QUEUE_DESC qd = {};
        qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        qd.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
        qd.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
        qd.NodeMask = 0;
        HRESULT hr = device->CreateCommandQueue(&qd, IID_PPV_ARGS(&g_OverlayQueue));
        if (FAILED(hr)) {
            HookLog("InitOverlaySync: Failed to create overlay queue hr=0x%08X", hr);
            return;
        }
        g_OverlayQueue->SetName(L"CE_OverlayQueue");
        HookLog("InitOverlaySync: Created dedicated overlay queue %p", g_OverlayQueue);
    }

    if (FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_State.fence)))) return;

    g_State.allocators.resize(DX12OverlayState::ALLOC_POOL_SIZE);
    g_State.fenceValues.resize(DX12OverlayState::ALLOC_POOL_SIZE);

    // CRITICAL FIX: Reset all fence values to 0 for fresh start
    // After resize/reinit, old fence values could be stale and cause infinite waits
    std::fill(g_State.fenceValues.begin(), g_State.fenceValues.end(), 0);
    g_State.currentFenceValue = 0;
    g_State.allocIndex = 0;

    HookLog("InitOverlaySync: Fence values reset to 0, currentFenceValue=0");

    bool success = true;
    for (int i = 0; i < DX12OverlayState::ALLOC_POOL_SIZE; i++) {
        if (FAILED(
                device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&g_State.allocators[i])))) {
            success = false;
            break;
        }
    }

    if (success) {
        if (FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g_State.allocators[0], nullptr,
                                             IID_PPV_ARGS(&g_State.cmdList)))) {
            success = false;
        }
    }

    if (success) {
        g_State.cmdList->Close();
        g_State.fenceEvent = CreateEvent(NULL, FALSE, FALSE, NULL);
        g_State.syncInit = true;
    } else {
        // Cleanup partial initialization
        for (auto* alloc : g_State.allocators)
            if (alloc) alloc->Release();
        g_State.allocators.clear();
        g_State.fenceValues.clear();
        if (g_State.cmdList) {
            g_State.cmdList->Release();
            g_State.cmdList = nullptr;
        }
        if (g_State.fence) {
            g_State.fence->Release();
            g_State.fence = nullptr;
        }
    }
}

static bool DrainCommandQueue(ID3D12CommandQueue* queue, ID3D12Device* device)
{
    if (!queue || !device) return false;

    // NON-BLOCKING DRAIN: Use a flush approach instead of waiting
    // to avoid deadlocking when called from the submit thread.
    ID3D12Fence* fence = nullptr;
    if (FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)))) return false;

    // Signal the fence but don't wait - if we're on the submit thread,
    // waiting would deadlock. The fence will be processed when the
    // game next submits work.
    queue->Signal(fence, 1);

    // Quick check if already completed (GPU was idle)
    if (fence->GetCompletedValue() >= 1) {
        fence->Release();
        return true;
    }

    // Optional: very short wait for already-in-flight work (1ms)
    // This helps if the GPU is just finishing up, without blocking
    // the submit thread for long.
    HANDLE event = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    if (event) {
        if (fence->SetEventOnCompletion(1, event) == S_OK) {
            WaitForSingleObject(event, 1);  // 1ms non-blocking wait
        }
        CloseHandle(event);
    }
    fence->Release();
    return true;
}

void CleanupOverlay()
{
    if (!g_State.syncInit) return;

    // Flush overlay queue before cleanup
    if (g_State.fence && g_OverlayQueue) {
        UINT64 waitValue = g_State.currentFenceValue + 1;
        if (SUCCEEDED(g_OverlayQueue->Signal(g_State.fence, waitValue))) {
            if (g_State.fence->GetCompletedValue() < waitValue) {
                g_State.fence->SetEventOnCompletion(waitValue, g_State.fenceEvent);
                WaitForSingleObject(g_State.fenceEvent, 200);
            }
        }
    }
    if (g_State.fenceEvent) {
        CloseHandle(g_State.fenceEvent);
        g_State.fenceEvent = NULL;
    }
    for (auto alloc : g_State.allocators)
        if (alloc) alloc->Release();
    g_State.allocators.clear();
    g_State.fenceValues.clear();
    if (g_State.cmdList) {
        g_State.cmdList->Release();
        g_State.cmdList = nullptr;
    }
    if (g_State.fence) {
        g_State.fence->Release();
        g_State.fence = nullptr;
    }
    g_State.currentFenceValue = 0;
    g_State.allocIndex = 0;
    g_State.syncInit = false;

    // Release overlay queue
    if (g_OverlayQueue) {
        g_OverlayQueue->Release();
        g_OverlayQueue = nullptr;
    }

    // ShutdownImGui(); // MIGRATED: OverlayAdapter handles cleanup
}

void CleanupRTVs()
{
    for (auto* r : g_State.backBuffers)
        if (r) r->Release();
    g_State.backBuffers.clear();
    if (g_DummyBackBuffer) {
        g_DummyBackBuffer->Release();
        g_DummyBackBuffer = nullptr;
    }
    if (g_State.rtvDescHeap) {
        g_State.rtvDescHeap->Release();
        g_State.rtvDescHeap = nullptr;
    }
    if (g_State.srvDescHeap) {
        g_State.srvDescHeap->Release();
        g_State.srvDescHeap = nullptr;
    }
}

void DX12_OnSwapchainResizeBegin()
{
    bool wasAlreadySet = g_InSwapchainResizeCleanup.exchange(true);
    HookLog("DX12: DX12_OnSwapchainResizeBegin called, wasAlreadySet=%d", wasAlreadySet);

    // Prevent recursion - if already in resize, return immediately
    if (wasAlreadySet) {
        HookLog("DX12: DX12_OnSwapchainResizeBegin - already in resize, returning early");
        return;
    }

    DXGIShared::g_SharedState.lastSwapchainCreation = std::chrono::steady_clock::now();
    HookLog("DX12: DX12_OnSwapchainResizeBegin - step 1: timestamp updated");

    // Use try_lock to avoid blocking the render thread
    if (!g_OverlayMutex.try_lock()) {
        HookLog("DX12: DX12_OnSwapchainResizeBegin - mutex busy, returning early");
        return;
    }
    HookLog("DX12: DX12_OnSwapchainResizeBegin - step 2: got mutex");

    // RAII unlock when we exit
    std::lock_guard<std::recursive_mutex> lock(g_OverlayMutex, std::adopt_lock);

    // DO NOT release D3D12 resources here - just mark them invalid.
    // The real ResizeBuffers will handle synchronization internally.
    // Releasing resources now can cause the GPU to hang waiting for them.

    // Just mark ImGui as not initialized - resources will be cleaned up after resize
    g_State.imGuiInit = false;
    g_State.syncInit = false;
    HookLog("DX12: DX12_OnSwapchainResizeBegin - step 3: marked state invalid");

    // Release swapchain reference only (this is safe)
    if (g_LastSwapChain) {
        g_LastSwapChain->Release();
        g_LastSwapChain = nullptr;
    }
    HookLog("DX12: DX12_OnSwapchainResizeBegin - complete");
}

void DX12_OnSwapchainResizeEnd()
{
    HookLog("DX12: DX12_OnSwapchainResizeEnd called");
    // Only clear if it was set - prevents unbalanced calls from clearing prematurely
    if (g_InSwapchainResizeCleanup.load(std::memory_order_acquire)) {
        g_InSwapchainResizeCleanup.store(false, std::memory_order_release);
    }
}

// --- CPU Prerender Limit Support (DX12) ---
static void ApplyPrerenderLimitDX12(float limit)
{
    if (limit < 0.0f) return;
    // CRITICAL FIX: Use thread-safe accessor to prevent race conditions
    DX12Context ctx = GetDX12Context();
    if (!ctx.IsValid()) return;

    std::lock_guard<std::mutex> lock(g_PrerenderMutex);

    // Initialize fence ring buffer if needed
    if (g_PrerenderFences.empty()) {
        for (int i = 0; i < 16; i++) {
            ID3D12Fence* fence = nullptr;
            HANDLE event = CreateEvent(nullptr, FALSE, FALSE, nullptr);
            if (SUCCEEDED(ctx.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)))) {
                g_PrerenderFences.push_back(fence);
                g_PrerenderEvents.push_back(event);
            } else if (event) {
                CloseHandle(event);
            }
        }
        HookLog("DX12: Created prerender limit fence ring buffer (size: %d)", (int)g_PrerenderFences.size());
    }

    if (g_PrerenderFences.empty()) return;

    size_t idx = g_PrerenderFrameIndex % g_PrerenderFences.size();
    ID3D12Fence* fence = g_PrerenderFences[idx];
    HANDLE event = g_PrerenderEvents[idx];

    if (limit == 0.0f) {
        // Strict Serial: Signal and immediately wait
        uint64_t value = g_PrerenderFrameIndex + 1;
        ctx.queue->Signal(fence, value);
        if (SUCCEEDED(fence->SetEventOnCompletion(value, event))) {
            WaitForSingleObject(event, INFINITE);
        }
    } else {
        // Buffered Limit
        bool isFractional = (limit > 0.01f && limit < 1.0f);
        int effectiveLimit = isFractional ? 1 : (int)limit;
        int lookback = effectiveLimit + 1;

        // Signal current frame
        uint64_t signalValue = g_PrerenderFrameIndex + 1;
        ctx.queue->Signal(fence, signalValue);

        // Wait on N frames ago
        if (g_PrerenderFrameIndex >= (uint64_t)lookback) {
            size_t waitIdx = (g_PrerenderFrameIndex - lookback) % g_PrerenderFences.size();
            ID3D12Fence* waitFence = g_PrerenderFences[waitIdx];
            HANDLE waitEvent = g_PrerenderEvents[waitIdx];
            uint64_t waitValue = (g_PrerenderFrameIndex - lookback) + 1;

            if (waitFence->GetCompletedValue() < waitValue) {
                if (SUCCEEDED(waitFence->SetEventOnCompletion(waitValue, waitEvent))) {
                    WaitForSingleObject(waitEvent, INFINITE);
                }
            }
        }
    }

    g_PrerenderFrameIndex++;
}

void ProcessFrame(IDXGISwapChain* pSwapChain, bool processCapture)
{
    bool inResize = g_InSwapchainResizeCleanup.load(std::memory_order_acquire);
    if (!pSwapChain || inResize) {
        HookLog("DX12: ProcessFrame - early return (null=%d, inResize=%d)", !pSwapChain, inResize);
        return;
    }

    // CRITICAL: Detect swapchain change (e.g., FSR FG activation creates new swapchain)
    // and force re-initialization to work with the new swapchain
    static IDXGISwapChain* s_lastSwapChain = nullptr;
    if (pSwapChain != s_lastSwapChain && g_State.imGuiInit) {
        HookLog("DX12: Swapchain changed (%p -> %p), forcing re-initialization", s_lastSwapChain, pSwapChain);
        CleanupOverlay();
        CleanupRTVs();
        // ShutdownImGui(); // MIGRATED: OverlayAdapter handles cleanup
        g_State.imGuiInit = false;
        g_State.syncInit = false;
        DX12_ResetImGuiFrameCounter();
    }
    s_lastSwapChain = pSwapChain;

    // CPU Prerender Limit - Apply before any rendering
    float prerenderLimit = GetActivePrerenderLimit();
    if (prerenderLimit >= 0.0f) {
        ApplyPrerenderLimitDX12(prerenderLimit);
    }

    // PERFORMANCE FIX: Use try_lock instead of blocking lock_guard
    // This prevents stalling the render thread if another thread holds the lock
    if (!g_OverlayMutex.try_lock()) {
        // Another thread is processing, skip this frame
        HookLog("DX12: ProcessFrame - mutex busy, skipping frame");
        return;
    }
    // RAII unlock when we exit
    std::lock_guard<std::recursive_mutex> lock(g_OverlayMutex, std::adopt_lock);

    // SAFETY: Check device state after acquiring lock
    if (g_InSwapchainResizeCleanup.load(std::memory_order_acquire)) {
        HookLog("DX12: ProcessFrame - in resize cleanup, returning");
        return;
    }

    // EARLY QUEUE CREATION: Create overlay queue as soon as we have a device
    // This ensures the queue exists before any ImGui initialization attempts
    if (!g_OverlayQueue && g_Device) {
        D3D12_COMMAND_QUEUE_DESC qd = {};
        qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        qd.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
        qd.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
        qd.NodeMask = 0;
        HRESULT hr = g_Device->CreateCommandQueue(&qd, IID_PPV_ARGS(&g_OverlayQueue));
        if (SUCCEEDED(hr)) {
            g_OverlayQueue->SetName(L"CE_OverlayQueue");
            HookLog("DX12: ProcessFrame - Created overlay queue %p (early creation)", g_OverlayQueue);
        } else {
            HookLog("DX12: ProcessFrame - Failed to create overlay queue (early) hr=0x%08X", hr);
        }
    }

    // OPTIMIZATION: Only resolve device if swapchain changed or device not yet known
    if (!g_Device || pSwapChain != g_LastSwapChain) {
        IUnknown* pUnk = nullptr;
        if (FAILED(pSwapChain->GetDevice(IID_PPV_ARGS(&pUnk)))) {
            HookLog("DX12: ProcessFrame - failed to get device from swapchain");
            return;
        }
        ID3D12Device* activeDevice = nullptr;
        ID3D12CommandQueue* activeQueue = nullptr;
        if (SUCCEEDED(pUnk->QueryInterface(IID_PPV_ARGS(&activeQueue)))) {
            activeQueue->GetDevice(IID_PPV_ARGS(&activeDevice));
            DX12_HookQueueVTable(activeQueue);
            activeQueue->Release();
            HookLog("DX12: ProcessFrame - got device via command queue");
        } else {
            pUnk->QueryInterface(IID_PPV_ARGS(&activeDevice));
            HookLog("DX12: ProcessFrame - got device directly (no queue)");
        }
        pUnk->Release();

        if (!activeDevice) {
            HookLog("DX12: ProcessFrame - no active device");
            return;
        }

        // Initialize SystemMetricsCollector with adapter LUID when device is first obtained
        // This MUST happen before any device/swapchain change detection
        // NOTE: The D3D12 device and swap chain are wrapped, so we can't use QueryInterface
        // or GetParent directly. Instead, we get the output from the swap chain desc,
        // then get the adapter from the output.
        static bool s_metricsInitialized = false;
        if (!s_metricsInitialized && pSwapChain) {
            DXGI_SWAP_CHAIN_DESC swapDesc;
            if (SUCCEEDED(pSwapChain->GetDesc(&swapDesc))) {
                // Get the output from the swap chain, then get adapter from output
                IDXGIOutput* output = swapDesc.OutputWindow ? nullptr : nullptr;
                // Actually, swapDesc.OutputWindow is HWND, not IDXGIOutput
                // Let's enumerate outputs from the swap chain

                // Alternative: Use EnumAdapters to find the adapter that matches the device's node
                // Or: Create a temporary DXGI factory and find the adapter

                // Best approach: Use the fact that we can get IDXGIDevice from the swap chain's device
                // even if the D3D12 device is wrapped
                IUnknown* pDeviceUnk = nullptr;
                if (SUCCEEDED(pSwapChain->GetDevice(IID_PPV_ARGS(&pDeviceUnk)))) {
                    // Try to query IDXGIDevice directly from the device
                    IDXGIDevice* dxgiDevice = nullptr;
                    if (SUCCEEDED(pDeviceUnk->QueryInterface(IID_PPV_ARGS(&dxgiDevice)))) {
                        IDXGIAdapter* adapter = nullptr;
                        if (SUCCEEDED(dxgiDevice->GetAdapter(&adapter))) {
                            DXGI_ADAPTER_DESC desc;
                            if (SUCCEEDED(adapter->GetDesc(&desc))) {
                                SystemMetricsCollector::Get().Initialize((int32_t)desc.AdapterLuid.LowPart,
                                                                         (int32_t)desc.AdapterLuid.HighPart);
                                SystemMetricsCollector::Get().SetVRAMTotal(desc.DedicatedVideoMemory);
                                HookLog("DX12: SystemMetricsCollector initialized with LUID %08X:%08X, VRAM: %llu MB",
                                        desc.AdapterLuid.HighPart, desc.AdapterLuid.LowPart,
                                        desc.DedicatedVideoMemory / (1024 * 1024));
                                s_metricsInitialized = true;
                            }
                            adapter->Release();
                        }
                        dxgiDevice->Release();
                    } else {
                        HookLog("DX12: Failed to query IDXGIDevice from swap chain's device");

                        // Fallback: Create DXGI factory and use first adapter
                        // This is better than nothing - most systems have one GPU
                        IDXGIFactory1* factory = nullptr;
                        if (SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
                            IDXGIAdapter* adapter = nullptr;
                            if (SUCCEEDED(factory->EnumAdapters(0, &adapter))) {
                                DXGI_ADAPTER_DESC desc;
                                if (SUCCEEDED(adapter->GetDesc(&desc))) {
                                    SystemMetricsCollector::Get().Initialize((int32_t)desc.AdapterLuid.LowPart,
                                                                             (int32_t)desc.AdapterLuid.HighPart);
                                    SystemMetricsCollector::Get().SetVRAMTotal(desc.DedicatedVideoMemory);
                                    HookLog(
                                        "DX12: SystemMetricsCollector FALLBACK init with LUID %08X:%08X, VRAM: %llu MB",
                                        desc.AdapterLuid.HighPart, desc.AdapterLuid.LowPart,
                                        desc.DedicatedVideoMemory / (1024 * 1024));
                                    s_metricsInitialized = true;
                                }
                                adapter->Release();
                            }
                            factory->Release();
                        }
                    }
                    pDeviceUnk->Release();
                } else {
                    HookLog("DX12: Failed to get device from swap chain");
                }
            } else {
                HookLog("DX12: Failed to get swap chain desc");
            }
        }

        // DIAGNOSTIC: Check if SystemMetricsCollector has been initialized
        static bool s_metricsInitLogged = false;
        if (!s_metricsInitLogged) {
            auto metrics = SystemMetricsCollector::Get().GetMetrics();
            HookLog("DX12: DIAGNOSTIC - SystemMetricsCollector GPU usage: %.1f%%, VRAM: %.1f MB", metrics.gpuUsage,
                    metrics.vramUsed / (1024.0f * 1024.0f));
            s_metricsInitLogged = true;
        }

        if (g_Device == nullptr || activeDevice != g_Device || pSwapChain != g_LastSwapChain) {
            if (g_Device) {
                CleanupOverlay();
                CleanupRTVs();
                // ShutdownImGui(); // MIGRATED: OverlayAdapter handles cleanup
                g_SharedCaptureD3D12.Reset();
                // Also cleanup overlay queue when device changes
                if (g_OverlayQueue) {
                    g_OverlayQueue->Release();
                    g_OverlayQueue = nullptr;
                }
                g_Device->Release();
            }
            g_Device = activeDevice;
            g_Device->AddRef();
            if (g_LastSwapChain) g_LastSwapChain->Release();
            g_LastSwapChain = pSwapChain;
            g_LastSwapChain->AddRef();
            g_State.imGuiInit = false;
            DX12_ResetImGuiFrameCounter();
            HookLog("DX12: ProcessFrame - new device/swapchain, ImGui reset");

            // CRITICAL FIX: Create overlay queue IMMEDIATELY after device is set
            // This ensures the queue exists before any overlay initialization
            if (!g_OverlayQueue && g_Device) {
                D3D12_COMMAND_QUEUE_DESC qd = {};
                qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
                qd.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
                qd.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
                qd.NodeMask = 0;
                HRESULT hr = g_Device->CreateCommandQueue(&qd, IID_PPV_ARGS(&g_OverlayQueue));
                if (SUCCEEDED(hr)) {
                    g_OverlayQueue->SetName(L"CE_OverlayQueue");
                    HookLog("DX12: ProcessFrame - Created overlay queue %p (immediate after device set)",
                            g_OverlayQueue);
                } else {
                    HookLog("DX12: ProcessFrame - Failed to create overlay queue (immediate) hr=0x%08X", hr);
                }
            }
        }
        activeDevice->Release();
    }
    // Use dedicated overlay queue for all ImGui rendering
    if (!g_OverlayQueue) {
        HookLog("DX12: ProcessFrame - no overlay queue, skipping overlay");
        return;
    }
    // ImGui_ImplDX12_SetCommandQueue REMOVED: Using OverlayAdapter instead
    // if (g_State.imGuiInit) ImGui_ImplDX12_SetCommandQueue(g_OverlayQueue);

    // Minimal delay before overlay init (1 frame to allow swapchain to stabilize)
    static std::atomic<int> s_framesBeforeInit{0};
    if (!g_State.imGuiInit) {
        int frames = ++s_framesBeforeInit;
        if (frames < 1) {
            // Skip - proceed immediately
            return;
        } else if (frames == 1) {
            HookLog("DX12: ProcessFrame - Proceeding with ImGui init (frame %d)", frames);
        }

        // CRITICAL FIX: Don't initialize ImGui during FG suspension, FSR stabilization, or native FSR FG
        // This prevents initialization with potentially unstable frame generation state
        // and avoids initializing overlay resources we'll never use (native FSR FG skips rendering)
        // CRITICAL FIX: Clean up any existing ImGui context from previous swapchain
        // This happens when FSR FG recreates the swapchain and we deferred cleanup
        // MUST hold mutex to prevent race with DrawOverlay
        if (g_OverlayAdapter.IsInitialized()) {
            std::lock_guard<std::recursive_mutex> cleanupLock(g_InitImGuiMutex);
            HookLog("DX12: ProcessFrame - cleaning up stale OverlayAdapter (mutex held)");
            g_OverlayAdapter.Shutdown();
            CleanupOverlay();
            CleanupRTVs();
            HookLog("DX12: ProcessFrame - cleanup complete, proceeding with init");
        }

        DXGI_SWAP_CHAIN_DESC desc;
        if (SUCCEEDED(pSwapChain->GetDesc(&desc))) {
            g_State.cachedWidth = desc.BufferDesc.Width;
            g_State.cachedHeight = desc.BufferDesc.Height;

            // Use actual swapchain buffer count for ImGui initialization
            // The separate overlay queue (Change 1) eliminates the need for buffer limiting
            int imguiBufferCount = desc.BufferCount;

            HookLog("DX12: ProcessFrame - initializing ImGui (%dx%d, buffers=%d)", g_State.cachedWidth,
                    g_State.cachedHeight, imguiBufferCount);

            // Validate swapchain buffers are accessible before initializing
            IDXGISwapChain3* sc3 = nullptr;
            if (SUCCEEDED(pSwapChain->QueryInterface(IID_PPV_ARGS(&sc3)))) {
                int validBuffers = 0;
                for (int i = 0; i < imguiBufferCount; i++) {
                    ID3D12Resource* bb = nullptr;
                    if (SUCCEEDED(sc3->GetBuffer(i, IID_PPV_ARGS(&bb)))) {
                        if (bb) {
                            bb->Release();
                            validBuffers++;
                        }
                    } else {
                        HookLog("DX12: ProcessFrame - buffer %d not accessible, stopping validation", i);
                        break;
                    }
                }

                if (validBuffers < imguiBufferCount) {
                    HookLog("DX12: ProcessFrame - only %d/%d buffers valid, skipping ImGui init this frame",
                            validBuffers, imguiBufferCount);
                    sc3->Release();
                    return;
                }

                if (InitImGui(g_Device, imguiBufferCount, desc.BufferDesc.Format, desc.OutputWindow)) {
                    // CRITICAL FIX: Create RTVs for ALL swapchain buffers, not just ImGui count
                    // This prevents buffer index issues when swapchain has more buffers than ImGui uses
                    int actualBufferCount = desc.BufferCount;
                    if (actualBufferCount > 8) {
                        HookLog("DX12: Swapchain has %d buffers, limiting RTVs to 8", actualBufferCount);
                        actualBufferCount = 8;
                    }
                    CreateRTVs(g_Device, sc3, actualBufferCount);
                    InitOverlaySync(g_Device, imguiBufferCount);
                    HookLog("DX12: ProcessFrame - ImGui initialized with %d RTVs, syncInit=%d", actualBufferCount,
                            g_State.syncInit);

                } else {
                    HookLog("DX12: ProcessFrame - ImGui initialization FAILED");
                }
                // SAFETY: Check sc3 is still valid before releasing
                if (sc3) {
                    sc3->Release();
                }
            } else {
                HookLog("DX12: ProcessFrame - failed to get IDXGISwapChain3 interface");
            }
        } else {
            HookLog("DX12: ProcessFrame - failed to get swapchain desc");
        }
    }

    // DIAGNOSTIC: Log state before overlay block check (first 20 frames)
    static int s_preOverlayCheckCount = 0;
    if (++s_preOverlayCheckCount <= 20) {
        HookLog("DX12: ProcessFrame #%d - imGuiInit=%d, syncInit=%d, overlayQueue=%p", s_preOverlayCheckCount,
                g_State.imGuiInit, g_State.syncInit, g_OverlayQueue);
    }

    if (g_State.imGuiInit && g_State.syncInit) {
        // DIAGNOSTIC: Log entry to overlay rendering block (first 10 frames)
        static int s_overlayBlockEntryCount = 0;
        if (++s_overlayBlockEntryCount <= 10) {
            HookLog("DX12: ProcessFrame - entered overlay block #%d (fence=%p, cmdList=%p, allocCount=%zu)",
                    s_overlayBlockEntryCount, g_State.fence, g_State.cmdList, g_State.allocators.size());
        }
        // Change 6: Remove verbose per-frame logging
        int idx = g_State.allocIndex;
        g_State.allocIndex = (idx + 1) % DX12OverlayState::ALLOC_POOL_SIZE;

        bool safeToProceed = true;
        if (g_State.fence) {
            UINT64 comp = g_State.fence->GetCompletedValue();
            UINT64 target = g_State.fenceValues[idx];
            if (comp < target) {
                HRESULT hr = g_State.fence->SetEventOnCompletion(target, g_State.fenceEvent);
                if (SUCCEEDED(hr)) {
                    // Wait with timeout to prevent deadlock
                    DWORD waitResult = WaitForSingleObject(g_State.fenceEvent, 5000);
                    if (waitResult != WAIT_OBJECT_0) {
                        HookLog("DX12: Fence wait failed/timeout, result=%lu, skipping overlay this frame", waitResult);
                        safeToProceed = false;
                    }
                } else {
                    HookLog("DX12: SetEventOnCompletion failed hr=0x%08X", hr);
                    safeToProceed = false;
                }
            }
        } else {
            HookLog("DX12: No fence, cannot proceed with overlay");
            safeToProceed = false;
        }

        if (safeToProceed) {
            auto* list = g_State.cmdList;
            auto* alloc = (idx < (int)g_State.allocators.size()) ? g_State.allocators[idx] : nullptr;
            if (list && alloc) {
                HRESULT allocResetHr = alloc->Reset();
                if (SUCCEEDED(allocResetHr)) {
                    HRESULT listResetHr = list->Reset(alloc, nullptr);
                    if (SUCCEEDED(listResetHr)) {
                        IDXGISwapChain3* sc3 = nullptr;
                        if (SUCCEEDED(pSwapChain->QueryInterface(IID_PPV_ARGS(&sc3)))) {
                            UINT swapchainBufferIdx = sc3->GetCurrentBackBufferIndex();
                            // CRITICAL FIX: Use actual swapchain buffer index directly
                            // CreateRTVs now creates RTVs for all swapchain buffers (up to 8)
                            // so no need to wrap the index - this prevents sync issues
                            UINT bufferIdx = swapchainBufferIdx;
                            // Validate buffer index is within our allocated range
                            if (bufferIdx >= (UINT)g_State.bufferCount) {
                                HookLog("DX12: Buffer index %u exceeds allocated count %d, clamping", bufferIdx,
                                        g_State.bufferCount);
                                bufferIdx = g_State.bufferCount - 1;
                            }
                            ID3D12Resource* bb = nullptr;
                            if (SUCCEEDED(pSwapChain->GetBuffer(swapchainBufferIdx, IID_PPV_ARGS(&bb)))) {
                                static int s_bufferLogCount = 0;
                                if (++s_bufferLogCount <= 10) {
                                    HookLog("DX12: Render to buffer %u/%u (swapchain says %u), bb=%p", bufferIdx,
                                            g_State.bufferCount, swapchainBufferIdx, bb);
                                }
                                // Transition buffer to RENDER_TARGET for overlay rendering
                                {
                                    D3D12_RESOURCE_BARRIER b = {D3D12_RESOURCE_BARRIER_TYPE_TRANSITION,
                                                                D3D12_RESOURCE_BARRIER_FLAG_NONE,
                                                                {bb, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                                                                 D3D12_RESOURCE_STATE_PRESENT,
                                                                 D3D12_RESOURCE_STATE_RENDER_TARGET}};
                                    list->ResourceBarrier(1, &b);
                                }
                                D3D12_CPU_DESCRIPTOR_HANDLE rtv =
                                    g_State.rtvDescHeap->GetCPUDescriptorHandleForHeapStart();
                                rtv.ptr += (SIZE_T)bufferIdx * g_State.rtvDescriptorSize;
                                g_Device->CreateRenderTargetView(bb, nullptr, rtv);
                                list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
                                // DEBUG: Clear to red to verify we're rendering to the right buffer
                                static int s_clearCount = 0;
                                if (++s_clearCount <= 5) {
                                    float red[] = {1.0f, 0.0f, 0.0f, 1.0f};  // Full opacity red
                                    list->ClearRenderTargetView(rtv, red, 0, nullptr);
                                    HookLog("DX12: DEBUG - Cleared buffer %u to RED (rtv.ptr=%p)", bufferIdx, rtv.ptr);
                                }
                                D3D12_VIEWPORT vp = {0, 0, (float)g_State.cachedWidth, (float)g_State.cachedHeight,
                                                     0, 1};
                                list->RSSetViewports(1, &vp);
                                D3D12_RECT scissor = {0, 0, (LONG)g_State.cachedWidth, (LONG)g_State.cachedHeight};
                                list->RSSetScissorRects(1, &scissor);
                                DrawOverlay(list, processCapture, bufferIdx);
                                // Re-enabled transitions after debugging confirmed barriers are NOT the issue
                                {
                                    D3D12_RESOURCE_BARRIER b = {D3D12_RESOURCE_BARRIER_TYPE_TRANSITION,
                                                                D3D12_RESOURCE_BARRIER_FLAG_NONE,
                                                                {bb, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                                                                 D3D12_RESOURCE_STATE_RENDER_TARGET,
                                                                 D3D12_RESOURCE_STATE_PRESENT}};
                                    list->ResourceBarrier(1, &b);
                                }
                                HRESULT closeHr = list->Close();
                                static int s_closeCount = 0;
                                if (++s_closeCount <= 10) {
                                    HookLog("DX12: cmdList->Close hr=0x%08X, overlayQueue=%p, fence=%p", closeHr,
                                            g_OverlayQueue, g_State.fence);
                                }
                                // Use overlay queue for rendering - game queue causes device removal
                                if (SUCCEEDED(closeHr) && g_OverlayQueue && g_State.fence) {
                                    ID3D12CommandList* lists[] = {list};
                                    g_OverlayQueue->ExecuteCommandLists(1, lists);

                                    // Signal fence from overlay queue
                                    g_State.currentFenceValue++;
                                    g_State.fenceValues[idx] = g_State.currentFenceValue;
                                    g_OverlayQueue->Signal(g_State.fence, g_State.currentFenceValue);

                                    static int s_execCount = 0;
                                    if (++s_execCount <= 10) {
                                        HookLog("DX12: Overlay submitted to queue %p", g_OverlayQueue);
                                    }

                                    // CRITICAL: Make the game's queue wait for overlay completion
                                    // This ensures overlay is visible before Present
                                    ID3D12CommandQueue* gameQueue = nullptr;
                                    {
                                        std::lock_guard<std::recursive_mutex> ql(g_CommandQueueMutex);
                                        gameQueue = g_CommandQueue;
                                    }
                                    if (gameQueue) {
                                        HRESULT waitHr = gameQueue->Wait(g_State.fence, g_State.currentFenceValue);
                                        if (SUCCEEDED(waitHr)) {
                                            if (s_execCount <= 10) {
                                                HookLog("DX12: Game queue waiting for overlay fence %llu",
                                                        g_State.currentFenceValue);
                                            }
                                        }
                                    }
                                } else if (!g_OverlayQueue) {
                                    static int s_noQueueCount = 0;
                                    if (++s_noQueueCount <= 5) {
                                        HookLog("DX12: Overlay skipped - no overlay queue");
                                    }
                                }

                                if (bb) bb->Release();
                            }
                            sc3->Release();
                        } else {
                            static int s_sc3FailCount = 0;
                            if (++s_sc3FailCount <= 3) {
                                HookLog("DX12: ProcessFrame - failed to get SwapChain3 interface");
                            }
                        }
                    } else {
                        static int s_listResetFailCount = 0;
                        if (++s_listResetFailCount <= 3) {
                            HookLog("DX12: ProcessFrame - list->Reset failed hr=0x%08X", listResetHr);
                        }
                    }
                } else {
                    static int s_allocResetFailCount = 0;
                    if (++s_allocResetFailCount <= 3) {
                        HookLog("DX12: ProcessFrame - alloc->Reset failed hr=0x%08X", allocResetHr);
                    }
                }
            } else {
                static int s_listAllocNullCount = 0;
                if (++s_listAllocNullCount <= 3) {
                    HookLog("DX12: ProcessFrame - null list or alloc (list=%p, alloc=%p, idx=%d, allocSize=%zu)", list,
                            alloc, idx, g_State.allocators.size());
                }
            }
        } else {
            static int s_notSafeCount = 0;
            if (++s_notSafeCount <= 3) {
                HookLog("DX12: ProcessFrame - safeToProceed=false, skipping overlay");
            }
        }
    }

    // Change 6: Remove verbose debug logging - keep only error logging
    if (processCapture && g_IPC && g_IPC->IsRecording()) {
        SharedMemoryLayout* shm = g_IPC->GetSharedMem();
        if (shm) {
            if (!g_SharedCaptureD3D12.IsActive()) g_SharedCaptureD3D12.Initialize(g_Device, pSwapChain);
            if (g_SharedCaptureD3D12.IsActive()) {
                std::lock_guard<std::recursive_mutex> capLock(g_DX12CaptureMutex);
                IDXGISwapChain3* sc3 = nullptr;
                pSwapChain->QueryInterface(IID_PPV_ARGS(&sc3));
                UINT bbIdx = sc3 ? sc3->GetCurrentBackBufferIndex() : 0;
                if (sc3) sc3->Release();
                // Capture uses the game's command queue (g_CommandQueue) for synchronization
                ID3D12CommandQueue* captureQueue = nullptr;
                {
                    std::lock_guard<std::recursive_mutex> ql(g_CommandQueueMutex);
                    captureQueue = g_CommandQueue;
                }
                if (captureQueue && g_SharedCaptureD3D12.CaptureFrame(captureQueue, bbIdx)) {
                    SharedFrameDescriptor desc;
                    if (g_SharedCaptureD3D12.GetCurrentFrame(&desc)) {
                        shm->SetSharedHandle(0, (uint64_t)g_SharedCaptureD3D12.GetSharedHandle(0));
                        shm->SetSharedHandle(1, (uint64_t)g_SharedCaptureD3D12.GetSharedHandle(1));
                        shm->SetFenceShareHandle((uint64_t)g_SharedCaptureD3D12.GetFenceShareHandle());
                        shm->SetWidth(desc.width);
                        shm->SetHeight(desc.height);
                        shm->SetFormat(desc.format);
                        // CRITICAL FIX: Use acquire ordering to see consumer's readIndex updates
                        uint32_t wIdx = shm->frameRing.writeIndex.load(std::memory_order_acquire);
                        uint32_t rIdx = shm->frameRing.readIndex.load(std::memory_order_acquire);
                        if ((uint32_t)(wIdx - rIdx) < (uint32_t)FRAME_RING_SIZE) {
                            FrameSlot& slot = shm->frameRing.slots[wIdx % FRAME_RING_SIZE];
                            slot.fenceValue = desc.fenceValue;
                            slot.timestamp = desc.presentTime;
                            slot.frameIndex = desc.frameNumber;
                            slot.textureIndex = desc.textureIndex;
                            slot.sourcePid = GetCurrentProcessId();
                            slot.valid.store(1, std::memory_order_release);
                            shm->frameRing.writeIndex.store(wIdx + 1, std::memory_order_release);
                        } else
                            shm->frameRing.droppedFrames.fetch_add(1, std::memory_order_relaxed);
                    }
                }
            }
        }
    }
}

// Delay overlay rendering for first frames after ImGui init
// This prevents GPU crashes when frame generation tech (DLSS FG/FSR FG) is initializing
static std::atomic<bool> s_initDelayComplete{false};
static std::atomic<int> s_framesBeforeInit{0};  // Defined earlier in ProcessFrame

void DX12_ResetImGuiFrameCounter()
{
    s_framesBeforeInit = 0;
    // Also reset the post-init frame counter
    s_framesSinceInit = 0;
    HookLog("DX12: Reset ImGui frame counter");
}

void DX12_ResetOverlayFrameDelay()
{
    s_framesSinceInit = 0;
    s_initDelayComplete = false;
    HookLog("DX12: Reset overlay frame delay counter");
}

void DX12_ProcessFrameExternal(IDXGISwapChain* pSwapChain)
{
    // CRITICAL: Heartbeat FIRST - before ANY checks that might early-return
    // This ensures the freeze watchdog gets heartbeats even with FSR/DLSS FG active
    g_RenderWatchdog.Heartbeat();

    // CRITICAL FIX: Reset delay flag when ImGui is not initialized
    // This ensures we wait again after each init
    if (!g_State.imGuiInit) {
        s_initDelayComplete = false;
        s_framesSinceInit = 0;
    }

    // Minimal delay after ImGui init before rendering overlay (for stability)
    if (g_State.imGuiInit && !s_initDelayComplete.load()) {
        int frames = ++s_framesSinceInit;
        if (frames < 1) {
            // Skip - proceed immediately
            return;
        } else {
            s_initDelayComplete = true;
            HookLog("DX12: ProcessFrameExternal - Overlay rendering enabled (frame %d after init)", frames);
        }
    }

    // CRITICAL FIX: Dynamically detect Vulkan WSI swapchains
    // When NVIDIA's Vulkan WSI-to-DXGI mapping is active, the swapchain is presented through
    // DXGI but the device is not a real D3D12 device we can render to. Check this dynamically
    // because games can switch between Vulkan WSI (focused) and DXGI (unfocused) modes.
    static bool s_checkedForVulkan = false;
    static bool s_vulkanLayerActive = false;
    if (!s_checkedForVulkan) {
        HMODULE hVulkanLayer = GetModuleHandleW(L"VK_LAYER_CE_overlay.dll");
        if (!hVulkanLayer) {
            hVulkanLayer = GetModuleHandleW(L"VK_LAYER_CE_overlay_x86.dll");
        }
        s_vulkanLayerActive = (hVulkanLayer != nullptr);
        if (s_vulkanLayerActive) {
            HookLog("DX12: Vulkan layer detected, will skip DXGI overlay for Vulkan WSI swapchains");
        }
        s_checkedForVulkan = true;
    }

    // If Vulkan layer is active, check if this is a Vulkan WSI swapchain
    // by attempting to get the D3D12 device - Vulkan WSI swapchains will fail
    // or return a device we can't use for rendering
    if (s_vulkanLayerActive && pSwapChain) {
        ID3D12Device* pDevice = nullptr;
        HRESULT hr = pSwapChain->GetDevice(IID_PPV_ARGS(&pDevice));
        if (FAILED(hr) || !pDevice) {
            // This is likely a Vulkan WSI swapchain - skip DX12 overlay
            // The Vulkan layer will handle overlay rendering
            return;
        }
        // Check if we can actually use this device (Vulkan WSI devices may fail here)
        D3D12_FEATURE_DATA_FEATURE_LEVELS featureLevels = {};
        hr = pDevice->CheckFeatureSupport(D3D12_FEATURE_FEATURE_LEVELS, &featureLevels, sizeof(featureLevels));
        pDevice->Release();
        if (FAILED(hr)) {
            // Vulkan WSI device that doesn't support full D3D12 features
            return;
        }
    }

    // Update freeze watchdog heartbeat
    g_RenderWatchdog.Heartbeat();

    // CRITICAL DEBUG: This log MUST appear if the function is called
    static int s_callCount = 0;
    if (++s_callCount <= 10) {
        HookLog("DX12: ProcessFrameExternal ENTERED call #%d", s_callCount);
    }

    if (!pSwapChain) {
        HookLog("DX12: ProcessFrameExternal - null swapchain");
        return;
    }
    IDXGISwapChain3* sc3 = nullptr;
    if (FAILED(pSwapChain->QueryInterface(IID_PPV_ARGS(&sc3))) || !sc3) {
        HookLog("DX12: ProcessFrameExternal - failed to get SwapChain3");
        return;
    }
    int count = g_CommandListsExecutedThisFrame.exchange(0);
    uint64_t frameNum = ++g_FGDebugFrameCount;
    g_FGCompat.RecordFrame(count);
    // Change 6: Only log frame info periodically to reduce log spam
    static uint64_t s_lastLoggedFrame = 0;
    if (frameNum - s_lastLoggedFrame >= 60) {
        HookLog("DX12: ProcessFrameExternal - Frame %llu, cmdLists=%d, isReal=%d", frameNum, count, count > 0);
        s_lastLoggedFrame = frameNum;
    }
    // Overlay renders on ALL frames (real + interpolated)
    bool isRealFrame = count > 0;
    ProcessFrame(sc3, isRealFrame);
    sc3->Release();
}

namespace DXGIShared {
void HandleDX12ProcessFrame(IDXGISwapChain* pSwapChain, bool isRealFrame) { DX12_ProcessFrameExternal(pSwapChain); }
void HandleDX12ResizeBegin() { DX12_OnSwapchainResizeBegin(); }
void HandleDX12ResizeEnd() { DX12_OnSwapchainResizeEnd(); }
}  // namespace DXGIShared

// External function for swapchain wrapper to wait for overlay completion before Present
extern "C" __declspec(dllexport) void DX12_WaitForOverlayCompletion(ID3D12CommandQueue* pGameQueue)
{
    if (!pGameQueue || !g_State.fence) return;

    // Get the most recent fence value that was signaled by the overlay queue
    // This is accessed from Present thread, written from render thread - use atomic load
    UINT64 fenceValueToWait = g_State.currentFenceValue;
    if (fenceValueToWait == 0) return;  // No overlay work submitted yet

    // Make the game's queue wait for the overlay fence
    // This ensures overlay rendering completes before Present
    HRESULT hr = pGameQueue->Wait(g_State.fence, fenceValueToWait);
    if (FAILED(hr)) {
        static int s_failCount = 0;
        if (++s_failCount <= 5) {
            HookLog("DX12: WaitForOverlay - Wait failed hr=0x%08X", hr);
        }
    } else {
        static int s_successCount = 0;
        if (++s_successCount <= 5) {
            HookLog("DX12: WaitForOverlay - Queue %p waiting for fence value %llu", pGameQueue, fenceValueToWait);
        }
    }
}

static const GUID SKID_D3D12SwapChainBufferBitmap = {
    0xbc53df3b, 0x956f, 0x47db, {0xa6, 0x53, 0x5, 0xd7, 0xb8, 0x71, 0x53, 0x38}};
static std::atomic<int> g_ECLCallCount{0};

void STDMETHODCALLTYPE DetourExecuteCommandLists(ID3D12CommandQueue* pThis, UINT NumCommandLists,
                                                 ID3D12CommandList* const* ppCommandLists)
{
    // Heartbeat for freeze watchdog - ExecuteCommandLists is called frequently
    g_RenderWatchdog.Heartbeat();

    // Debug: Log first few calls to verify hook is working
    int count = ++g_ECLCallCount;
    if (count <= 5) {
        HookLog("DX12: ExecuteCommandLists called #%d (queue=%p)", count, pThis);
    }

    // FIX: Count command lists from ALL queues EXCEPT our overlay queue
    // This ensures isReal frames are detected correctly
    // The game command queue identification is NOT needed - we just exclude our own overlay queue
    if (pThis != g_OverlayQueue) {
        g_CommandListsExecutedThisFrame.fetch_add(NumCommandLists, std::memory_order_relaxed);

        // FIX: Register game's queue for overlay execution
        // Now that we use game's queue instead of separate overlay queue, we need to capture it
        DX12_SetCommandQueue(pThis);
    }

    // NOTE: Removed complex swapchain-based queue detection to prevent crashes
    // The isReal detection works without needing to identify the game's command queue
    // We simply count command lists from all queues except our overlay queue

    if (oExecuteCommandLists) oExecuteCommandLists(pThis, NumCommandLists, ppCommandLists);
}

void DX12_HookQueueVTable(ID3D12CommandQueue* queue)
{
    if (!queue) return;

    // We ALWAYS hook the queue for freeze detection heartbeat
    // The overlay rendering is skipped separately in ProcessFrameExternal if needed
    // This ensures freeze watchdog works even with DLSS/FSR FG

    void* unwrapped = nullptr;
    static const GUID IID_CWrapD3D12CommandQueue = {
        0xd4e5f678, 0x90ab, 0xcdef, {0x12, 0x34, 0x56, 0x78, 0x90, 0x12, 0x34, 0x56}};
    if (SUCCEEDED(queue->QueryInterface(IID_CWrapD3D12CommandQueue, &unwrapped))) {
        ((IUnknown*)unwrapped)->Release();
        return;
    }
    static std::recursive_mutex s_HookMutex;
    std::lock_guard<std::recursive_mutex> lock(s_HookMutex);
    void** vtbl = *reinterpret_cast<void***>(queue);
    if (vtbl[10] != (void*)DetourExecuteCommandLists) {
        HookLog("DX12: Hooking ExecuteCommandLists vtable for queue %p", queue);
        VTableHook::Create(&vtbl[10], (LPVOID)DetourExecuteCommandLists, (LPVOID*)&oExecuteCommandLists);
    }
}

HRESULT STDMETHODCALLTYPE DetourCreateSwapChain(IDXGIFactory* pThis, IUnknown* pDevice, DXGI_SWAP_CHAIN_DESC* pDesc,
                                                IDXGISwapChain** ppSwapChain)
{
    // CRITICAL: ALWAYS hook the command queue vtable for frame detection
    if (pDevice) {
        ID3D12CommandQueue* q = nullptr;
        if (SUCCEEDED(pDevice->QueryInterface(IID_PPV_ARGS(&q)))) {
            DX12_HookQueueVTable(q);
            q->Release();
        }
    }

    HRESULT hr = oCreateSwapChain(pThis, pDevice, pDesc, ppSwapChain);
    if (SUCCEEDED(hr) && ppSwapChain && *ppSwapChain) {
        IDXGISwapChain3* sc3 = nullptr;
        if (SUCCEEDED((*ppSwapChain)->QueryInterface(IID_PPV_ARGS(&sc3)))) {
            DXGIShared::InstallHooks(sc3);
            sc3->Release();
        }
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE DetourCreateSwapChainForHwnd(IDXGIFactory2* pThis, IUnknown* pDevice, HWND hWnd,
                                                       const DXGI_SWAP_CHAIN_DESC1* pDesc,
                                                       const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* pFDesc, IDXGIOutput* pOut,
                                                       IDXGISwapChain1** ppSC)
{
    // CRITICAL: ALWAYS hook the command queue vtable for frame detection
    if (pDevice) {
        ID3D12CommandQueue* q = nullptr;
        if (SUCCEEDED(pDevice->QueryInterface(IID_PPV_ARGS(&q)))) {
            DX12_HookQueueVTable(q);
            q->Release();
        }
    }

    HRESULT hr = oCreateSwapChainForHwnd(pThis, pDevice, hWnd, pDesc, pFDesc, pOut, ppSC);

    // ALWAYS install Present hooks - overlay renders on ALL frames (real + interpolated)
    if (SUCCEEDED(hr) && ppSC && *ppSC) {
        IDXGISwapChain3* sc3 = nullptr;
        if (SUCCEEDED((*ppSC)->QueryInterface(IID_PPV_ARGS(&sc3)))) {
            DXGIShared::InstallHooks(sc3);
            sc3->Release();
        }
    }
    return hr;
}

void STDMETHODCALLTYPE DetourCreateSampler(ID3D12Device* pDevice, const D3D12_SAMPLER_DESC* pDesc,
                                           D3D12_CPU_DESCRIPTOR_HANDLE DestDescriptor)
{
    if (oCreateSampler) oCreateSampler(pDevice, pDesc, DestDescriptor);
}
HRESULT WINAPI DetourSerializeRootSignature(const D3D12_ROOT_SIGNATURE_DESC* pRootSignature,
                                            D3D_ROOT_SIGNATURE_VERSION Version, ID3DBlob** ppBlob,
                                            ID3DBlob** ppErrorBlob)
{
    if (oSerializeRootSignature) return oSerializeRootSignature(pRootSignature, Version, ppBlob, ppErrorBlob);
    return E_FAIL;
}
HRESULT WINAPI DetourSerializeVersionedRootSignature(const D3D12_VERSIONED_ROOT_SIGNATURE_DESC* pRootSignature,
                                                     ID3DBlob** ppBlob, ID3DBlob** ppErrorBlob)
{
    if (oSerializeVersionedRootSignature) return oSerializeVersionedRootSignature(pRootSignature, ppBlob, ppErrorBlob);
    return E_FAIL;
}
HRESULT STDMETHODCALLTYPE DetourCreateCommittedResource(ID3D12Device* device,
                                                        const D3D12_HEAP_PROPERTIES* pHeapProperties,
                                                        D3D12_HEAP_FLAGS HeapFlags, const D3D12_RESOURCE_DESC* pDesc,
                                                        D3D12_RESOURCE_STATES InitialResourceState,
                                                        const D3D12_CLEAR_VALUE* pOptimizedClearValue,
                                                        REFIID riidResource, void** ppvResource)
{
    if (oCreateCommittedResource)
        return oCreateCommittedResource(device, pHeapProperties, HeapFlags, pDesc, InitialResourceState,
                                        pOptimizedClearValue, riidResource, ppvResource);
    return E_FAIL;
}

void DX12Hook::Shutdown()
{
    CleanupResources();
    // ShutdownImGui(); // MIGRATED: OverlayAdapter handles cleanup
    CleanupOverlay();
    CleanupRTVs();
    {
        std::lock_guard<std::recursive_mutex> lock(g_DeviceQueuesMutex);
        for (auto& pair : g_DeviceQueues)
            if (pair.second) pair.second->Release();
        g_DeviceQueues.clear();
    }
    if (g_CommandQueue) {
        g_CommandQueue->Release();
        g_CommandQueue = nullptr;
    }
    if (g_Device) {
        g_Device->Release();
        g_Device = nullptr;
    }
    if (g_LastSwapChain) {
        g_LastSwapChain->Release();
        g_LastSwapChain = nullptr;
    }
    if (g_SharedCaptureD3D12.IsActive()) g_SharedCaptureD3D12.Reset();
    g_IPCReady = false;
}

void DX12Hook::OnHostDisconnect() { g_IPCReady = false; }
void DX12Hook::TrackResource(IUnknown* res)
{
    if (!res) return;
    std::lock_guard<std::recursive_mutex> lock(resourceMutex);
    res->AddRef();
    trackedResources.push_back(res);
}
void DX12Hook::CleanupResources()
{
    std::lock_guard<std::recursive_mutex> lock(resourceMutex);
    for (auto* res : trackedResources)
        if (res) res->Release();
    trackedResources.clear();
}

// FIXED: Clean up the global hook instance if allocated
DWORD WINAPI UnloadThread(LPVOID lpParam)
{
    Sleep(200);
    if (g_dx12HookInstance) {
        g_dx12HookInstance->Shutdown();
        delete g_dx12HookInstance;
        g_dx12HookInstance = nullptr;
    }
    return 0;
}
