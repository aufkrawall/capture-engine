#include "ffx_hook_internal.h"
#include "../../common/log_meter.h"

namespace FFXHook {
void* GetPresentCallbackBridgeKey(void* ffx_hook_context) {
    return GetOrCreatePresentCallbackBridgeKey(ffx_hook_context);
}
}

namespace FFXHook {
void RegisterDynamicHooks() {
    ffx_hook_RegisterDynamicHooksOnce();
}
}

namespace FFXHook {
bool InstallBridgeOnTrackedContexts(void* swapChain) {
    return InstallBridgeOnTrackedContextsImpl(swapChain);
}
}

namespace FFXHook {
void Init() {
    std::lock_guard<std::mutex> lock(ffx_hook_g_InitMutex);

    static int s_initCallCount = 0;
    ++s_initCallCount;

    if (!ffx_hook_g_Initialized.load(std::memory_order_acquire) && !ffx_hook_g_NoModulesLogged.load(std::memory_order_acquire)) {
        HookLog("FFX Hook: Initializing...");
    }

    ffx_hook_RegisterDynamicHooksOnce();

    // Try to find FFX modules.
    // These cover both the older explicit FG DLL names and newer generic
    // FidelityFX runtime DLL names observed in GTA V Enhanced.
    // Also includes dlssg-to-fsr3 mod DLLs that redirect DLSS FG to FSR FG.
    const wchar_t* ffxModules[] = {
        // FSR 4 / FSR 3.1 DLLs (UE5 native integration) - check first.
        L"amd_fidelityfx_framegeneration_dx12.dll",
        L"amd_fidelityfx_framegeneration_vk.dll",
        // GTA V Enhanced can load the generic FidelityFX runtime DLL name while
        // still routing native frame generation through the FFX API exports.
        L"amd_fidelityfx_dx12.dll",
        L"amd_fidelityfx_vk.dll",
        // Standard AMD FSR FG DLLs.
        L"amd_fidelityfx_fg.dll",
        L"ffx_frameinterpolation_x64.dll",
        L"amd_fidelityfx_framegeneration.dll",
        L"ffx_framegeneration.dll",
        // dlssg-to-fsr3 mod - uses nvngx_dlssg.dll as a proxy that calls FFX API.
        L"nvngx_dlssg.dll",
        // FSR3 FG mod common names.
        L"fsr3fg.dll",
        L"fsr3mod.dll",
    };

    const char* ffxModuleNames[] = {
        "amd_fidelityfx_framegeneration_dx12.dll",
        "amd_fidelityfx_framegeneration_vk.dll",
        "amd_fidelityfx_dx12.dll",
        "amd_fidelityfx_vk.dll",
        "amd_fidelityfx_fg.dll",
        "ffx_frameinterpolation_x64.dll",
        "amd_fidelityfx_framegeneration.dll",
        "ffx_framegeneration.dll",
        "nvngx_dlssg.dll",
        "fsr3fg.dll",
        "fsr3mod.dll",
    };

    bool foundSupportedModule = false;
    for (size_t i = 0; i < _countof(ffxModules); ++i) {
        HMODULE hMod = GetModuleHandleW(ffxModules[i]);
        if (hMod) {
            // Metered diagnostic: a module that never becomes hookable (e.g. the
            // real nvngx_dlssg.dll has no FFX exports) re-runs this scan about
            // once per second, and the line below used to repeat on every scan
            // because the hook latch never sticks (516 copies in one 8-minute
            // trace session). Log the first 10 observations and then every 300th
            // as a heartbeat; a genuinely new module pointer always logs.
            static HMODULE s_lastFoundModuleLogged = nullptr;
            static std::atomic<int> s_foundModuleLogCount{0};
            const bool newModulePointer = s_lastFoundModuleLogged != hMod;
            if ((!ffx_hook_g_Initialized.load(std::memory_order_acquire) || ffx_hook_g_HookedModule != hMod) &&
                (newModulePointer ||
                 ce::log_meter::ShouldLogCadence(
                     static_cast<uint32_t>(s_foundModuleLogCount.fetch_add(1, std::memory_order_relaxed) + 1), 10,
                     300))) {
                HookLog("FFX Hook: Found module %s at %p", ffxModuleNames[i], hMod);
                s_lastFoundModuleLogged = hMod;
            }
            g_FGCompat.SetFSRFGSupportPresent(true);
            if (ffx_hook_InstallHooksForModule(hMod, ffxModuleNames[i])) {
                foundSupportedModule = true;
                continue;
            }
            // Module exists but has no FFX exports (e.g. real nvngx_dlssg.dll)
            static std::atomic<int> s_moduleWithoutExportsLogCount{0};
            const int logCount = s_moduleWithoutExportsLogCount.fetch_add(1, std::memory_order_relaxed) + 1;
            if (logCount <= 20 || (logCount % 300) == 0) {
                HookLog("FFX Hook: Module %s has no FFX exports, continuing search (log=%d)", ffxModuleNames[i],
                        logCount);
            }
        }
    }

    if (foundSupportedModule) {
        ffx_hook_g_Initialized.store(true, std::memory_order_release);
        ffx_hook_g_NoModulesLogged.store(false, std::memory_order_release);
        return;
    }

    ffx_hook_g_Initialized.store(false, std::memory_order_release);
    ffx_hook_g_HookedModule = nullptr;
    ffx_hook_g_DefaultPresentCallback = nullptr;

    if (!ffx_hook_g_NoModulesLogged.exchange(true, std::memory_order_acq_rel)) {
        HookLog("FFX Hook: No FFX modules found, hooks not installed");
    }

    // One-time diagnostic at 30th retry: enumerate loaded modules for debugging
    if (s_initCallCount == 30) {
        HookLogImportant("FFX Hook: Module enumeration diagnostic (call #%d):", s_initCallCount);
        std::vector<HMODULE> hMods;
        if (ce::EnumerateProcessModules(GetCurrentProcess(), hMods)) {
            int found = 0;
            for (size_t i = 0; i < hMods.size(); i++) {
                wchar_t modName[MAX_PATH];
                if (GetModuleFileNameW(hMods[i], modName, MAX_PATH)) {
                    std::wstring lower(modName);
                    for (auto& c : lower)
                        c = towlower(c);
                    if (lower.find(L"fidelity") != std::wstring::npos || lower.find(L"ffx") != std::wstring::npos ||
                        lower.find(L"framegen") != std::wstring::npos || lower.find(L"fsr") != std::wstring::npos ||
                        lower.find(L"amd_") != std::wstring::npos) {
                        char narrowName[MAX_PATH];
                        WideCharToMultiByte(CP_UTF8, 0, modName, -1, narrowName, MAX_PATH, NULL, NULL);
                        HookLogImportant("FFX Hook:   Loaded: %s", narrowName);
                        found++;
                    }
                }
            }
            if (found == 0) {
                HookLogImportant("FFX Hook:   No AMD/FFX/FSR modules among %zu inspected", hMods.size());
            }
        }
    }
}
}

