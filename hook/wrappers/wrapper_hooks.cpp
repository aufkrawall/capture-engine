/**
 * Wrapper Hook Entry Points Implementation
 */

#include <atomic>
#include <cstdarg>
#include <cstdio>

// Include Windows header for MinGW compatibility
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#ifndef DIRECTDRAW_VERSION
#define DIRECTDRAW_VERSION 0x0700
#endif
#include <ddraw.h>
#define CE_WRAPPER_RETURN_ADDRESS() __builtin_return_address(0)

// Forward declaration from dx12_hook.cpp
extern void EnsureDX12Hook();
struct IDXGISwapChain;
// Forward declaration from dx11_hook.cpp
extern void DX11Hook_OnSwapChainCreated(IDXGISwapChain* pSwapChain);
#include "../apis/ddraw_hook.h"
#include "../apis/dx11_hook.h"
#include "../apis/dx12_hook.h"  // Access to g_DX12Hook implementation
#include "../common/dx12_dred.h"
#include "../common/dx12_overlay_policy.h"
#include "../common/fg_detection.h"
#include "../common/hook_common.h"
#include "../common/overlay_compat.h"
#include "../common/streamline_runtime_policy.h"
#include "d3d10_device_wrap.h"

// Returns true if the given return address is inside a Streamline module
// (sl.common, sl.interposer, sl.dlss_g, etc.).  SL creates internal DXGI
// factories during DllMain for its own plumbing.  Wrapping those and installing
// Present hooks triggers Steam's overlay on the SL worker thread where Steam is
// uninitialized, causing a null pointer crash inside OverlayHookD3D3.
static bool IsCallerFromStreamlineModule(const void* returnAddress, char* callerPathOut = nullptr,
                                         size_t callerPathOutCount = 0) {
    if (callerPathOut && callerPathOutCount > 0) {
        callerPathOut[0] = '\0';
    }
    HMODULE callerMod = nullptr;
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<const char*>(returnAddress), &callerMod) ||
        !callerMod) {
        return false;
    }
    char callerPath[MAX_PATH] = {};
    if (!GetModuleFileNameA(callerMod, callerPath, sizeof(callerPath))) {
        return false;
    }
    if (callerPathOut && callerPathOutCount > 0) {
        strncpy_s(callerPathOut, callerPathOutCount, callerPath, _TRUNCATE);
    }
    return ce::streamline_runtime_policy::IsStreamlineModuleNameForFeatureHooking(callerPath);
}

static bool IsCallerFromFFXFrameGenerationModule(const void* returnAddress, char* callerPathOut = nullptr,
                                                 size_t callerPathOutCount = 0) {
    if (callerPathOut && callerPathOutCount > 0) {
        callerPathOut[0] = '\0';
    }
    HMODULE callerMod = nullptr;
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<const char*>(returnAddress), &callerMod) ||
        !callerMod) {
        return false;
    }
    char callerPath[MAX_PATH] = {};
    if (!GetModuleFileNameA(callerMod, callerPath, sizeof(callerPath))) {
        return false;
    }
    if (callerPathOut && callerPathOutCount > 0) {
        strncpy_s(callerPathOut, callerPathOutCount, callerPath, _TRUNCATE);
    }
    return ce::overlay_compat::IsFFXFrameGenerationModulePath(callerPath);
}

static bool IsStreamlineRuntimeLoadedForFactoryBypass() {
    return GetModuleHandleA("sl.interposer.dll") != nullptr || GetModuleHandleA("sl.dlss_g.dll") != nullptr ||
           GetModuleHandleA("sl.common.dll") != nullptr;
}

struct DXGIFactoryRuntimeDecision {
    bool bypassWrapper = false;
    bool callerFromStreamline = false;
    bool callerFromFFX = false;
    bool streamlinePresent = false;
    bool fsrPresent = false;
    ce::fg_runtime::RuntimeMode runtimeMode = ce::fg_runtime::RuntimeMode::kOff;
    char callerPath[MAX_PATH] = {};
};

static DXGIFactoryRuntimeDecision EvaluateDXGIFactoryRuntimeDecision(const void* returnAddress,
                                                                     const char* functionName) {
    DXGIFactoryRuntimeDecision decision = {};
    char streamlineCallerPath[MAX_PATH] = {};
    char ffxCallerPath[MAX_PATH] = {};
    decision.callerFromStreamline =
        IsCallerFromStreamlineModule(returnAddress, streamlineCallerPath, sizeof(streamlineCallerPath));
    decision.callerFromFFX = IsCallerFromFFXFrameGenerationModule(returnAddress, ffxCallerPath, sizeof(ffxCallerPath));
    if (decision.callerFromStreamline) {
        strncpy_s(decision.callerPath, sizeof(decision.callerPath), streamlineCallerPath, _TRUNCATE);
    } else if (decision.callerFromFFX) {
        strncpy_s(decision.callerPath, sizeof(decision.callerPath), ffxCallerPath, _TRUNCATE);
    } else if (returnAddress) {
        ce::overlay_compat::TryGetModulePathFromCodeAddress(returnAddress, decision.callerPath,
                                                            sizeof(decision.callerPath));
    }

    const bool callerFromFrameGenerationRuntime = decision.callerFromStreamline || decision.callerFromFFX;
    const bool streamlinePresent = g_FGCompat.HasStreamlineSupport() || IsStreamlineRuntimeLoadedForFactoryBypass();
    const bool fsrPresent = g_FGCompat.HasFSRFGSupport();
    const auto runtimeMode = g_FGCompat.GetRuntimeMode();
    decision.streamlinePresent = streamlinePresent;
    decision.fsrPresent = fsrPresent;
    decision.runtimeMode = runtimeMode;
    decision.bypassWrapper = ce::dx12_overlay_policy::ShouldBypassDXGIFactoryWrapperForFrameGenerationRuntime(
        callerFromFrameGenerationRuntime, streamlinePresent, fsrPresent, runtimeMode);
    if (decision.bypassWrapper) {
        static std::atomic<int> s_factoryBypassLogCount{0};
        const int logCount = s_factoryBypassLogCount.fetch_add(1, std::memory_order_relaxed);
        if (logCount < 20 || (logCount % 256) == 0) {
            WrapperLog(
                "%s: Returning unwrapped DXGI factory for frame-generation runtime compatibility "
                "(callerSL=%d callerFFX=%d streamlinePresent=%d fsrPresent=%d runtime=%s caller=%s)",
                functionName ? functionName : "CreateDXGIFactory", decision.callerFromStreamline ? 1 : 0,
                decision.callerFromFFX ? 1 : 0, streamlinePresent ? 1 : 0, fsrPresent ? 1 : 0,
                ce::fg_runtime::GetRuntimeModeName(runtimeMode),
                decision.callerPath[0] ? decision.callerPath : "unresolved");
        }
    }
    return decision;
}
#include "d3d11_device_wrap.h"
#include "d3d11_devicecontext_wrap.h"
#include "d3d9_device_wrap.h"
#include "d3d9_wrap.h"
#include "dxgi_factory_wrap.h"
#include "dxgi_swapchain_wrap.h"
#include "iat_hook.h"
#include "wrapper_hooks.h"
// Forward declaration from dx11_hook.cpp (after D3D11 types are available)
extern void DX11Hook_InstallDeviceAndContextHooks(ID3D11Device* pDevice, ID3D11DeviceContext* pContext,
                                                  IDXGISwapChain* pSwapChain);

