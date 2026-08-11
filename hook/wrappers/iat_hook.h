/**
 * IAT (Import Address Table) Patching Utility
 *
 * Replaces MinHook for API hooking by patching import tables directly.
 * This is safer and doesn't require runtime code modification.
 */

#pragma once

#include <windows.h>
#include <algorithm>
#include <cstring>
#include <cwctype>
#include <string>
#include <vector>

namespace IATHook {

using DynamicHookModuleFilter = bool (*)(const char* moduleBaseName, HMODULE module);
using IATTargetModuleFilter = bool (*)(HMODULE module, const wchar_t* modulePath);

inline bool IsPathUnderDirectoryRoot(const wchar_t* modulePath, const wchar_t* directory) {
    if (!modulePath || !*modulePath || !directory || !*directory) {
        return false;
    }
    std::wstring lowerPath(modulePath);
    std::wstring lowerDirectory(directory);
    std::transform(lowerPath.begin(), lowerPath.end(), lowerPath.begin(),
                   [](wchar_t ch) { return static_cast<wchar_t>(std::towlower(ch)); });
    std::transform(lowerDirectory.begin(), lowerDirectory.end(), lowerDirectory.begin(),
                   [](wchar_t ch) { return static_cast<wchar_t>(std::towlower(ch)); });
    while (!lowerDirectory.empty() && (lowerDirectory.back() == L'\\' || lowerDirectory.back() == L'/')) {
        lowerDirectory.pop_back();
    }
    return !lowerDirectory.empty() && lowerPath.rfind(lowerDirectory + L"\\", 0) == 0;
}

inline bool IsWindowsSystemModulePathUnderRoot(const wchar_t* modulePath, const wchar_t* windowsDirectory) {
    if (!IsPathUnderDirectoryRoot(modulePath, windowsDirectory)) {
        return false;
    }
    std::wstring lowerPath(modulePath);
    std::wstring lowerWindowsDirectory(windowsDirectory);
    std::transform(lowerPath.begin(), lowerPath.end(), lowerPath.begin(),
                   [](wchar_t ch) { return static_cast<wchar_t>(std::towlower(ch)); });
    std::transform(lowerWindowsDirectory.begin(), lowerWindowsDirectory.end(), lowerWindowsDirectory.begin(),
                   [](wchar_t ch) { return static_cast<wchar_t>(std::towlower(ch)); });
    while (!lowerWindowsDirectory.empty() &&
           (lowerWindowsDirectory.back() == L'\\' || lowerWindowsDirectory.back() == L'/')) {
        lowerWindowsDirectory.pop_back();
    }
    const std::wstring system32Prefix = lowerWindowsDirectory + L"\\system32\\";
    const std::wstring syswow64Prefix = lowerWindowsDirectory + L"\\syswow64\\";
    return lowerPath.rfind(system32Prefix, 0) == 0 || lowerPath.rfind(syswow64Prefix, 0) == 0;
}

inline bool IsWindowsSystemModulePath(const wchar_t* modulePath) {
    wchar_t windowsDirectory[MAX_PATH] = {};
    return GetWindowsDirectoryW(windowsDirectory, MAX_PATH) != 0 &&
           IsWindowsSystemModulePathUnderRoot(modulePath, windowsDirectory);
}

inline bool IsGraphicsProxyModuleBaseName(const wchar_t* modulePath) {
    if (!modulePath)
        return false;
    const wchar_t* base = modulePath;
    for (const wchar_t* cursor = modulePath; *cursor; ++cursor) {
        if (*cursor == L'\\' || *cursor == L'/')
            base = cursor + 1;
    }
    static constexpr const wchar_t* candidates[] = {
        L"dxgi.dll",       L"d3d12.dll",       L"d3d11.dll",      L"d3d10_1.dll",
        L"d3d10.dll",      L"d3d9.dll",        L"d3d8.dll",       L"ddraw.dll",
        L"opengl32.dll",   L"dinput8.dll",     L"dsound.dll",     L"xinput1_3.dll",
        L"xinput9_1_0.dll", L"winmm.dll",       L"version.dll",    L"wininet.dll",
        L"winhttp.dll",    L"dbghelp.dll",     L"nvngx.dll",
    };
    for (const wchar_t* candidate : candidates) {
        if (_wcsicmp(base, candidate) == 0)
            return true;
    }
    return false;
}

inline bool IsNonSystemGraphicsProxyModulePath(const wchar_t* modulePath) {
    return IsGraphicsProxyModuleBaseName(modulePath) && !IsWindowsSystemModulePath(modulePath);
}

inline bool ShouldApplyDynamicHookForModule(DynamicHookModuleFilter moduleFilter, const char* moduleBaseName,
                                            HMODULE module) {
    return !moduleFilter || moduleFilter(moduleBaseName, module);
}

inline bool IsFFXApiDynamicHookName(const char* functionName) {
    if (!functionName) {
        return false;
    }

    return strcmp(functionName, "ffxCreateContext") == 0 || strcmp(functionName, "ffxDestroyContext") == 0 ||
           strcmp(functionName, "ffxConfigure") == 0;
}

inline bool IsDXGIFactoryDynamicHookName(const char* functionName) {
    if (!functionName) {
        return false;
    }

    return strcmp(functionName, "CreateDXGIFactory") == 0 || strcmp(functionName, "CreateDXGIFactory1") == 0 ||
           strcmp(functionName, "CreateDXGIFactory2") == 0;
}

inline bool ShouldAllowStreamlineProxyExportToBypassDynamicHook(bool targetIsStreamlineFrameGenerationModule,
                                                                const char* functionName) {
    // Streamline exposes DXGI factory proxy exports. Games that explicitly fetch
    // those from sl.interposer.dll must receive the Streamline proxy, not CE's
    // generic DXGI wrapper; otherwise DLSS-G can enable on a swapchain that never
    // went through Streamline's factory/swapchain interposer.
    return targetIsStreamlineFrameGenerationModule && IsDXGIFactoryDynamicHookName(functionName);
}

inline bool IsNvApiQueryInterfaceDynamicHookName(const char* functionName) {
    return functionName && strcmp(functionName, "nvapi_QueryInterface") == 0;
}

// nvngx_dlssg.dll reads the DLSS FG render preset out of the driver settings
// through nvapi_QueryInterface, so `dlss_fg_preset` can only be honored if that
// one resolution reaches CE's dispatcher. Streamline/FG modules are otherwise
// deliberately left on untouched driver pointers, so this exception stays as
// narrow as the feature: the DLSS-G snippet, that single export, and only while
// a preset is actually configured.
inline bool ShouldAllowNgxFrameGenerationPresetDynamicHook(bool ngxFgPresetOverrideArmed,
                                                           bool callerIsNgxFrameGenerationSnippet,
                                                           const char* functionName) {
    return ngxFgPresetOverrideArmed && callerIsNgxFrameGenerationSnippet &&
           IsNvApiQueryInterfaceDynamicHookName(functionName);
}

inline bool ShouldAllowDynamicHookForThirdPartyOverlayCaller(bool targetIsFFXFrameGenerationModule,
                                                             const char* functionName) {
    // GTA Enhanced can route official FFX module lookups through an overlay
    // shim. The generic overlay-caller guard is still correct for graphics API
    // hooks, but FFX API hooks must remain visible there so CE can install the
    // native FSR present-callback bridge instead of falling back to unsafe
    // normal overlay GPU submission.
    return targetIsFFXFrameGenerationModule && IsFFXApiDynamicHookName(functionName);
}

inline bool ShouldBypassDynamicHookForCaller(bool callerIsSystemModule, bool callerIsThirdPartyOverlayModule,
                                             bool callerIsCaptureHookModule, bool callerIsWrapperModule,
                                             bool callerIsStreamlineFrameGenerationModule,
                                             bool callerIsFFXFrameGenerationModule,
                                             bool targetIsStreamlineFrameGenerationModule,
                                             bool targetIsFFXFrameGenerationModule, const char* functionName) {
    if (ShouldAllowStreamlineProxyExportToBypassDynamicHook(targetIsStreamlineFrameGenerationModule, functionName)) {
        return true;
    }

    if (callerIsSystemModule || callerIsCaptureHookModule || callerIsWrapperModule ||
        callerIsStreamlineFrameGenerationModule || callerIsFFXFrameGenerationModule) {
        return true;
    }

    if (callerIsThirdPartyOverlayModule &&
        !ShouldAllowDynamicHookForThirdPartyOverlayCaller(targetIsFFXFrameGenerationModule, functionName)) {
        return true;
    }

    return false;
}

/**
 * Patch a single imported function in a module's IAT
 *
 * @param targetModule - Module whose IAT to patch (NULL
 * for main exe)
 * @param sourceModule - DLL name that exports the function (e.g., "dxgi.dll")
 * @param functionName - Name of the function to hook
 * @param hookFunction - Our replacement function
 * @param outOriginal  - Receives pointer to original function
 * @return true on success
 */
bool PatchIAT(HMODULE targetModule, const char* sourceModule, const char* functionName, void* hookFunction,
              void** outOriginal);

/**
 * Patch a function in all loaded modules
 * Useful for hooking functions across DLLs
 */
bool PatchIATAllModules(const char* sourceModule, const char* functionName, void* hookFunction, void** outOriginal);

// Patch a function in loaded modules accepted by targetFilter. Modules whose
// path cannot be resolved are rejected when a filter is supplied.
bool PatchIATAllModulesFiltered(const char* sourceModule, const char* functionName, void* hookFunction,
                                void** outOriginal, IATTargetModuleFilter targetFilter);

/**
 * Restore original IAT entry
 */
bool RestoreIAT(HMODULE targetModule, const char* sourceModule, const char* functionName, void* originalFunction);

/**
 * EAT (Export Address Table) hooking for intercepting exports
 * Used when we want to intercept functions that modules export
 */
bool PatchEAT(HMODULE exportingModule, const char* functionName, void* hookFunction, void** outOriginal);

/**
 * Initialize IAT hooks for DXGI/D3D
 * Patches CreateDXGIFactory*, D3D11CreateDevice*, etc.
 */
bool InitializeDXGIHooks();
bool InitializeD3D10Hooks();
bool InitializeD3D11Hooks();
bool InitializeD3D12Hooks();
bool InitializeD3D9Hooks();
bool InitializeDDrawHooks();
// Note: InitializeVulkanHooks removed - Vulkan is now handled by
// VK_LAYER_CE_overlay

/**
 * Initialize kernel32 hooks (LoadLibrary*, CreateProcess*)
 * These are critical for late injection support
 */
bool InitializeKernel32Hooks(void* LoadLibraryAHook, void** pOriginalLoadLibraryA, void* LoadLibraryWHook,
                             void** pOriginalLoadLibraryW, void* LoadLibraryExAHook, void** pOriginalLoadLibraryExA,
                             void* LoadLibraryExWHook, void** pOriginalLoadLibraryExW, void* CreateProcessAHook,
                             void** pOriginalCreateProcessA, void* CreateProcessWHook, void** pOriginalCreateProcessW);

/**
 * Register a hook for GetProcAddress interception.
 * Used for dynamic hooking of APIs loaded via GetProcAddress (e.g. OpenGL,
 * NVNGX).
 *
 * @param functionName Name of the function to intercept (e.g. "wglSwapBuffers")
 * @param hookFunction Your replacement function pointer
 * @param outOriginal  Pointer to variable that will hold the original function
 * address
 */
void RegisterDynamicHook(const char* functionName, void* hookFunction, void** outOriginal);

/**
 * Register a hook for GetProcAddress interception, limited to matching export
 * owner modules.
 */
void RegisterDynamicHookFiltered(const char* functionName, void* hookFunction, void** outOriginal,
                                 DynamicHookModuleFilter moduleFilter);

/**
 * Initialize GetProcAddress hook to enable dynamic hooking.
 * Should be called once during initialization.
 */
void InitializeGetProcAddressHook();

/**
 * Detour function for GetProcAddress. Called when GetProcAddress is invoked
 * by any module whose IAT we patched. Checks g_DynamicHooks and returns the
 * hook function for matching function names (unless the caller is a system
 * DLL or third-party overlay, in which case it returns the real function).
 *
 * @param hModule Module handle to query
 * @param lpProcName Function name to look up
 * @return Hook function if registered, otherwise the real function
 */
FARPROC WINAPI DetourGetProcAddress(HMODULE hModule, LPCSTR lpProcName);

/**
 * Cleanup all IAT patches
 */
void ShutdownIATHooks();

}  // namespace IATHook
