/**
 * Wrapper Hook Entry Points Implementation
 *
 * Caller attribution, the shared wrapper state, and the DXGI factory exports.
 * The device-creation exports and hook installation live in
 * wrapper_hooks_devices.cpp.
 */

#include <atomic>
#include <cstdint>
#include <cstdarg>
#include <cstdio>

#include "../../common/raii_helpers.h"

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
#include "../common/dx12_process_frame_diagnostics.h"
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
#include "vulkan_dxgi_fifo_present.h"
#include "wrapper_hooks.h"
// Forward declaration from dx11_hook.cpp (after D3D11 types are available)
extern void DX11Hook_InstallDeviceAndContextHooks(ID3D11Device* pDevice, ID3D11DeviceContext* pContext,
                                                  IDXGISwapChain* pSwapChain);
#include "wrapper_hooks_internal.h"

bool ApplyD3D11CreateDeviceSwapChainBackbufferOverride(DXGI_SWAP_CHAIN_DESC& desc) {
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

bool g_WrappersActive = false;

// API Detection Flags - set when actual device creation is called
// These allow distinguishing between DLL being loaded (e.g. d3d12.dll via
// D3D11On12) vs the app actually using that API.
std::atomic<bool> g_D3D11Or10DeviceCreated{false};
static std::atomic<bool> g_D3D12DeviceCreated{false};

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
        if (realFn) {
            const HRESULT hr = realFn(riid, ppFactory);
            if (SUCCEEDED(hr) && ppFactory && *ppFactory)
                ce::vulkan_dxgi_fifo::MaybeInstallFactoryHooks(
                    static_cast<IUnknown*>(*ppFactory), "CreateDXGIFactory/reentrant");
            return hr;
        }
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
        const bool vulkanFinalFifo =
            ce::vulkan_dxgi_fifo::MaybeInstallFactoryHooks(
                pRealFactory, "CreateDXGIFactory");
        // Skip wrapping while a frame-generation runtime is in play. Streamline
        // and FFX create DXGI factories from both runtime and game frames during
        // mode handoff; returning a CE factory wrapper there lets us mutate
        // runtime-owned queues before the SDK has finished its own swapchain
        // wiring.
        if (decision.bypassWrapper || vulkanFinalFifo) {
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
        if (realFn) {
            const HRESULT hr = realFn(riid, ppFactory);
            if (SUCCEEDED(hr) && ppFactory && *ppFactory)
                ce::vulkan_dxgi_fifo::MaybeInstallFactoryHooks(
                    static_cast<IUnknown*>(*ppFactory), "CreateDXGIFactory1/reentrant");
            return hr;
        }
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
        const bool vulkanFinalFifo =
            ce::vulkan_dxgi_fifo::MaybeInstallFactoryHooks(
                pRealFactory, "CreateDXGIFactory1");
        if (decision.bypassWrapper || vulkanFinalFifo) {
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
        if (realFn) {
            const HRESULT hr = realFn(Flags, riid, ppFactory);
            if (SUCCEEDED(hr) && ppFactory && *ppFactory)
                ce::vulkan_dxgi_fifo::MaybeInstallFactoryHooks(
                    static_cast<IUnknown*>(*ppFactory), "CreateDXGIFactory2/reentrant");
            return hr;
        }
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
        const bool vulkanFinalFifo =
            ce::vulkan_dxgi_fifo::MaybeInstallFactoryHooks(
                pRealFactory, "CreateDXGIFactory2");
        if (decision.bypassWrapper || vulkanFinalFifo) {
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