static bool ApplyD3D11CreateDeviceSwapChainBackbufferOverride(DXGI_SWAP_CHAIN_DESC& desc) {
    const auto& gfx = GetActiveGraphicsConfig();
    if (!HasBackbufferCountOverride(gfx.backbufferCount)) {
        return false;
    }

    const UINT requested = static_cast<UINT>(gfx.backbufferCount);
    const bool isFlip =
        (desc.SwapEffect == DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL || desc.SwapEffect == DXGI_SWAP_EFFECT_FLIP_DISCARD);
    if (isFlip)
        desc.Flags |= DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
    if (isFlip && requested < desc.BufferCount) {
        desc.Flags |= DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
        WrapperLog(
            "Wrapped_D3D11CreateDeviceAndSwapChain: BufferCount override skipped requested=%u game=%u "
            "swapEffect=%d (flip model)",
            requested, desc.BufferCount, desc.SwapEffect);
        return false;
    }
    if (desc.BufferCount == requested) {
        WrapperLog("Wrapped_D3D11CreateDeviceAndSwapChain: BufferCount already matches requested=%u swapEffect=%d",
                   requested, desc.SwapEffect);
        return false;
    }

    WrapperLog("Wrapped_D3D11CreateDeviceAndSwapChain: BufferCount override %u -> %u swapEffect=%d", desc.BufferCount,
               requested, desc.SwapEffect);
    desc.BufferCount = requested;
    return true;
}

// ============================================================================
// Original Function Pointers
// ============================================================================

PFN_CreateDXGIFactory oCreateDXGIFactory = nullptr;
PFN_CreateDXGIFactory1 oCreateDXGIFactory1 = nullptr;
PFN_CreateDXGIFactory2 oCreateDXGIFactory2 = nullptr;
#ifdef ENABLE_D3D12_WRAPPER
PFN_D3D12CreateDevice oD3D12CreateDevice = nullptr;
#endif

// D3D11 function pointers
typedef HRESULT(WINAPI* PFN_D3D11CreateDevice)(IDXGIAdapter* pAdapter, D3D_DRIVER_TYPE DriverType, HMODULE Software,
                                               UINT Flags, const D3D_FEATURE_LEVEL* pFeatureLevels, UINT FeatureLevels,
                                               UINT SDKVersion, ID3D11Device** ppDevice,
                                               D3D_FEATURE_LEVEL* pFeatureLevel,
                                               ID3D11DeviceContext** ppImmediateContext);

typedef HRESULT(WINAPI* PFN_D3D11CreateDeviceAndSwapChain)(IDXGIAdapter* pAdapter, D3D_DRIVER_TYPE DriverType,
                                                           HMODULE Software, UINT Flags,
                                                           const D3D_FEATURE_LEVEL* pFeatureLevels, UINT FeatureLevels,
                                                           UINT SDKVersion, const DXGI_SWAP_CHAIN_DESC* pSwapChainDesc,
                                                           IDXGISwapChain** ppSwapChain, ID3D11Device** ppDevice,
                                                           D3D_FEATURE_LEVEL* pFeatureLevel,
                                                           ID3D11DeviceContext** ppImmediateContext);

// Removed static to allow external access (match wrapper_hooks.h)
PFN_D3D11CreateDevice oD3D11CreateDevice = nullptr;
// PFN_D3D11CreateDeviceAndSwapChain oD3D11CreateDeviceAndSwapChain = nullptr;
// // Defined in dx11_hook.cpp

// D3D10 function pointers definitions
// Removed static
PFN_D3D10CreateDevice oD3D10CreateDevice = nullptr;
PFN_D3D10CreateDevice1 oD3D10CreateDevice1 = nullptr;
PFN_D3D10CreateDeviceAndSwapChain oD3D10CreateDeviceAndSwapChain = nullptr;
PFN_D3D10CreateDeviceAndSwapChain1 oD3D10CreateDeviceAndSwapChain1 = nullptr;

// D3D9 function pointers
typedef IDirect3D9*(WINAPI* PFN_Direct3DCreate9)(UINT SDKVersion);
typedef HRESULT(WINAPI* PFN_Direct3DCreate9Ex)(UINT SDKVersion, IDirect3D9Ex** ppD3D);

PFN_Direct3DCreate9 oDirect3DCreate9 = nullptr;
PFN_Direct3DCreate9Ex oDirect3DCreate9Ex = nullptr;
PFN_DirectDrawCreate oDirectDrawCreate = nullptr;
PFN_DirectDrawCreateEx oDirectDrawCreateEx = nullptr;

static bool g_WrappersActive = false;

// API Detection Flags - set when actual device creation is called
// These allow distinguishing between DLL being loaded (e.g. d3d12.dll via
// D3D11On12) vs the app actually using that API.
static std::atomic<bool> g_D3D11Or10DeviceCreated{false};
static std::atomic<bool> g_D3D12DeviceCreated{false};

namespace {
thread_local int s_D3D10CreateDepth = 0;

class D3D10CreateScope {
public:
    D3D10CreateScope() {
        ++s_D3D10CreateDepth;
    }
    ~D3D10CreateScope() {
        --s_D3D10CreateDepth;
    }
};

bool IsInD3D10CreateScope() {
    return s_D3D10CreateDepth > 0;
}
}  // namespace

bool WasD3D11Or10DeviceCreated() {
    return g_D3D11Or10DeviceCreated.load(std::memory_order_acquire);
}

bool WasD3D12DeviceCreated() {
    return g_D3D12DeviceCreated.load(std::memory_order_acquire);
}

void MarkD3D12DeviceCreated() {
    g_D3D12DeviceCreated.store(true, std::memory_order_release);
}

void WrapperLog(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    char buf[4096];
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);

    // Log to HookLogImportant (bypasses the shared-memory log level filter)
    HookLogImportant("%s", buf);
}

// ============================================================================
// Wrapped DXGI Factory Creation
// ============================================================================

