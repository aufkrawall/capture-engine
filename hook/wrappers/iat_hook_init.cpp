/**
 * Per-API IAT hook installation
 *
 * Installs the DXGI/D3D12/D3D11/D3D10/D3D9/DirectDraw/kernel32/advapi32 import
 * hooks over the primitives in iat_hook.cpp, and owns the GetProcAddress detour
 * that catches dynamically resolved exports.
 *
 * Split out of iat_hook.cpp.
 */

#include "iat_hook.h"
#include <d3d12.h>
#include <psapi.h>
#include <tlhelp32.h>
#include <atomic>
#include <mutex>
#include <unordered_map>
#include <vector>
#include "../apis/dx11_hook.h"
#include "../apis/dx12_sampler_hooks.h"
#include "../apis/lod_helper.h"
#include "../common/module_enumeration.h"
#include "../common/module_pin.h"
#include "../common/ngx_fg_preset_override.h"
#include "../common/overlay_compat.h"
#include "../common/sampler_override_utils.h"
#include "hook_common.h"
#include "wrapper_hooks.h"
#include "iat_hook_internal.h"

// ============================================================================
// DXGI/D3D Hook Initialization
// ============================================================================

// Forward declarations for wrapped functions (from wrapper_hooks.cpp)
// These are in global namespace, not IATHook namespace
extern HRESULT WINAPI Wrapped_CreateDXGIFactory(REFIID riid, void** ppFactory);
extern HRESULT WINAPI Wrapped_CreateDXGIFactory1(REFIID riid, void** ppFactory);
extern HRESULT WINAPI Wrapped_CreateDXGIFactory2(UINT Flags, REFIID riid, void** ppFactory);

#ifdef ENABLE_D3D12_WRAPPER
extern HRESULT WINAPI Wrapped_D3D12CreateDevice(IUnknown* pAdapter, D3D_FEATURE_LEVEL MinimumFeatureLevel, REFIID riid,
                                                void** ppDevice);
#endif

// Original function pointers (defined in wrapper_hooks.cpp)
extern PFN_CreateDXGIFactory oCreateDXGIFactory;
extern PFN_CreateDXGIFactory1 oCreateDXGIFactory1;
extern PFN_CreateDXGIFactory2 oCreateDXGIFactory2;

