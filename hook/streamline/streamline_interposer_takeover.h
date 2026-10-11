#pragma once

#include <windows.h>
#include <cstddef>
#include <cstdint>

/*
 * In-process same-generation takeover for a statically imported sl.interposer.dll.
 *
 * When a title statically links against sl.interposer.dll (e.g. The Witcher 3 DX12,
 * Alan Wake 2), ntdll!LdrpInitializeProcess maps the game-directory copy before CE's
 * DllMain runs, so neither LoadLibrary/LdrLoadDll redirection nor HookThread preloading
 * can prevent that initial mapping. However, the game does not call slInit until after
 * DllMain returns (measured at +36 ms in Witcher 3 session 20261010_225115, while
 * HookThread was still inside early setup).
 *
 * Without parent-process hooks, CBT hooks, or launching the game through CE, CE takes
 * over the uninitialized resident interposer in two phases:
 *
 *   1. ArmEarlyFromDllMain (under the loader lock, zero LoadLibrary calls):
 *      When a foreign sl.interposer.dll is already mapped, no sl.common.dll is loaded
 *      yet, and the configured streamline_dll_path contains a same-generation
 *      sl.interposer.dll, CE patches the process's sl.interposer.dll import table to
 *      lightweight takeover thunks before DllMain releases the loader lock.
 *
 *   2. EnsureCustomInterposerReady (outside DllMain):
 *      Called eagerly at the start of HookThread (and inside PlaceStreamlinePluginSet),
 *      or lazily by the first takeover thunk (including dynamic export lookups) if
 *      the game's main thread races ahead of HookThread. Loads and pins the configured
 *      sl.interposer.dll, resolves its exports, rebinds all sl.interposer.dll IAT slots
 *      directly to the custom interposer exports, and redirects GetModuleHandle* and
 *      GetProcAddress queries for sl.interposer.dll to the custom module so sl.interposer
 *      and sl.common discover plugins in the configured override directory.
 */

namespace ce::streamline_interposer_takeover {

inline constexpr const char* kTakeoverExportNames[] = {
    "slInit",
    "slShutdown",
    "slIsFeatureSupported",
    "slIsFeatureLoaded",
    "slSetFeatureLoaded",
    "slEvaluateFeature",
    "slAllocateResources",
    "slFreeResources",
    "slSetTag",
    "slSetTagForFrame",
    "slSetConstants",
    "slGetFeatureFunction",
    "slGetFeatureRequirements",
    "slGetFeatureVersion",
    "slGetNativeInterface",
    "slGetNewFrameToken",
    "slSetD3DDevice",
    "slSetVulkanInfo",
    "slUpgradeInterface",
    "slGetPluginFunction",
    "slSetFeatureConstants",
    "slGetFeatureSettings",
    "slGetFeatureConfiguration",
    "slSetFeatureEnabled",
    "slIsFeatureEnabled",
    "slGetHooks",
    "slGetNumHooks",
    "CreateDXGIFactory",
    "CreateDXGIFactory1",
    "CreateDXGIFactory2",
    "DXGIGetDebugInterface1",
    "D3D12CreateDevice",
    "D3D12GetDebugInterface",
    "D3D12GetInterface",
    "D3D12SerializeRootSignature",
    "D3D12SerializeVersionedRootSignature",
    "D3D12CreateRootSignatureDeserializer",
    "D3D12CreateVersionedRootSignatureDeserializer",
    "D3D12EnableExperimentalFeatures",
    "vkGetInstanceProcAddr",
    "vkGetDeviceProcAddr",
    "vkCreateInstance",
    "vkCreateDevice",
    "vkEnumerateInstanceExtensionProperties",
    "vkEnumerateInstanceLayerProperties",
    "vkEnumeratePhysicalDevices",
    "vkCreateSwapchainKHR",
    "vkQueuePresentKHR",
};

inline constexpr size_t kTakeoverExportCount = sizeof(kTakeoverExportNames) / sizeof(kTakeoverExportNames[0]);

void ArmEarlyFromDllMain();
bool EnsureCustomInterposerReady();

bool IsTakeoverArmed();
bool IsTakeoverActive();
bool ShouldAllowTakeoverInterposerLoad(const char* candidatePath);
void NoteTakeoverInterposerMapped(HMODULE module, const char* resolvedPath);


HMODULE ActiveInterposerOr(HMODULE fallback);
HMODULE ResolveModuleByNameForRedirect(const char* baseName);
HMODULE GetCustomInterposerModule();
HMODULE GetSupersededInterposerModule();
bool IsSupersededInterposerModule(HMODULE module);
bool IsOwnedStreamlineInterposerModule(const char* sourceModule, HMODULE ownerModule);
HMODULE RedirectInterposerModuleForGetProcAddress(HMODULE requestedModule);
// Lookup only: returns a deferred export gate without loading or waiting in DLL callbacks.
FARPROC ResolvePendingTakeoverExport(HMODULE requestedModule, LPCSTR name);

void InstallModuleHandleHooksForAllModules();
void PatchModuleHandleIatForModule(HMODULE module);
void PatchInterposerIatForLateLoadedModule(HMODULE module, const char* moduleNameOrPath);

HMODULE WINAPI HookedGetModuleHandleA(LPCSTR lpModuleName);
HMODULE WINAPI HookedGetModuleHandleW(LPCWSTR lpModuleName);
BOOL WINAPI HookedGetModuleHandleExA(DWORD dwFlags, LPCSTR lpModuleName, HMODULE* phModule);
BOOL WINAPI HookedGetModuleHandleExW(DWORD dwFlags, LPCWSTR lpModuleName, HMODULE* phModule);

}  // namespace ce::streamline_interposer_takeover