// Resolve the true (pre-hook) address of a dxgi.dll export by reading the
// on-disk PE export table.  Third-party overlays (e.g., Steam) may redirect
// dxgi.dll's in-memory EAT to their own code; loading the file as an image
// resource gives us the original function RVA to apply against the loaded base.
static void* GetUnhookedDXGIExport(const char* funcName) {
    HMODULE hDXGI = GetModuleHandleA("dxgi.dll");
    if (!hDXGI)
        return nullptr;

    char path[MAX_PATH] = {};
    if (!GetModuleFileNameA(hDXGI, path, sizeof(path)))
        return nullptr;

    HMODULE hDisk = LoadLibraryExA(path, nullptr, LOAD_LIBRARY_AS_IMAGE_RESOURCE | LOAD_LIBRARY_AS_DATAFILE);
    if (!hDisk)
        return nullptr;

    // Low bits encode the load type; mask them to get the actual image base.
    auto base = reinterpret_cast<const BYTE*>((uintptr_t)hDisk & ~(uintptr_t)3);
    void* result = nullptr;

    auto dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic == IMAGE_DOS_SIGNATURE) {
        auto nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
        const auto& ed = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
        if (ed.VirtualAddress) {
            auto exp = reinterpret_cast<const IMAGE_EXPORT_DIRECTORY*>(base + ed.VirtualAddress);
            auto names = reinterpret_cast<const DWORD*>(base + exp->AddressOfNames);
            auto ordinals = reinterpret_cast<const WORD*>(base + exp->AddressOfNameOrdinals);
            auto functions = reinterpret_cast<const DWORD*>(base + exp->AddressOfFunctions);
            for (DWORD i = 0; i < exp->NumberOfNames; ++i) {
                if (strcmp(reinterpret_cast<const char*>(base + names[i]), funcName) == 0) {
                    DWORD rva = functions[ordinals[i]];
                    // Apply original RVA to the real in-memory dxgi.dll base.
                    result = reinterpret_cast<BYTE*>(hDXGI) + rva;
                    break;
                }
            }
        }
    }

    FreeLibrary(hDisk);
    return result;
}

static void* GetLiveDXGIExport(const char* funcName) {
    HMODULE hDXGI = GetModuleHandleA("dxgi.dll");
    return hDXGI ? reinterpret_cast<void*>(GetProcAddress(hDXGI, funcName)) : nullptr;
}

static bool IsCodeAddressFromCaptureHookModule(const void* codeAddress) {
    char modulePath[MAX_PATH] = {};
    return ce::overlay_compat::TryGetModulePathFromCodeAddress(codeAddress, modulePath, sizeof(modulePath)) &&
           strstr(modulePath, "capture_hook") != nullptr;
}

template <typename Fn>
static Fn SelectDXGIFactoryExportForCall(const char* functionName, Fn unhookedFn,
                                         const DXGIFactoryRuntimeDecision& decision) {
    void* liveExport = GetLiveDXGIExport(functionName);
    const bool liveFromCaptureHook = IsCodeAddressFromCaptureHookModule(liveExport);
    const bool callerFromFrameGenerationRuntime = decision.callerFromStreamline || decision.callerFromFFX;
    const bool useLiveExport = ce::dx12_overlay_policy::ShouldUseLiveDXGIFactoryExportForFrameGenerationRuntime(
        decision.bypassWrapper, callerFromFrameGenerationRuntime, liveExport != nullptr, liveFromCaptureHook);
    Fn selected = useLiveExport ? reinterpret_cast<Fn>(liveExport) : unhookedFn;

    if (decision.bypassWrapper || useLiveExport) {
        static std::atomic<int> s_factoryExportSourceLogCount{0};
        const int logCount = s_factoryExportSourceLogCount.fetch_add(1, std::memory_order_relaxed);
        if (logCount < 30 || (logCount % 256) == 0) {
            char liveModulePath[MAX_PATH] = {};
            char selectedModulePath[MAX_PATH] = {};
            ce::overlay_compat::TryGetModulePathFromCodeAddress(liveExport, liveModulePath, sizeof(liveModulePath));
            ce::overlay_compat::TryGetModulePathFromCodeAddress(reinterpret_cast<const void*>(selected),
                                                                selectedModulePath, sizeof(selectedModulePath));
            WrapperLog(
                "%s: DXGI factory export source selected=%s selectedFn=%p selectedModule=%s liveFn=%p "
                "liveModule=%s liveCaptureHook=%d callerRuntime=%d",
                functionName ? functionName : "CreateDXGIFactory", useLiveExport ? "live" : "unhooked",
                reinterpret_cast<void*>(selected), selectedModulePath[0] ? selectedModulePath : "unresolved",
                liveExport, liveModulePath[0] ? liveModulePath : "unresolved", liveFromCaptureHook ? 1 : 0,
                callerFromFrameGenerationRuntime ? 1 : 0);
        }
    }

    return selected;
}

HRESULT WINAPI Wrapped_CreateDXGIFactory(REFIID riid, void** ppFactory) {
    // Guard against mutual recursion with third-party overlay hooks (e.g., Steam).
    thread_local bool t_inFactory = false;
    if (t_inFactory) {
        static auto* realFn = reinterpret_cast<PFN_CreateDXGIFactory>(GetUnhookedDXGIExport("CreateDXGIFactory"));
        HookLogImportant("Wrapper: Reentrancy in CreateDXGIFactory – calling real export (realFn=%p)", realFn);
        if (realFn)
            return realFn(riid, ppFactory);
        return E_FAIL;
    }

    // CRITICAL: Use the unhooked DXGI export (original RVA from disk) instead of
    // oCreateDXGIFactory. Steam overlays EAT-hook dxgi.dll exports so that
    // GetProcAddress returns Steam's OverlayHookD3D3 trampoline.  Calling through
    // oCreateDXGIFactory during SL's DllMain goes through Steam's uninitialized
    // overlay code and crashes (0xC0000005 at RIP=0).  GetUnhookedDXGIExport
    // reads the original on-disk RVA and applies it to the loaded dxgi.dll base,
    // giving the real function entry that bypasses Steam's EAT hook entirely.
    const auto decision = EvaluateDXGIFactoryRuntimeDecision(CE_WRAPPER_RETURN_ADDRESS(), "CreateDXGIFactory");
    static auto* unhookedFn = reinterpret_cast<PFN_CreateDXGIFactory>(GetUnhookedDXGIExport("CreateDXGIFactory"));
    auto* createFn = SelectDXGIFactoryExportForCall("CreateDXGIFactory", unhookedFn, decision);
    if (!createFn)
        return E_FAIL;

    t_inFactory = true;
    IDXGIFactory* pRealFactory = nullptr;
    HRESULT hr = createFn(riid, (void**)&pRealFactory);
    t_inFactory = false;

    if (SUCCEEDED(hr) && pRealFactory) {
        // Skip wrapping while a frame-generation runtime is in play. Streamline
        // and FFX create DXGI factories from both runtime and game frames during
        // mode handoff; returning a CE factory wrapper there lets us mutate
        // runtime-owned queues before the SDK has finished its own swapchain
        // wiring.
        if (decision.bypassWrapper) {
            *ppFactory = pRealFactory;
            return hr;
        }

        // Wrap with CWrapDXGIFactory2 (handles all factory versions)
        IDXGIFactory2* pRealFactory2 = nullptr;
        if (SUCCEEDED(pRealFactory->QueryInterface(IID_PPV_ARGS(&pRealFactory2)))) {
            auto* pWrapper = new CWrapDXGIFactory2(pRealFactory2);
            pRealFactory2->Release();
            pRealFactory->Release();

            // Return the wrapper via QueryInterface to handle different riid
            hr = pWrapper->QueryInterface(riid, ppFactory);
            pWrapper->Release();
            WrapperLog("Wrapper: Created wrapped DXGIFactory");
        } else {
            // Fallback: no IDXGIFactory2 interface (unlikely)
            *ppFactory = pRealFactory;
            WrapperLog(
                "Wrapper: DXGI factory does not support IDXGIFactory2, "
                "returning unwrapped");
        }
    }
    return hr;
}

