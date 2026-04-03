/**
 * IAT (Import Address Table) Patching Utility
 *
 * Replaces MinHook for API hooking by patching import tables directly.
 * This is safer and doesn't require runtime code modification.
 */

#pragma once

#include <windows.h>
#include <string>
#include <vector>

namespace IATHook {

/**
 * Patch a single imported function in a module's IAT
 *
 * @param targetModule - Module whose IAT to patch (NULL for main exe)
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
 * Initialize advapi32 hooks (RegQueryValueExW)
 */
bool InitializeAdvapi32Hooks(void* RegQueryValueExWHook, void** pOriginalRegQueryValueExW);

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
 * Initialize GetProcAddress hook to enable dynamic hooking.
 * Should be called once during initialization.
 */
void InitializeGetProcAddressHook();

/**
 * Cleanup all IAT patches
 */
void ShutdownIATHooks();

}  // namespace IATHook