namespace IATHook {

static bool ShouldLogRepeatedIATScan(std::atomic<int>& counter) {
    const int count = counter.fetch_add(1, std::memory_order_relaxed) + 1;
    return count <= 3 || count == 10 || (count % 300) == 0;
}

bool InitializeDXGIHooks() {
    WrapperLog("IAT: Initializing DXGI hooks...");
    bool success = true;

    // Get dxgi.dll - if not loaded, we'll hook when it loads.
    // Pinned: the export addresses cached below outlive this call and are
    // handed to game code through the GetProcAddress detour, so the image
    // must not be able to unload under them (see common/module_pin.h).
    HMODULE hDXGI = ce::module_pin::PinByName("dxgi.dll");

    if (hDXGI) {
        // Get original functions from dxgi.dll
        oCreateDXGIFactory = reinterpret_cast<PFN_CreateDXGIFactory>(GetProcAddress(hDXGI, "CreateDXGIFactory"));
        oCreateDXGIFactory1 = reinterpret_cast<PFN_CreateDXGIFactory1>(GetProcAddress(hDXGI, "CreateDXGIFactory1"));
        oCreateDXGIFactory2 = reinterpret_cast<PFN_CreateDXGIFactory2>(GetProcAddress(hDXGI, "CreateDXGIFactory2"));

        // Patch IAT in all modules
        void* dummy;
        if (!PatchIATAllModules("dxgi.dll", "CreateDXGIFactory", (void*)Wrapped_CreateDXGIFactory, &dummy)) {
            WrapperLog("IAT: CreateDXGIFactory not found in IAT (may not be imported)");
        }

        if (!PatchIATAllModules("dxgi.dll", "CreateDXGIFactory1", (void*)Wrapped_CreateDXGIFactory1, &dummy)) {
            WrapperLog("IAT: CreateDXGIFactory1 not found in IAT");
        }

        if (!PatchIATAllModules("dxgi.dll", "CreateDXGIFactory2", (void*)Wrapped_CreateDXGIFactory2, &dummy)) {
            WrapperLog("IAT: CreateDXGIFactory2 not found in IAT");
        }

        // CRITICAL FIX: Register dynamic hooks for GetProcAddress interception
        // Games like Strange Brigade may load dxgi.dll dynamically at runtime
        // and use GetProcAddress to get factory creation functions. Without
        // dynamic hooks, we won't intercept these calls.
        RegisterDynamicHook("CreateDXGIFactory", (void*)Wrapped_CreateDXGIFactory, (void**)&oCreateDXGIFactory);
        RegisterDynamicHook("CreateDXGIFactory1", (void*)Wrapped_CreateDXGIFactory1, (void**)&oCreateDXGIFactory1);
        RegisterDynamicHook("CreateDXGIFactory2", (void*)Wrapped_CreateDXGIFactory2, (void**)&oCreateDXGIFactory2);
        WrapperLog("IAT: Registered DXGI factory functions for dynamic hooking");

        WrapperLog("IAT: DXGI hooks initialized");
    } else {
        WrapperLog("IAT: dxgi.dll not loaded");
        success = false;
    }

    return success;
}

bool InitializeD3D12Hooks() {
    WrapperLog("IAT: Initializing D3D12 hooks...");

    HMODULE hD3D12 = ce::module_pin::PinByName("d3d12.dll");
    if (!hD3D12) {
        WrapperLog("IAT: d3d12.dll not loaded");
        return false;
    }

    // Hook D3D12SerializeRootSignature and D3D12SerializeVersionedRootSignature
    // These handle static samplers for AF/mip bias overrides
    // CRITICAL: These hooks are needed even without ENABLE_D3D12_WRAPPER
    oSerializeRootSignature =
        reinterpret_cast<D3D12SerializeRootSignaturePtr>(GetProcAddress(hD3D12, "D3D12SerializeRootSignature"));

    oSerializeVersionedRootSignature = reinterpret_cast<D3D12SerializeVersionedRootSignaturePtr>(
        GetProcAddress(hD3D12, "D3D12SerializeVersionedRootSignature"));

    oD3D12CreateDeviceRaw = reinterpret_cast<D3D12CreateDeviceRawPtr>(GetProcAddress(hD3D12, "D3D12CreateDevice"));
    oD3D12GetInterface = reinterpret_cast<D3D12GetInterfacePtr>(GetProcAddress(hD3D12, "D3D12GetInterface"));

    void* dummy;
    if (oSerializeRootSignature) {
        PatchIATAllModules("d3d12.dll", "D3D12SerializeRootSignature", (void*)DetourSerializeRootSignature, &dummy);
        RegisterDynamicHook("D3D12SerializeRootSignature", (void*)DetourSerializeRootSignature,
                            (void**)&oSerializeRootSignature);
        WrapperLog("IAT: Hooked D3D12SerializeRootSignature");
    }

    if (oSerializeVersionedRootSignature) {
        PatchIATAllModules("d3d12.dll", "D3D12SerializeVersionedRootSignature",
                           (void*)DetourSerializeVersionedRootSignature, &dummy);
        RegisterDynamicHook("D3D12SerializeVersionedRootSignature", (void*)DetourSerializeVersionedRootSignature,
                            (void**)&oSerializeVersionedRootSignature);
        WrapperLog("IAT: Hooked D3D12SerializeVersionedRootSignature");
    }

    if (oD3D12GetInterface) {
        PatchIATAllModules("d3d12.dll", "D3D12GetInterface", (void*)DetourD3D12GetInterface, &dummy);
        RegisterDynamicHook("D3D12GetInterface", (void*)DetourD3D12GetInterface, (void**)&oD3D12GetInterface);
        WrapperLog("IAT: Hooked D3D12GetInterface / ID3D12DeviceFactory");
    }

#ifdef ENABLE_D3D12_WRAPPER
    // D3D12CreateDevice wrapper requires d3d12_wrappers.dll which may not exist
    WrapperLog("IAT: Initializing D3D12CreateDevice wrapper...");

    // CRITICAL: Pre-load d3d12_wrappers.dll to avoid delay-load race condition
    // When the game calls D3D12CreateDevice, Wrapped_D3D12CreateDevice will
    // call D3D12Wrapper_WrapDevice which is in d3d12_wrappers.dll. If we don't
    // preload it here, the delay-load mechanism could crash due to thread
    // safety issues.
    static bool s_WrappersPreloaded = false;
    if (!s_WrappersPreloaded) {
        s_WrappersPreloaded = true;
        HMODULE hWrappers = LoadLibraryExW(L"d3d12_wrappers.dll", nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR);
        if (!hWrappers) {
            hWrappers = LoadLibraryExW(L"d3d12_wrappers_x86.dll", nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR);
        }
        if (hWrappers) {
            WrapperLog("IAT: Pre-loaded d3d12_wrappers.dll at %p", hWrappers);

            // Register sampler override callback for AF/mip bias support
            typedef void(WINAPI * PFN_SetSamplerOverrideCallback)(void* callback);
            auto* setCallback =
                (PFN_SetSamplerOverrideCallback)GetProcAddress(hWrappers, "D3D12Wrapper_SetSamplerOverrideCallback");
            if (setCallback) {
                setCallback((void*)ApplyDX12SamplerOverridesCallback);
                WrapperLog("IAT: Registered D3D12 sampler override callback");
            }
        } else {
            WrapperLog(
                "IAT: WARNING - Could not pre-load d3d12_wrappers.dll, "
                "delay-load will be used. Err=%d",
                GetLastError());
        }
    }

    oD3D12CreateDevice = reinterpret_cast<PFN_D3D12CreateDevice>(GetProcAddress(hD3D12, "D3D12CreateDevice"));

    bool patchResult = PatchIATAllModules("d3d12.dll", "D3D12CreateDevice", (void*)Wrapped_D3D12CreateDevice, &dummy);
    RegisterDynamicHook("D3D12CreateDevice", (void*)Wrapped_D3D12CreateDevice, (void**)&oD3D12CreateDevice);
    if (!patchResult) {
        WrapperLog("IAT: D3D12CreateDevice not found in IAT");
    }

    WrapperLog("IAT: D3D12 hooks initialized (patchResult=%d)", patchResult);
#else
    bool patchResult = false;
    if (oD3D12CreateDeviceRaw) {
        patchResult = PatchIATAllModules("d3d12.dll", "D3D12CreateDevice", (void*)DetourD3D12CreateDeviceRaw, &dummy);
        RegisterDynamicHook("D3D12CreateDevice", (void*)DetourD3D12CreateDeviceRaw, (void**)&oD3D12CreateDeviceRaw);
    }
    WrapperLog("IAT: D3D12 raw device/sampler/root-signature hooks initialized (patchResult=%d)", patchResult);
#endif

    return true;
}

bool InitializeD3D11Hooks() {
    static std::atomic<int> s_d3d11ScanLogCount{0};
    const bool logScan = ShouldLogRepeatedIATScan(s_d3d11ScanLogCount);
    if (logScan) {
        WrapperLog("IAT: Initializing D3D11 hooks...");
    }

    HMODULE hD3D11 = ce::module_pin::PinByName("d3d11.dll");

    if (hD3D11) {
        // Get original D3D11CreateDeviceAndSwapChain
        // oD3D11CreateDeviceAndSwapChain and DX11_DetourCreateDeviceAndSwapChain
        // are declared in dx11_hook.h

        ::oD3D11CreateDeviceAndSwapChain = reinterpret_cast<PFN_D3D11CreateDeviceAndSwapChain>(
            GetProcAddress(hD3D11, "D3D11CreateDeviceAndSwapChain"));

        ::oD3D11CreateDevice = reinterpret_cast<PFN_D3D11CreateDevice>(GetProcAddress(hD3D11, "D3D11CreateDevice"));

        void* dummy;

        // Patch D3D11CreateDeviceAndSwapChain
        if (PatchIATAllModules("d3d11.dll", "D3D11CreateDeviceAndSwapChain",
                               (void*)::Wrapped_D3D11CreateDeviceAndSwapChain,
                               &dummy)) {  // Use Wrapped_, not DX11_Detour
            WrapperLog("IAT: Patched D3D11CreateDeviceAndSwapChain");
        } else {
            WrapperLog("IAT: D3D11CreateDeviceAndSwapChain not found in IAT");
        }

        // Patch D3D11CreateDevice
        if (PatchIATAllModules("d3d11.dll", "D3D11CreateDevice", (void*)::Wrapped_D3D11CreateDevice, &dummy)) {
            WrapperLog("IAT: Patched D3D11CreateDevice");
        } else {
            WrapperLog("IAT: D3D11CreateDevice not found in IAT");
        }

        // CRITICAL FIX: Register dynamic hooks for GetProcAddress interception
        // Games may load d3d11.dll dynamically at runtime and use GetProcAddress
        // to get device creation functions. Without dynamic hooks, we won't
        // intercept these calls.
        RegisterDynamicHook("D3D11CreateDeviceAndSwapChain", (void*)::Wrapped_D3D11CreateDeviceAndSwapChain,
                            (void**)&::oD3D11CreateDeviceAndSwapChain);
        RegisterDynamicHook("D3D11CreateDevice", (void*)::Wrapped_D3D11CreateDevice, (void**)&::oD3D11CreateDevice);
        WrapperLog("IAT: Registered D3D11 functions for dynamic hooking");

        WrapperLog("IAT: D3D11 hooks initialized");
        return true;
    }

    return false;
}

bool InitializeD3D10Hooks() {
    static std::atomic<int> s_d3d10ScanLogCount{0};
    const bool logScan = ShouldLogRepeatedIATScan(s_d3d10ScanLogCount);
    if (logScan) {
        WrapperLog("IAT: Initializing D3D10 hooks...");
    }

    HMODULE hD3D10 = ce::module_pin::PinByName("d3d10.dll");

    if (hD3D10) {
        ::oD3D10CreateDevice = (PFN_D3D10CreateDevice)GetProcAddress(hD3D10, "D3D10CreateDevice");
        ::oD3D10CreateDeviceAndSwapChain =
            (PFN_D3D10CreateDeviceAndSwapChain)GetProcAddress(hD3D10, "D3D10CreateDeviceAndSwapChain");

        void* dummy;
        PatchIATAllModules("d3d10.dll", "D3D10CreateDevice", (void*)::Wrapped_D3D10CreateDevice, &dummy);
        PatchIATAllModules("d3d10.dll", "D3D10CreateDeviceAndSwapChain", (void*)::Wrapped_D3D10CreateDeviceAndSwapChain,
                           &dummy);

        // CRITICAL FIX: Register dynamic hooks for GetProcAddress interception
        RegisterDynamicHook("D3D10CreateDevice", (void*)::Wrapped_D3D10CreateDevice, (void**)&::oD3D10CreateDevice);
        RegisterDynamicHook("D3D10CreateDeviceAndSwapChain", (void*)::Wrapped_D3D10CreateDeviceAndSwapChain,
                            (void**)&::oD3D10CreateDeviceAndSwapChain);

        // D3D10.1
        HMODULE hD3D10_1 = ce::module_pin::PinByName("d3d10_1.dll");
        if (hD3D10_1) {
            ::oD3D10CreateDevice1 = (PFN_D3D10CreateDevice1)GetProcAddress(hD3D10_1, "D3D10CreateDevice1");
            ::oD3D10CreateDeviceAndSwapChain1 =
                (PFN_D3D10CreateDeviceAndSwapChain1)GetProcAddress(hD3D10_1, "D3D10CreateDeviceAndSwapChain1");
            PatchIATAllModules("d3d10_1.dll", "D3D10CreateDevice1", (void*)::Wrapped_D3D10CreateDevice1, &dummy);
            PatchIATAllModules("d3d10_1.dll", "D3D10CreateDeviceAndSwapChain1",
                               (void*)::Wrapped_D3D10CreateDeviceAndSwapChain1, &dummy);
            RegisterDynamicHook("D3D10CreateDevice1", (void*)::Wrapped_D3D10CreateDevice1,
                                (void**)&::oD3D10CreateDevice1);
            RegisterDynamicHook("D3D10CreateDeviceAndSwapChain1", (void*)::Wrapped_D3D10CreateDeviceAndSwapChain1,
                                (void**)&::oD3D10CreateDeviceAndSwapChain1);
        }

        WrapperLog("IAT: D3D10 hooks initialized");
        return true;
    }
    if (logScan) {
        WrapperLog("IAT: d3d10.dll not loaded");
    }
    return false;
}

bool InitializeD3D9Hooks() {
    static std::atomic<int> s_d3d9ScanLogCount{0};
    const bool logScan = ShouldLogRepeatedIATScan(s_d3d9ScanLogCount);
    if (logScan) {
        WrapperLog("IAT: Initializing D3D9 hooks...");
    }

    HMODULE hD3D9 = ce::module_pin::PinByName("d3d9.dll");

    if (hD3D9) {
        // Get original functions
        oDirect3DCreate9 = reinterpret_cast<PFN_Direct3DCreate9>(GetProcAddress(hD3D9, "Direct3DCreate9"));
        oDirect3DCreate9Ex = reinterpret_cast<PFN_Direct3DCreate9Ex>(GetProcAddress(hD3D9, "Direct3DCreate9Ex"));

        void* dummy;
        // Patch Direct3DCreate9
        if (PatchIATAllModules("d3d9.dll", "Direct3DCreate9", (void*)Wrapped_Direct3DCreate9, &dummy)) {
            WrapperLog("IAT: Patched Direct3DCreate9");
        } else {
            // Also try explicit GetProcAddress target for late binding
            if (oDirect3DCreate9) {
                // If IAT search failed, it might be due to ordinal-only or forwarded
                // export. But generally "Direct3DCreate9" is by name.
                WrapperLog("IAT: Direct3DCreate9 not found in IAT");
            }
        }

        // Patch Direct3DCreate9Ex
        if (PatchIATAllModules("d3d9.dll", "Direct3DCreate9Ex", (void*)Wrapped_Direct3DCreate9Ex, &dummy)) {
            WrapperLog("IAT: Patched Direct3DCreate9Ex");
        } else {
            WrapperLog("IAT: Direct3DCreate9Ex not found in IAT");
        }

        WrapperLog("IAT: D3D9 hooks initialized");
        return true;
    }

    if (logScan) {
        WrapperLog("IAT: d3d9.dll not loaded");
    }
    return false;
}

bool InitializeDDrawHooks() {
    static std::atomic<int> s_ddrawScanLogCount{0};
    const bool logScan = ShouldLogRepeatedIATScan(s_ddrawScanLogCount);
    if (logScan) {
        WrapperLog("IAT: Initializing DirectDraw hooks...");
    }

    HMODULE hDDraw = ce::module_pin::PinByName("ddraw.dll");
    if (!hDDraw) {
        if (logScan) {
            WrapperLog("IAT: ddraw.dll not loaded");
        }
        return false;
    }

    oDirectDrawCreate = reinterpret_cast<PFN_DirectDrawCreate>(GetProcAddress(hDDraw, "DirectDrawCreate"));
    oDirectDrawCreateEx = reinterpret_cast<PFN_DirectDrawCreateEx>(GetProcAddress(hDDraw, "DirectDrawCreateEx"));

    void* dummy = nullptr;
    if (PatchIATAllModules("ddraw.dll", "DirectDrawCreate", (void*)Wrapped_DirectDrawCreate, &dummy)) {
        WrapperLog("IAT: Patched DirectDrawCreate");
    } else {
        WrapperLog("IAT: DirectDrawCreate not found in IAT");
    }
    if (PatchIATAllModules("ddraw.dll", "DirectDrawCreateEx", (void*)Wrapped_DirectDrawCreateEx, &dummy)) {
        WrapperLog("IAT: Patched DirectDrawCreateEx");
    } else {
        WrapperLog("IAT: DirectDrawCreateEx not found in IAT");
    }

    RegisterDynamicHook("DirectDrawCreate", (void*)Wrapped_DirectDrawCreate, (void**)&oDirectDrawCreate);
    RegisterDynamicHook("DirectDrawCreateEx", (void*)Wrapped_DirectDrawCreateEx, (void**)&oDirectDrawCreateEx);
    WrapperLog("IAT: DirectDraw hooks initialized");
    return true;
}

// Note: InitializeVulkanHooks removed - Vulkan is now handled by
// VK_LAYER_CE_overlay The Vulkan layer approach provides better compatibility
// and doesn't require IAT patching

bool InitializeKernel32Hooks(void* LoadLibraryAHook, void** pOriginalLoadLibraryA, void* LoadLibraryWHook,
                             void** pOriginalLoadLibraryW, void* LoadLibraryExAHook, void** pOriginalLoadLibraryExA,
                             void* LoadLibraryExWHook, void** pOriginalLoadLibraryExW, void* CreateProcessAHook,
                             void** pOriginalCreateProcessA, void* CreateProcessWHook, void** pOriginalCreateProcessW) {
    WrapperLog("IAT: Initializing kernel32 hooks...");

    HMODULE hKernel32 = GetModuleHandleA("kernel32.dll");
    if (!hKernel32) {
        WrapperLog("IAT: kernel32.dll not loaded (unexpected!)");
        return false;
    }

    bool success = true;

    // Get original function addresses
    if (pOriginalLoadLibraryA) {
        *pOriginalLoadLibraryA = (void*)GetProcAddress(hKernel32, "LoadLibraryA");
    }
    if (pOriginalLoadLibraryW) {
        *pOriginalLoadLibraryW = (void*)GetProcAddress(hKernel32, "LoadLibraryW");
    }
    if (pOriginalLoadLibraryExA) {
        *pOriginalLoadLibraryExA = (void*)GetProcAddress(hKernel32, "LoadLibraryExA");
    }
    if (pOriginalLoadLibraryExW) {
        *pOriginalLoadLibraryExW = (void*)GetProcAddress(hKernel32, "LoadLibraryExW");
    }
    if (pOriginalCreateProcessA) {
        *pOriginalCreateProcessA = (void*)GetProcAddress(hKernel32, "CreateProcessA");
    }
    if (pOriginalCreateProcessW) {
        *pOriginalCreateProcessW = (void*)GetProcAddress(hKernel32, "CreateProcessW");
    }

    // Patch LoadLibrary* in all modules
    void* dummy;
    if (LoadLibraryAHook) {
        PatchIATAllModules("kernel32.dll", "LoadLibraryA", LoadLibraryAHook, &dummy);
    }
    if (LoadLibraryWHook) {
        PatchIATAllModules("kernel32.dll", "LoadLibraryW", LoadLibraryWHook, &dummy);
    }
    if (LoadLibraryExAHook) {
        PatchIATAllModules("kernel32.dll", "LoadLibraryExA", LoadLibraryExAHook, &dummy);
    }
    if (LoadLibraryExWHook) {
        PatchIATAllModules("kernel32.dll", "LoadLibraryExW", LoadLibraryExWHook, &dummy);
    }

    // Patch CreateProcess* in all modules
    if (CreateProcessAHook) {
        PatchIATAllModules("kernel32.dll", "CreateProcessA", CreateProcessAHook, &dummy);
    }
    if (CreateProcessWHook) {
        PatchIATAllModules("kernel32.dll", "CreateProcessW", CreateProcessWHook, &dummy);
    }

    WrapperLog("IAT: kernel32 hooks initialized");
    return success;
}

void ShutdownIATHooks() {
    std::lock_guard<std::mutex> lock(g_PatchLock);

    // Restore all patched entries
    for (auto& entry : g_PatchedEntries) {
        MEMORY_BASIC_INFORMATION memory = {};
        if (VirtualQuery(reinterpret_cast<const void*>(entry.iatEntry), &memory, sizeof(memory)) != sizeof(memory) ||
            memory.State != MEM_COMMIT ||
            memory.Type != MEM_IMAGE || memory.AllocationBase != entry.targetModule ||
            (memory.Protect & (PAGE_NOACCESS | PAGE_GUARD)) || *entry.iatEntry != entry.hookFunction) {
            WrapperLog("IAT: Shutdown preserved foreign or unavailable entry for %s!%s in module %p",
                       entry.sourceModule.c_str(), entry.functionName.c_str(), entry.targetModule);
            continue;
        }
        DWORD oldProtect;
        if (VirtualProtect(reinterpret_cast<void*>(entry.iatEntry), sizeof(void*), PAGE_READWRITE, &oldProtect)) {
            void* replaced = InterlockedCompareExchangePointer(reinterpret_cast<PVOID volatile*>(entry.iatEntry),
                                                               entry.originalFunction, entry.hookFunction);
            VirtualProtect(reinterpret_cast<void*>(entry.iatEntry), sizeof(void*), oldProtect, &oldProtect);
            if (replaced != entry.hookFunction) {
                WrapperLog("IAT: Shutdown preserved concurrent replacement %p for %s!%s in module %p", replaced,
                           entry.sourceModule.c_str(), entry.functionName.c_str(), entry.targetModule);
            }
        }
    }

    g_PatchedEntries.clear();

    // CRITICAL FIX: Clear dynamic hooks map to prevent memory leak
    // and stale pointers on DLL unload
    {
        std::lock_guard<std::mutex> dynLock(g_DynamicHookLock);
        g_DynamicHooks.clear();
    }

    WrapperLog("IAT: All hooks restored");
}

// ============================================================================
// Dynamic Hooking Implementation (GetProcAddress)
// ============================================================================

FARPROC WINAPI DetourGetProcAddress(HMODULE hModule, LPCSTR lpProcName) {
    // CRITICAL: During process termination, static data may be invalid
    // External DLLs (e.g., opengl32.dll) may call GetProcAddress during their
    // atexit destructors. We must not access any static data that may have
    // been destroyed.
    if (IsProcessTerminating()) {
        return ::GetProcAddress(hModule, lpProcName);
    }

    // Call through this module's unpatched static import. PatchIATAllModules()
    // deliberately excludes capture_hook, so this cannot recurse. Do not cache a
    // self-resolved kernel32!GetProcAddress pointer here: that export can be a
    // suppressed CFG target and guarded indirect calls to it fast-fail in a
    // CFG-enabled host even though ordinary IAT calls are valid.
    FARPROC proc = ::GetProcAddress(hModule, lpProcName);

    // Cooperative dejection leaves published function pointers resident. Do
    // not hand out any new CE wrappers while dormant: callers must receive the
    // exact address the current module export chain resolved.
    if (HookIsShuttingDown()) {
        return proc;
    }

    // If getting address failed, or if name is invalid (ordinal), return result
    // immediately
    if (!proc || (uintptr_t)lpProcName < 0x10000) {
        return proc;
    }

    // CRITICAL: Resolve EAT-patching pollution. When the EAT (Export Address Table)
    // of a module like d3d11.dll has been patched via PatchEAT(), the original
    // GetProcAddress now returns our wrapper function instead of the real function.
    // This is intentional for non-system callers (the game), but causes infinite
    // recursion when system DLLs (SysWOW64\SysWOW64) or overlay DLLs internally call
    // GetProcAddress on the same function — they get our wrapper, call it, wrapper
    // calls the original, original calls GetProcAddress on itself → loop.
    //
    // Fix: if `proc` matches one of our registered hook functions, resolve it to
    // the stored original address. This ensures system/overlay callers always get
    // the real function even when the EAT is patched.
    {
        // Check all registered hooks for a match against this proc address.
        // We hold g_DynamicHookLock, so the map is stable.
        std::lock_guard<std::mutex> lock(g_DynamicHookLock);
        for (const auto& hookEntry : g_DynamicHooks) {
            if ((void*)proc == hookEntry.second.hookFunction && hookEntry.second.outOriginal &&
                *hookEntry.second.outOriginal) {
                proc = (FARPROC)*hookEntry.second.outOriginal;
                break;
            }
        }
    }

    // Get module name for logging
    char moduleName[MAX_PATH] = {0};
    if (hModule) {
        GetModuleFileNameA(hModule, moduleName, MAX_PATH);
        // Extract just the filename
        char* p = moduleName + strlen(moduleName);
        while (p > moduleName && *(p - 1) != '\\' && *(p - 1) != '/') {
            p--;
        }
        if (p > moduleName) {
            memmove(moduleName, p, strlen(p) + 1);
        }
    }

    // Check if we have a hook for this function name
    std::lock_guard<std::mutex> lock(g_DynamicHookLock);
    auto it = g_DynamicHooks.find(lpProcName);
    if (it != g_DynamicHooks.end()) {
        // Don't intercept GetProcAddress calls originating from Windows system DLLs or known
        // overlay DLLs. Those DLLs call GetProcAddress internally for their own implementation
        // purposes. Intercepting such calls causes mutual recursion with third-party overlays
        // (e.g., Steam's gameoverlayrenderer64) that also hook DXGI/D3D factory functions:
        //   Wrapped_CreateDXGIFactory2 -> oCreateDXGIFactory2 (Steam's hook) ->
        //   Steam reads a stored "original" that received our wrapper -> infinite loop.
        {
            void* callerAddr = __builtin_return_address(0);
            HMODULE callerMod = nullptr;
            if (GetModuleHandleExA(
                    GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                    (LPCSTR)callerAddr, &callerMod) &&
                callerMod) {
                char callerPath[MAX_PATH] = {};
                if (GetModuleFileNameA(callerMod, callerPath, sizeof(callerPath))) {
                    for (char* p = callerPath; *p; ++p)
                        *p = (char)tolower((unsigned char)*p);
                    const bool callerIsSystemModule =
                        strstr(callerPath, "\\system32\\") || strstr(callerPath, "\\syswow64\\");
                    const bool callerIsThirdPartyOverlayModule =
                        ce::overlay_compat::IsThirdPartyOverlayModulePath(callerPath);
                    const bool callerIsCaptureHookModule = strstr(callerPath, "capture_hook") != nullptr;
                    const bool callerIsWrapperModule = strstr(callerPath, "d3d12_wrappers") != nullptr;
                    const bool callerIsStreamlineFrameGenerationModule =
                        ce::overlay_compat::IsStreamlineFrameGenerationModulePath(callerPath);
                    const bool callerIsFFXFrameGenerationModule =
                        ce::overlay_compat::IsFFXFrameGenerationModulePath(callerPath);
                    const bool targetIsStreamlineFrameGenerationModule =
                        ce::overlay_compat::IsStreamlineFrameGenerationModulePath(moduleName);
                    const bool targetIsFFXFrameGenerationModule =
                        ce::overlay_compat::IsFFXFrameGenerationModulePath(moduleName);
                    const bool allowNgxFgPresetResolution = ShouldAllowNgxFrameGenerationPresetDynamicHook(
                        ce::ngx_fg_preset::IsArmed(),
                        ce::ngx_fg_preset::IsFrameGenerationSnippetModulePath(callerPath), lpProcName);
                    if (!allowNgxFgPresetResolution &&
                        ShouldBypassDynamicHookForCaller(
                            callerIsSystemModule, callerIsThirdPartyOverlayModule, callerIsCaptureHookModule,
                            callerIsWrapperModule, callerIsStreamlineFrameGenerationModule,
                            callerIsFFXFrameGenerationModule, targetIsStreamlineFrameGenerationModule,
                            targetIsFFXFrameGenerationModule, lpProcName)) {
                        if (ShouldAllowStreamlineProxyExportToBypassDynamicHook(targetIsStreamlineFrameGenerationModule,
                                                                                lpProcName)) {
                            static std::atomic<int> s_streamlineProxyBypassLogCount{0};
                            const int bypassLogCount =
                                s_streamlineProxyBypassLogCount.fetch_add(1, std::memory_order_relaxed);
                            if (bypassLogCount < 10 || (bypassLogCount % 100) == 0) {
                                HookLogImportant(
                                    "GetProcAddress: Leaving Streamline proxy export %s from %s unmodified "
                                    "(orig=%p) so DLSS-G owns its factory/swapchain interposer",
                                    lpProcName, moduleName[0] ? moduleName : "unknown", proc);
                            }
                        }
                        return proc;
                    }
                }
            }
        }

        if (!ShouldApplyDynamicHookForModule(it->second.moduleFilter, moduleName[0] ? moduleName : nullptr, hModule)) {
            return proc;
        }

        // We found a hook!
        // Store the original address if requested
        if (it->second.outOriginal && *it->second.outOriginal == nullptr) {
            *it->second.outOriginal = (void*)proc;
        }

        // Return our hook address
        WrapperLog("GetProcAddress: Intercepting %s from %s (orig=%p, hook=%p)", lpProcName,
                   moduleName[0] ? moduleName : "unknown", proc, it->second.hookFunction);
        if (IsFFXApiDynamicHookName(lpProcName)) {
            static std::atomic<int> s_ffxDynamicInterceptLogCount{0};
            const int logCount = s_ffxDynamicInterceptLogCount.fetch_add(1, std::memory_order_relaxed);
            if (logCount < 20 || (logCount % 300) == 0) {
                HookLogImportant(
                    "GetProcAddress: Intercepted FFX API %s from %s (orig=%p hook=%p log=%d) - native FSR "
                    "present-callback bridge can arm before unsafe overlay fallback",
                    lpProcName, moduleName[0] ? moduleName : "unknown", proc, it->second.hookFunction, logCount + 1);
            }
        }
        // CRITICAL: Log D3D11CreateDevice intercept at high visibility
        if (strcmp(lpProcName, "D3D11CreateDevice") == 0 || strcmp(lpProcName, "D3D11CreateDeviceAndSwapChain") == 0) {
            HookLogImportant(
                "GetProcAddress: Intercepted %s -> Wrapped_%s (game=%s) — "
                "preventing UE3 vtable-cache bypass",
                lpProcName, lpProcName, moduleName[0] ? moduleName : "unknown");
        }
        return (FARPROC)it->second.hookFunction;
    }

    // Debug logging for DXGI/D3D functions that might be looked up
    static std::atomic<int> s_LogCount{0};
    if (s_LogCount < 50) {
        if (strstr(lpProcName, "D3D11") || strstr(lpProcName, "DXGI") || strstr(lpProcName, "D3D12") ||
            strstr(lpProcName, "D3D10")) {
            WrapperLog("GetProcAddress: %s from %s (no hook registered)", lpProcName,
                       moduleName[0] ? moduleName : "unknown");
            s_LogCount++;
        }
    }

    return proc;
}

void RegisterDynamicHook(const char* functionName, void* hookFunction, void** outOriginal) {
    RegisterDynamicHookFiltered(functionName, hookFunction, outOriginal, nullptr);
}

void RegisterDynamicHookFiltered(const char* functionName, void* hookFunction, void** outOriginal,
                                 DynamicHookModuleFilter moduleFilter) {
    std::lock_guard<std::mutex> lock(g_DynamicHookLock);
    g_DynamicHooks[functionName] = {hookFunction, outOriginal, moduleFilter};
}

void InitializeGetProcAddressHook() {
    WrapperLog("IAT: Initializing GetProcAddress hook for dynamic interception...");

    void* dummy = nullptr;
    if (PatchIATAllModules("kernel32.dll", "GetProcAddress", (void*)DetourGetProcAddress, &dummy)) {
        WrapperLog("IAT: GetProcAddress hook initialized");
    } else {
        WrapperLog("IAT: No eligible GetProcAddress imports found");
    }
}

}  // namespace IATHook