HRESULT WINAPI Wrapped_CreateDXGIFactory1(REFIID riid, void** ppFactory) {
    // Guard against mutual recursion with third-party overlay hooks (e.g., Steam).
    thread_local bool t_inFactory1 = false;
    if (t_inFactory1) {
        static auto* realFn = reinterpret_cast<PFN_CreateDXGIFactory1>(GetUnhookedDXGIExport("CreateDXGIFactory1"));
        HookLogImportant("Wrapper: Reentrancy in CreateDXGIFactory1 – calling real export (realFn=%p)", realFn);
        if (realFn)
            return realFn(riid, ppFactory);
        return E_FAIL;
    }

    const auto decision = EvaluateDXGIFactoryRuntimeDecision(CE_WRAPPER_RETURN_ADDRESS(), "CreateDXGIFactory1");
    static auto* unhookedFn = reinterpret_cast<PFN_CreateDXGIFactory1>(GetUnhookedDXGIExport("CreateDXGIFactory1"));
    auto* createFn = SelectDXGIFactoryExportForCall("CreateDXGIFactory1", unhookedFn, decision);
    if (!createFn)
        return E_FAIL;

    t_inFactory1 = true;
    IDXGIFactory1* pRealFactory = nullptr;
    HRESULT hr = createFn(riid, (void**)&pRealFactory);
    t_inFactory1 = false;

    if (SUCCEEDED(hr) && pRealFactory) {
        if (decision.bypassWrapper) {
            *ppFactory = pRealFactory;
            return hr;
        }

        // Wrap with CWrapDXGIFactory2
        IDXGIFactory2* pRealFactory2 = nullptr;
        if (SUCCEEDED(pRealFactory->QueryInterface(IID_PPV_ARGS(&pRealFactory2)))) {
            auto* pWrapper = new CWrapDXGIFactory2(pRealFactory2);
            pRealFactory2->Release();
            pRealFactory->Release();

            hr = pWrapper->QueryInterface(riid, ppFactory);
            pWrapper->Release();
            WrapperLog("Wrapper: Created wrapped DXGIFactory1");
        } else {
            *ppFactory = pRealFactory;
            WrapperLog(
                "Wrapper: DXGI factory does not support IDXGIFactory2, "
                "returning unwrapped");
        }
    }
    return hr;
}

HRESULT WINAPI Wrapped_CreateDXGIFactory2(UINT Flags, REFIID riid, void** ppFactory) {
    // Guard against mutual recursion with third-party overlay hooks (e.g., Steam).
    // When Steam EAT-hooks dxgi.dll, oCreateDXGIFactory2 points to Steam's
    // trampoline (OverlayHookD3D3).  Calling it during SL's DllMain crashes
    // Steam's uninitialized overlay code.  Always use the unhooked export
    // (original on-disk RVA) to bypass Steam's EAT hook entirely.
    thread_local bool t_inFactory2 = false;
    if (t_inFactory2) {
        static auto* realFn = reinterpret_cast<PFN_CreateDXGIFactory2>(GetUnhookedDXGIExport("CreateDXGIFactory2"));
        HookLogImportant("Wrapper: Reentrancy in CreateDXGIFactory2 – calling real export (realFn=%p)", realFn);
        if (realFn)
            return realFn(Flags, riid, ppFactory);
        return E_FAIL;
    }

    const auto decision = EvaluateDXGIFactoryRuntimeDecision(CE_WRAPPER_RETURN_ADDRESS(), "CreateDXGIFactory2");
    static auto* unhookedFn = reinterpret_cast<PFN_CreateDXGIFactory2>(GetUnhookedDXGIExport("CreateDXGIFactory2"));
    auto* createFn = SelectDXGIFactoryExportForCall("CreateDXGIFactory2", unhookedFn, decision);
    if (!createFn)
        return E_FAIL;

    t_inFactory2 = true;
    IDXGIFactory2* pRealFactory = nullptr;
    HRESULT hr = createFn(Flags, riid, (void**)&pRealFactory);
    t_inFactory2 = false;

    if (SUCCEEDED(hr) && pRealFactory) {
        if (decision.bypassWrapper) {
            *ppFactory = pRealFactory;
            return hr;
        }

        // Wrap with CWrapDXGIFactory2
        auto* pWrapper = new CWrapDXGIFactory2(pRealFactory);
        pRealFactory->Release();

        hr = pWrapper->QueryInterface(riid, ppFactory);
        pWrapper->Release();
        WrapperLog("Wrapper: Created wrapped DXGIFactory2");
    }
    return hr;
}

// ============================================================================
// Wrapped D3D12 Device Creation (uses MSVC-compiled wrapper via C interface)
// ============================================================================