namespace FFXHook {
bool IsInitialized() {
    return ffx_hook_g_Initialized.load(std::memory_order_acquire);
}
}

namespace FFXHook {
void Shutdown() {
    std::lock_guard<std::mutex> lock(ffx_hook_g_InitMutex);

    if (!ffx_hook_g_Initialized.load(std::memory_order_acquire)) {
        return;
    }

    HookLog("FFX Hook: Shutting down...");

    // Remove IAT hooks
    if (ffx_hook_g_HookedModule) {
        // Note: IATHook::RemoveHook would need to be implemented
        // For now, we just clear the state - hooks will naturally be cleaned up
        // when the DLL unloads
    }

    // Restore the FFX proxy-swapchain Present vtable hook (guarded: only touches the class vtable when it
    // is still readable and still points at CE's detour — the FFX module may already be unloaded here).
    DX12_RemoveFFXProxyPresentHook("FFX hook shutdown");

    // Restore client-owned pre-resolved pointer slots before clearing original export addresses.
    ce::ffx_cached_pointer_router::Shutdown();

    // Cleanup VEH breakpoint hook
    if (ffx_hook_g_ffxConfigureVehHandle) {
        RemoveVectoredExceptionHandler(ffx_hook_g_ffxConfigureVehHandle);
        ffx_hook_g_ffxConfigureVehHandle = nullptr;
    }
    RestoreFfxConfigureBreakpointIfCurrent(ffx_hook_g_ffxConfigureTarget.load(std::memory_order_acquire), "FFX hook shutdown");
    ffx_hook_g_ffxConfigureVehInstalled = false;
    ffx_hook_g_ffxConfigureVehArmed.store(false, std::memory_order_release);
    ffx_hook_g_ffxConfigureTarget.store(nullptr, std::memory_order_release);

    ffx_hook_g_Original_ffxCreateContext = nullptr;
    ffx_hook_g_Original_ffxDestroyContext = nullptr;
    ffx_hook_g_Original_ffxConfigure = nullptr;
    ffx_hook_g_DurableCachedConfigureRouteActive.store(false, std::memory_order_release);
    ffx_hook_g_HookedModule = nullptr;
    ffx_hook_g_DefaultPresentCallback = nullptr;
    FFXHook_ClearSubstituteUiReRegistration();
    DX12_UnregisterNativeFSRSwapchainPresentationQueue(nullptr, "FFX hook shutdown");
    {
        std::lock_guard<std::mutex> contextLock(ffx_hook_g_ContextMapMutex);
        ffx_hook_g_ContextTypeMap.clear();
        ffx_hook_g_FrameGenerationRoutingByContext.clear();
    }
    {
        std::lock_guard<std::mutex> bridgeLock(ffx_hook_g_PresentCallbackBridgeMutex);
        ffx_hook_g_PresentCallbackBridgeKeys.clear();
    }
    ffx_hook_g_FGContextCount.store(0, std::memory_order_release);
    DX12_ClearNativeFSRStartupConfigureArming("FFX hook shutdown");
    g_FGCompat.SetFSRFGActive(false);
    g_FGCompat.SetFSRFGSupportPresent(false);
    ffx_hook_g_NoModulesLogged.store(false, std::memory_order_release);
    ffx_hook_g_Initialized.store(false, std::memory_order_release);

    HookLog("FFX Hook: Shutdown complete");
}
}