#ifdef ENABLE_D3D12_WRAPPER
// Removed dllexport attribute to match header declaration and avoid warning
HRESULT WINAPI Wrapped_D3D12CreateDevice(IUnknown* pAdapter, D3D_FEATURE_LEVEL MinimumFeatureLevel, REFIID riid,
                                         void** ppDevice) {
    WrapperLog("Wrapper: D3D12CreateDevice called (feature level=0x%X, pAdapter=%p)", MinimumFeatureLevel, pAdapter);

    // If adapter is provided, try to get its LUID for debugging
    if (pAdapter) {
        IDXGIAdapter* pDXGIAdapter = nullptr;
        if (SUCCEEDED(pAdapter->QueryInterface(IID_PPV_ARGS(&pDXGIAdapter)))) {
            DXGI_ADAPTER_DESC desc;
            if (SUCCEEDED(pDXGIAdapter->GetDesc(&desc))) {
                WrapperLog(
                    "Wrapper: D3D12CreateDevice - Adapter LUID: %08X:%08X, "
                    "VRAM: %llu MB",
                    desc.AdapterLuid.HighPart, desc.AdapterLuid.LowPart, desc.DedicatedVideoMemory / (1024 * 1024));
            }
            pDXGIAdapter->Release();
        }
    } else {
        WrapperLog(
            "Wrapper: D3D12CreateDevice - pAdapter is NULL (will use "
            "default adapter)");
    }

    // Mark that D3D12 device creation was actually called
    MarkD3D12DeviceCreated();

    // Initialize DX12 hooks (global vtable hooks + swapchain recreation trigger)
    if (g_dx12HookInstance) {
        g_dx12HookInstance->Init();
        g_dx12HookInstance->EnsurePresentHooks();  // Deferred: only now is D3D12 confirmed
    } else {
        WrapperLog("Wrapper: WARNING - g_dx12HookInstance is null, creating new instance");
        EnsureDX12Hook();
        if (g_dx12HookInstance) {
            g_dx12HookInstance->Init();
            g_dx12HookInstance->EnsurePresentHooks();
        }
    }

    if (!oD3D12CreateDevice) {
        WrapperLog("Wrapper: FATAL - oD3D12CreateDevice is NULL");
        return E_FAIL;
    }

    WrapperLog("Wrapper: Call oD3D12CreateDevice at %p", oD3D12CreateDevice);

    // Arm DRED auto-breadcrumbs + page-fault BEFORE the real device is created so
    // a later DXGI_ERROR_DEVICE_HUNG/REMOVED (e.g. the x86 DX12 focus-loss
    // freeze) yields the exact hung command list and faulting GPU VA instead of a
    // bare HRESULT. Gated by env CE_DX12_DRED (default on).
    ce::dx12_dred::ArmBeforeDeviceCreation();

    // Create the real device first
    ID3D12Device* pRealDevice = nullptr;
    HRESULT hr = oD3D12CreateDevice(pAdapter, MinimumFeatureLevel, IID_PPV_ARGS(&pRealDevice));
    WrapperLog("Wrapper: oD3D12CreateDevice returned hr=0x%08X, pRealDevice=%p", hr, pRealDevice);

    // Hook CreateSampler on the game's actual device
    if (SUCCEEDED(hr) && pRealDevice) {
        DX12_HookDeviceVTable(pRealDevice);
        WrapperLog("Wrapper: Hooked CreateSampler on device %p", pRealDevice);
        hr = pRealDevice->QueryInterface(riid, ppDevice);
        pRealDevice->Release();
    }

    return hr;
}
#endif

// ============================================================================
// Wrapped D3D11 Device Creation
// ============================================================================

HRESULT WINAPI Wrapped_D3D11CreateDevice(IDXGIAdapter* pAdapter, D3D_DRIVER_TYPE DriverType, HMODULE Software,
                                         UINT Flags, const D3D_FEATURE_LEVEL* pFeatureLevels, UINT FeatureLevels,
                                         UINT SDKVersion, ID3D11Device** ppDevice, D3D_FEATURE_LEVEL* pFeatureLevel,
                                         ID3D11DeviceContext** ppImmediateContext) {
    WrapperLog("Wrapped_D3D11CreateDevice: CALLED");

    // Mark that D3D11 device creation was actually called
    g_D3D11Or10DeviceCreated.store(true, std::memory_order_release);

    if (!oD3D11CreateDevice) {
        WrapperLog("Wrapped_D3D11CreateDevice: ERROR - oD3D11CreateDevice is NULL!");
        return E_FAIL;
    }

    if (IsInD3D10CreateScope()) {
        static std::atomic<int> s_D3D10BypassLogCount{0};
        if (s_D3D10BypassLogCount.fetch_add(1, std::memory_order_relaxed) < 8) {
            WrapperLog(
                "Wrapped_D3D11CreateDevice: D3D10 create path detected, "
                "bypassing D3D11 wrappers");
        }
        return oD3D11CreateDevice(DeWrap(pAdapter), DriverType, Software, Flags, pFeatureLevels, FeatureLevels,
                                  SDKVersion, ppDevice, pFeatureLevel, ppImmediateContext);
    }

    ID3D11Device* pRealDevice = nullptr;
    ID3D11DeviceContext* pRealContext = nullptr;
    HRESULT hr = oD3D11CreateDevice(DeWrap(pAdapter), DriverType, Software, Flags, pFeatureLevels, FeatureLevels,
                                    SDKVersion, ppDevice ? &pRealDevice : nullptr, pFeatureLevel,
                                    ppImmediateContext ? &pRealContext : nullptr);

    WrapperLog("Wrapped_D3D11CreateDevice: Original returned hr=0x%08X", hr);

    CWrapD3D11Device* pWrapper = nullptr;
    if (SUCCEEDED(hr) && pRealDevice && ppDevice) {
        DX11Hook_RegisterDeviceIdentity(pRealDevice, "D3D11CreateDevice", true);
        pWrapper = new CWrapD3D11Device(pRealDevice);
        *ppDevice = pWrapper;
        pRealDevice->Release();
        WrapperLog("Wrapped_D3D11CreateDevice: Created wrapped D3D11 device");
    }

    // Install vtable hooks immediately so the game cannot cache un-hooked Draw
    // function pointers from the real context before our detours are active.
    if (SUCCEEDED(hr)) {
        DX11Hook_InstallDeviceAndContextHooks(pRealDevice, pRealContext, NULL);
    }

    if (ppImmediateContext) {
        if (SUCCEEDED(hr) && pRealContext) {
            auto* wrappedContext = new CWrapD3D11DeviceContext(pRealContext, pWrapper);
            *ppImmediateContext = wrappedContext;
            pRealContext->Release();
            WrapperLog("Wrapped_D3D11CreateDevice: Returned wrapped immediate context real=%p wrapper=%p", pRealContext,
                       wrappedContext);
        } else {
            *ppImmediateContext = pRealContext;
        }
    }

    return hr;
}

HRESULT WINAPI Wrapped_D3D11CreateDeviceAndSwapChain(IDXGIAdapter* pAdapter, D3D_DRIVER_TYPE DriverType,
                                                     HMODULE Software, UINT Flags,
                                                     const D3D_FEATURE_LEVEL* pFeatureLevels, UINT FeatureLevels,
                                                     UINT SDKVersion, const DXGI_SWAP_CHAIN_DESC* pSwapChainDesc,
                                                     IDXGISwapChain** ppSwapChain, ID3D11Device** ppDevice,
                                                     D3D_FEATURE_LEVEL* pFeatureLevel,
                                                     ID3D11DeviceContext** ppImmediateContext) {
    WrapperLog("Wrapped_D3D11CreateDeviceAndSwapChain: CALLED");
    WrapperLog("  Adapter=%p, DriverType=%d, Flags=0x%X", pAdapter, DriverType, Flags);
    if (pSwapChainDesc) {
        WrapperLog("  SwapChain: %dx%d, BufferCount=%u", pSwapChainDesc->BufferDesc.Width,
                   pSwapChainDesc->BufferDesc.Height, pSwapChainDesc->BufferCount);
    }

    // Mark that D3D11 device creation was actually called
    g_D3D11Or10DeviceCreated.store(true, std::memory_order_release);

    if (!oD3D11CreateDeviceAndSwapChain) {
        WrapperLog(
            "Wrapped_D3D11CreateDeviceAndSwapChain: ERROR - "
            "oD3D11CreateDeviceAndSwapChain is NULL!");
        return E_FAIL;
    }

    if (IsInD3D10CreateScope()) {
        static std::atomic<int> s_D3D10BypassLogCount{0};
        if (s_D3D10BypassLogCount.fetch_add(1, std::memory_order_relaxed) < 8) {
            WrapperLog(
                "Wrapped_D3D11CreateDeviceAndSwapChain: D3D10 create path "
                "detected, bypassing D3D11 wrappers");
        }
        return oD3D11CreateDeviceAndSwapChain(DeWrap(pAdapter), DriverType, Software, Flags, pFeatureLevels,
                                              FeatureLevels, SDKVersion, pSwapChainDesc, ppSwapChain, ppDevice,
                                              pFeatureLevel, ppImmediateContext);
    }

    WrapperLog("Wrapped_D3D11CreateDeviceAndSwapChain: Calling original at %p", oD3D11CreateDeviceAndSwapChain);

    DXGI_SWAP_CHAIN_DESC modifiedDesc = {};
    const DXGI_SWAP_CHAIN_DESC* pDescToUse = pSwapChainDesc;
    if (pSwapChainDesc) {
        modifiedDesc = *pSwapChainDesc;
        ApplyD3D11CreateDeviceSwapChainBackbufferOverride(modifiedDesc);
        pDescToUse = &modifiedDesc;
    }

    HRESULT hr = oD3D11CreateDeviceAndSwapChain(DeWrap(pAdapter), DriverType, Software, Flags, pFeatureLevels,
                                                FeatureLevels, SDKVersion, pDescToUse, ppSwapChain, ppDevice,
                                                pFeatureLevel, ppImmediateContext);

    WrapperLog("Wrapped_D3D11CreateDeviceAndSwapChain: Original returned hr=0x%08X", hr);

    if (SUCCEEDED(hr)) {
        if (ppDevice && *ppDevice) {
            DX11Hook_RegisterDeviceIdentity(*ppDevice, "D3D11CreateDeviceAndSwapChain", true);
        }
        const auto& gfx = GetActiveGraphicsConfig();
        if (ppSwapChain && *ppSwapChain && HasBackbufferCountOverride(gfx.backbufferCount)) {
            DXGI_SWAP_CHAIN_DESC actualDesc = {};
            if (SUCCEEDED((*ppSwapChain)->GetDesc(&actualDesc))) {
                WrapperLog(
                    "Wrapped_D3D11CreateDeviceAndSwapChain: Actual BufferCount=%u requested=%d "
                    "size=%ux%u swapEffect=%d",
                    actualDesc.BufferCount, gfx.backbufferCount, actualDesc.BufferDesc.Width,
                    actualDesc.BufferDesc.Height, actualDesc.SwapEffect);
            }
        }

        // Install vtable hooks immediately on device and context, before any
        // swapchain-specific setup, so the game cannot cache un-hooked function
        // pointers.
        DX11Hook_InstallDeviceAndContextHooks(ppDevice ? *ppDevice : NULL,
                                              ppImmediateContext ? *ppImmediateContext : NULL,
                                              ppSwapChain ? *ppSwapChain : NULL);

        // D3D11 runtime compatibility: return raw objects and hook swapchain vtable.
        if (ppSwapChain && *ppSwapChain) {
            DX11Hook_OnSwapChainCreated(*ppSwapChain);
        }
        if (ppImmediateContext && *ppImmediateContext) {
            ID3D11DeviceContext* realContext = *ppImmediateContext;
            auto* wrappedContext = new CWrapD3D11DeviceContext(realContext, nullptr);
            *ppImmediateContext = wrappedContext;
            realContext->Release();
            WrapperLog("Wrapped_D3D11CreateDeviceAndSwapChain: Returned wrapped immediate context real=%p wrapper=%p",
                       realContext, wrappedContext);
        }
        static std::atomic<int> s_D3D11CompatLogCount{0};
        if (s_D3D11CompatLogCount.fetch_add(1, std::memory_order_relaxed) < 8) {
            WrapperLog(
                "Wrapped_D3D11CreateDeviceAndSwapChain: compatibility mode - "
                "returning raw device/swapchain with wrapped context");
        }
    }

    return hr;
}

// ============================================================================
// Wrapped D3D10 Device Creation
// ============================================================================

HRESULT WINAPI Wrapped_D3D10CreateDevice(IDXGIAdapter* pAdapter, D3D10_DRIVER_TYPE DriverType, HMODULE Software,
                                         UINT Flags, UINT SDKVersion, ID3D10Device** ppDevice) {
    WrapperLog("Wrapper: D3D10CreateDevice called");

    // Mark that D3D10 device creation was actually called
    g_D3D11Or10DeviceCreated.store(true, std::memory_order_release);

    if (!oD3D10CreateDevice)
        return E_FAIL;

    D3D10CreateScope d3d10CreateScope;

    ID3D10Device* pRealDevice = nullptr;
    HRESULT hr = oD3D10CreateDevice(DeWrap(pAdapter), DriverType, Software, Flags, SDKVersion, &pRealDevice);

    if (SUCCEEDED(hr) && pRealDevice && ppDevice) {
        DX10Hook_RegisterDeviceIdentity(pRealDevice, false, "D3D10CreateDevice");
        auto* pWrapper = new CWrapD3D10Device(pRealDevice);
        *ppDevice = pWrapper;
        pRealDevice->Release();
        WrapperLog("Wrapper: Created wrapped D3D10 device");
    }

    return hr;
}

HRESULT WINAPI Wrapped_D3D10CreateDevice1(IDXGIAdapter* pAdapter, D3D10_DRIVER_TYPE DriverType, HMODULE Software,
                                          UINT Flags, D3D10_FEATURE_LEVEL1 HardwareLevel, UINT SDKVersion,
                                          ID3D10Device1** ppDevice) {
    WrapperLog("Wrapper: D3D10CreateDevice1 called");

    // Mark that D3D10 device creation was actually called
    g_D3D11Or10DeviceCreated.store(true, std::memory_order_release);

    if (!oD3D10CreateDevice1)
        return E_FAIL;

    D3D10CreateScope d3d10CreateScope;

    ID3D10Device1* pRealDevice = nullptr;
    HRESULT hr =
        oD3D10CreateDevice1(DeWrap(pAdapter), DriverType, Software, Flags, HardwareLevel, SDKVersion, &pRealDevice);

    if (SUCCEEDED(hr) && pRealDevice && ppDevice) {
        DX10Hook_RegisterDeviceIdentity(pRealDevice, true, "D3D10CreateDevice1");
        // Cast to base and wrap
        auto* pWrapper = new CWrapD3D10Device(pRealDevice);
        *ppDevice = static_cast<ID3D10Device1*>(pWrapper);
        pRealDevice->Release();
        WrapperLog("Wrapper: Created wrapped D3D10Device1");
    }

    return hr;
}

HRESULT WINAPI Wrapped_D3D10CreateDeviceAndSwapChain(IDXGIAdapter* pAdapter, D3D10_DRIVER_TYPE DriverType,
                                                     HMODULE Software, UINT Flags, UINT SDKVersion,
                                                     DXGI_SWAP_CHAIN_DESC* pSwapChainDesc, IDXGISwapChain** ppSwapChain,
                                                     ID3D10Device** ppDevice) {
    WrapperLog("Wrapper: D3D10CreateDeviceAndSwapChain called");

    // Mark that D3D10 device creation was actually called
    g_D3D11Or10DeviceCreated.store(true, std::memory_order_release);

    if (!oD3D10CreateDeviceAndSwapChain)
        return E_FAIL;

    D3D10CreateScope d3d10CreateScope;

    ID3D10Device* pRealDevice = nullptr;
    IDXGISwapChain* pRealSwapChain = nullptr;

    HRESULT hr =
        oD3D10CreateDeviceAndSwapChain(DeWrap(pAdapter), DriverType, Software, Flags, SDKVersion, pSwapChainDesc,
                                       ppSwapChain ? &pRealSwapChain : nullptr, ppDevice ? &pRealDevice : nullptr);

    if (SUCCEEDED(hr)) {
        if (pRealDevice) {
            DX10Hook_RegisterDeviceIdentity(pRealDevice, false, "D3D10CreateDeviceAndSwapChain");
        }
        if (pRealSwapChain) {
            DX10Hook_RegisterSwapChainIdentity(pRealSwapChain, false, "D3D10CreateDeviceAndSwapChain");
        }
        // D3D10 runtime compatibility: return raw objects.
        // Wrapping D3D10 swapchains/devices has caused invalid vtable pointers in
        // both x64 and x86 test coverage.
        if (pRealDevice && ppDevice) {
            *ppDevice = pRealDevice;
            pRealDevice = nullptr;
        }
        if (pRealSwapChain && ppSwapChain) {
            *ppSwapChain = pRealSwapChain;
            pRealSwapChain = nullptr;
        }
        WrapperLog("Wrapper: D3D10 compatibility mode - returning unwrapped objects");
        return hr;
    }

    return hr;
}

HRESULT WINAPI Wrapped_D3D10CreateDeviceAndSwapChain1(IDXGIAdapter* pAdapter, D3D10_DRIVER_TYPE DriverType,
                                                      HMODULE Software, UINT Flags, D3D10_FEATURE_LEVEL1 HardwareLevel,
                                                      UINT SDKVersion, DXGI_SWAP_CHAIN_DESC* pSwapChainDesc,
                                                      IDXGISwapChain** ppSwapChain, ID3D10Device1** ppDevice) {
    WrapperLog("Wrapper: D3D10CreateDeviceAndSwapChain1 called");
    g_D3D11Or10DeviceCreated.store(true, std::memory_order_release);

    if (!oD3D10CreateDeviceAndSwapChain1)
        return E_FAIL;

    D3D10CreateScope d3d10CreateScope;
    ID3D10Device1* pRealDevice = nullptr;
    IDXGISwapChain* pRealSwapChain = nullptr;
    const HRESULT hr = oD3D10CreateDeviceAndSwapChain1(
        DeWrap(pAdapter), DriverType, Software, Flags, HardwareLevel, SDKVersion, pSwapChainDesc,
        ppSwapChain ? &pRealSwapChain : nullptr, ppDevice ? &pRealDevice : nullptr);

    if (SUCCEEDED(hr)) {
        if (pRealDevice) {
            DX10Hook_RegisterDeviceIdentity(pRealDevice, true, "D3D10CreateDeviceAndSwapChain1");
        }
        if (pRealSwapChain) {
            DX10Hook_RegisterSwapChainIdentity(pRealSwapChain, true, "D3D10CreateDeviceAndSwapChain1");
        }
        if (pRealDevice && ppDevice) {
            *ppDevice = pRealDevice;
            pRealDevice = nullptr;
        }
        if (pRealSwapChain && ppSwapChain) {
            *ppSwapChain = pRealSwapChain;
            pRealSwapChain = nullptr;
        }
        WrapperLog("Wrapper: D3D10.1 compatibility mode - returning unwrapped objects");
    }

    return hr;
}

// ============================================================================
// Wrapped D3D9 Direct3DCreate9
// ============================================================================

IDirect3D9* WINAPI Wrapped_Direct3DCreate9(UINT SDKVersion) {
    WrapperLog("Wrapper: Direct3DCreate9 called (version %u)", SDKVersion);

    // Always return the original IDirect3D9 object from Direct3DCreate9.
    //
    // DO NOT return IDirect3D9Ex here even though IDirect3D9Ex is a COM superset.
    // Some applications and injected overlays access non-COM runtime details;
    // IDirect3D9Ex also changes managed-resource and lost-device semantics.
    //
    // DX9Hook's vtable hook on IDirect3D9::CreateDevice (installed by
    // DetourDirect3DCreate9 when this call reaches dx9_hook.cpp) intercepts
    // device creation while preserving the requested classic device type. Shared
    // capture resources are supplied by an internal helper after creation.
    if (!oDirect3DCreate9)
        return nullptr;
    IDirect3D9* pReal = oDirect3DCreate9(SDKVersion);
    WrapperLog("Wrapper: Returning original IDirect3D9 (safe for internal-layout-aware games)");
    return pReal;
}

HRESULT WINAPI Wrapped_Direct3DCreate9Ex(UINT SDKVersion, IDirect3D9Ex** ppD3D) {
    WrapperLog("Wrapper: Direct3DCreate9Ex called (version %u)", SDKVersion);

    if (!oDirect3DCreate9Ex)
        return E_FAIL;

    IDirect3D9Ex* pReal = nullptr;
    HRESULT hr = oDirect3DCreate9Ex(SDKVersion, &pReal);

    if (SUCCEEDED(hr) && pReal) {
        // Return raw IDirect3D9Ex — same rationale as Wrapped_Direct3DCreate9.
        *ppD3D = pReal;
        WrapperLog("Wrapper: Returning raw IDirect3D9Ex from Direct3DCreate9Ex");
        return S_OK;
    }

    *ppD3D = pReal;
    return hr;
}

HRESULT WINAPI Wrapped_DirectDrawCreateEx(GUID* lpGuid, LPVOID* lplpDD, REFIID iid, IUnknown* pUnkOuter) {
    WrapperLog("Wrapper: DirectDrawCreateEx called (out=%p)", lplpDD);

    if (!oDirectDrawCreateEx)
        return E_FAIL;

    HRESULT hr = oDirectDrawCreateEx(lpGuid, lplpDD, iid, pUnkOuter);
    WrapperLog("Wrapper: DirectDrawCreateEx returned hr=0x%08X, object=%p", hr,
               (lplpDD && SUCCEEDED(hr)) ? *lplpDD : nullptr);
    if (SUCCEEDED(hr) && lplpDD && *lplpDD) {
        bool hooked = HookDirectDrawObject(*lplpDD, iid);
        WrapperLog("Wrapper: HookDirectDrawObject returned %d for object=%p", hooked ? 1 : 0, *lplpDD);
    }

    return hr;
}

HRESULT WINAPI Wrapped_DirectDrawCreate(GUID* lpGuid, LPVOID* lplpDD, IUnknown* pUnkOuter) {
    WrapperLog("Wrapper: DirectDrawCreate called (out=%p)", lplpDD);
    if (!oDirectDrawCreate)
        return E_FAIL;

    const HRESULT hr = oDirectDrawCreate(lpGuid, lplpDD, pUnkOuter);
    if (SUCCEEDED(hr) && lplpDD && *lplpDD)
        HookDirectDrawObject(*lplpDD, IID_IDirectDraw);
    return hr;
}

// ============================================================================
// Wrapper System Initialization
// ============================================================================

static bool s_DXGIInitialized = false;
static bool s_D3D10Initialized = false;
static bool s_D3D11Initialized = false;
static bool s_D3D12Initialized = false;
static bool s_D3D9Initialized = false;
static bool s_DDrawInitialized = false;

bool InitializeWrapperHooks() {
    // Do NOT return early when g_WrappersActive is true from a previous
    // partial initialization (e.g. DllMain ran before D3D11.dll was loaded).
    // The per-category !s_*Initialized guards below let us retry categories
    // whose DLLs weren't available on the first call.  g_WrappersActive only
    // prevents double-running the category-independent setup below.

    if (!g_WrappersActive) {
        EarlyLog("Wrapper: Initializing wrapper hooks (IAT mode)...");
    }

    bool anySuccess = false;

    // CRITICAL FIX: Each hook category can be retried independently if the DLL
    // wasn't loaded yet We must NOT set g_WrappersActive = true until ALL
    // categories that will ever load are done

    if (!s_DXGIInitialized) {
        EarlyLog("Wrapper: Initializing DXGI hooks...");
        s_DXGIInitialized = IATHook::InitializeDXGIHooks();
        if (s_DXGIInitialized)
            anySuccess = true;
    }

    if (!s_D3D10Initialized) {
        EarlyLog("Wrapper: Initializing D3D10 hooks...");
        s_D3D10Initialized = IATHook::InitializeD3D10Hooks();
        if (s_D3D10Initialized)
            anySuccess = true;
    }

    if (!s_D3D11Initialized) {
        EarlyLog("Wrapper: Initializing D3D11 hooks...");
        s_D3D11Initialized = IATHook::InitializeD3D11Hooks();
        if (s_D3D11Initialized) {
            anySuccess = true;
            HookLogImportant("Wrapper: D3D11 IAT hooks installed (retry)");
        }
    }

    if (!s_D3D12Initialized) {
        EarlyLog("Wrapper: Initializing D3D12 hooks...");
        s_D3D12Initialized = IATHook::InitializeD3D12Hooks();
        if (s_D3D12Initialized)
            anySuccess = true;
    }

    if (!s_D3D9Initialized) {
        EarlyLog("Wrapper: Initializing D3D9 hooks...");
        s_D3D9Initialized = IATHook::InitializeD3D9Hooks();
        if (s_D3D9Initialized)
            anySuccess = true;
    }

    if (!s_DDrawInitialized) {
        EarlyLog("Wrapper: Initializing DirectDraw hooks...");
        s_DDrawInitialized = IATHook::InitializeDDrawHooks();
        if (s_DDrawInitialized)
            anySuccess = true;
    }

    // Mark as active if ANY hooks were installed (allows partial initialization)
    // This enables retry for categories whose DLLs weren't loaded yet
    if (anySuccess) {
        g_WrappersActive = true;
    }

    EarlyLog(
        "Wrapper: IAT initialization complete (DXGI=%d, D3D10=%d, D3D11=%d, "
        "D3D12=%d, D3D9=%d, DDraw=%d)",
        s_DXGIInitialized, s_D3D10Initialized, s_D3D11Initialized, s_D3D12Initialized, s_D3D9Initialized,
        s_DDrawInitialized);
    return anySuccess;
}

void ShutdownWrapperHooks() {
    if (!g_WrappersActive)
        return;

    WrapperLog("Wrapper: Shutting down wrapper hooks...");

    IATHook::ShutdownIATHooks();

    g_WrappersActive = false;
}

bool AreWrappersActive() {
    return g_WrappersActive;
}

// ============================================================================
// Helper Functions
// ============================================================================

IDXGISwapChain* UnwrapSwapchain(IDXGISwapChain* pSwapChain) {
    if (!pSwapChain)
        return nullptr;

    CWrapDXGISwapChain* pWrapper = nullptr;
    if (SUCCEEDED(pSwapChain->QueryInterface(IID_CWrapDXGISwapChain, (void**)&pWrapper))) {
        IDXGISwapChain* pReal = pWrapper->GetReal();
        pWrapper->Release();
        return pReal;
    }

    return pSwapChain;
}

#ifdef ENABLE_D3D12_WRAPPER
ID3D12Device* UnwrapDevice(ID3D12Device* pDevice) {
    // Use the C interface to unwrap (calls into MSVC-compiled code)
    return D3D12Wrapper_UnwrapDevice(pDevice);
}
#endif

bool IsSwapchainWrapped(IDXGISwapChain* pSwapChain) {
    if (!pSwapChain)
        return false;

    CWrapDXGISwapChain* pWrapper = nullptr;
    if (SUCCEEDED(pSwapChain->QueryInterface(IID_CWrapDXGISwapChain, (void**)&pWrapper))) {
        pWrapper->Release();
        return true;
    }

    return false;
}
