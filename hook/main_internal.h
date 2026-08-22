#pragma once

struct ExternalDumpStormRecord;

struct ExternalDumpGateDecision;

struct ChildInjectParams;

#include "../common/utils/scanner.h"

#include "apis/ddraw_hook.h"

#include "apis/dx11_hook.h"

#include "apis/dx12_hook.h"

#include "apis/dx8_hook.h"

#include "apis/dx9_hook.h"

#include "apis/opengl_hook.h"

#include "apis/streamline_hook.h"

// CRITICAL: windows.h MUST come before psapi.h and intrin.h
#include <windows.h>

#include <winternl.h>

#include <cstdio>

#include <intrin.h> // For __builtin_return_address

#include <psapi.h>

// Vulkan hook removed - using VK_LAYER_CE_overlay (ICD layer approach) instead
#include "../common/crash_dump_policy.h"

#include "../common/crash_handler.h"

#include "apis/ffx_hook.h" // FSR Frame Generation hook

#include "apis/nvngx_hook.h"

#include "capture/shared_capture.h"

#include "common/dll_utils.h"

#include "common/dxgi_shared.h"

#include "common/fg_detection.h"

#include "common/graphics_runtime_module_policy.h"

#include "common/hook_common.h"

#include "common/hook_context.h"

#include "common/dlss_indicator_spoof.h"

#include "apis/streamline_bridge.h"

#include "common/nv_lod_spread_override.h"

#include "common/input_manager.h"

#include "common/ipc_client.h"

#include "common/overlay_compat.h"

#include "common/perf_logger.h"

#include "common/screenshot_hook.h"

#include "common/streamline_runtime_policy.h"

#include "common/reflex_limiter.h"

#include "common/system_metrics.h"

#include "wrappers/d3dkmt_hook.h"

#include "wrappers/dxgi_swapchain_wrap.h"

#include "wrappers/iat_hook.h"

#include "wrappers/inline_hook.h"

#include "wrappers/wrapper_hooks.h"

#include <algorithm>

#include <atomic>

#include <cctype>

#include <cstddef>

#include <cstring>

#include <dbghelp.h>

#include <filesystem>

#include <fstream>

#include <memory>

#include <mutex>

#include <new>

#include <string>

#include <string_view>

#include <thread>

#include <unordered_map>

#include <vector>

using MiniDumpWriteDump_t = decltype(&MiniDumpWriteDump);

using RaiseFailFastException_t = VOID(WINAPI*)(PEXCEPTION_RECORD, PCONTEXT, DWORD);

using TerminateProcess_t = BOOL(WINAPI*)(HANDLE, UINT);

using ExitProcess_t = VOID(WINAPI*)(UINT);

using RtlExitUserProcess_t = VOID(NTAPI*)(NTSTATUS);

using NtTerminateProcess_t = NTSTATUS(NTAPI*)(HANDLE, NTSTATUS);

using InvalidParameterNoInfoNoReturn_t = void(__cdecl*)();

using InvokeWatson_t = void(__cdecl*)(const wchar_t*, const wchar_t*, const wchar_t*, unsigned int, uintptr_t);

using Abort_t = void(__cdecl*)();

using Terminate_t = void(__cdecl*)();

using Purecall_t = int(__cdecl*)();

enum class ExternalPreTerminationDumpResult { kUnavailable, kCaptured, kFailed, kTimedOut };

enum class ProcessCategory {
  PotentialGame,
  Launcher,
  InternalTool,
  Blacklisted
};

#include "../common/logging.h"

// LoadLibrary Hook Typedefs
typedef HMODULE(WINAPI *LoadLibraryA_t)(LPCSTR lpLibFileName);

typedef HMODULE(WINAPI *LoadLibraryW_t)(LPCWSTR lpLibFileName);

typedef HMODULE(WINAPI *LoadLibraryExA_t)(LPCSTR lpLibFileName, HANDLE hFile,
                                          DWORD dwFlags);

typedef HMODULE(WINAPI *LoadLibraryExW_t)(LPCWSTR lpLibFileName, HANDLE hFile,
                                          DWORD dwFlags);

typedef NTSTATUS(NTAPI *LdrLoadDll_t)(PWSTR SearchPath,
                                      PULONG DllCharacteristics,
                                      PUNICODE_STRING DllName,
                                      PVOID *BaseAddress);

// CreateProcess Hook Typedefs for child process injection
typedef BOOL(WINAPI *CreateProcessA_t)(LPCSTR, LPSTR, LPSECURITY_ATTRIBUTES,

                                       LPSECURITY_ATTRIBUTES, BOOL, DWORD,
                                       LPVOID, LPCSTR, LPSTARTUPINFOA,
                                       LPPROCESS_INFORMATION);

typedef BOOL(WINAPI *CreateProcessW_t)(LPCWSTR, LPWSTR, LPSECURITY_ATTRIBUTES,
                                       LPSECURITY_ATTRIBUTES, BOOL, DWORD,
                                       LPVOID, LPCWSTR, LPSTARTUPINFOW,
                                       LPPROCESS_INFORMATION);

// ----------------------------------------------------------------------------
// LdrRegisterDllNotification: authoritative, loader-safe DLL load/unload tracking
// for third-party-overlay detection. Fires for ALL load mechanisms (LoadLibrary,
// LdrLoadDll, static-import resolution) and — crucially — for UNLOADs, which the
// LoadLibrary/LdrLoadDll hooks do not see. The callback runs UNDER the loader lock,
// so it must stay loader-safe: read the notification's base-name UNICODE_STRING,
// match it against the static overlay list, and update an atomic. No GetModuleHandle,
// no LoadLibrary, no heap-heavy work. This keeps the Present hot path loader-free
// (it only reads the atomic) — the root-cause fix for the x86 Alt+Tab freeze.
// ----------------------------------------------------------------------------
#ifndef LDR_DLL_NOTIFICATION_REASON_LOADED
#define LDR_DLL_NOTIFICATION_REASON_LOADED 1
#define LDR_DLL_NOTIFICATION_REASON_UNLOADED 2
typedef struct _LDR_DLL_NOTIFICATION_DATA {
  ULONG Flags;
  const UNICODE_STRING *FullDllName;
  const UNICODE_STRING *BaseDllName;
  PVOID DllBase;
  ULONG SizeOfImage;
} LDR_DLL_NOTIFICATION_DATA, *PLDR_DLL_NOTIFICATION_DATA;
typedef const LDR_DLL_NOTIFICATION_DATA *PCLDR_DLL_NOTIFICATION_DATA;
#endif

typedef VOID(CALLBACK *PLDR_DLL_NOTIFICATION_FUNCTION)(
    ULONG NotificationReason, PCLDR_DLL_NOTIFICATION_DATA NotificationData, PVOID Context);

typedef NTSTATUS(NTAPI *PFN_LdrRegisterDllNotification)(
    ULONG Flags, PLDR_DLL_NOTIFICATION_FUNCTION NotificationFunction, PVOID Context, PVOID *Cookie);

BOOL WINAPI HookedMiniDumpWriteDump(HANDLE hProcess, DWORD ProcessId, HANDLE hFile, MINIDUMP_TYPE DumpType, PMINIDUMP_EXCEPTION_INFORMATION ExceptionParam, PMINIDUMP_USER_STREAM_INFORMATION UserStreamParam, PMINIDUMP_CALLBACK_INFORMATION CallbackParam);

VOID WINAPI HookedRaiseFailFastException(PEXCEPTION_RECORD ExceptionRecord, PCONTEXT ContextRecord, DWORD Flags);

VOID WINAPI HookedRaiseException(DWORD ExceptionCode, DWORD ExceptionFlags, DWORD NumberOfArguments, const ULONG_PTR* Arguments);

VOID NTAPI HookedRtlRaiseException(PEXCEPTION_RECORD ExceptionRecord);

VOID NTAPI HookedRtlRaiseStatus(NTSTATUS Status);

NTSTATUS NTAPI HookedNtRaiseException(PEXCEPTION_RECORD ExceptionRecord, PCONTEXT ContextRecord, BOOLEAN FirstChance);

BOOL WINAPI HookedTerminateProcess(HANDLE hProcess, UINT uExitCode);

VOID WINAPI HookedExitProcess(UINT uExitCode);

VOID NTAPI HookedRtlExitUserProcess(NTSTATUS ExitStatus);

NTSTATUS NTAPI HookedNtTerminateProcess(HANDLE ProcessHandle, NTSTATUS ExitStatus);

void __cdecl HookedInvalidParameterNoInfoNoReturn();

void __cdecl HookedInvokeWatson(const wchar_t* expression, const wchar_t* functionName, const wchar_t* fileName, unsigned int line, uintptr_t reserved);

void __cdecl HookedAbort();

void __cdecl HookedTerminate();

int __cdecl HookedPurecall();

void* ResolveModuleExport(const char* moduleName, const char* functionName);

bool IsUcrtDynamicHookModule(const char* moduleBaseName, HMODULE module);

bool IsApplicationFatalIatModule(HMODULE, const wchar_t* modulePath);

bool IsCurrentProcessHandle(HANDLE processHandle);

bool IsFrameGenerationRuntimeActiveForTerminationDump();

void DescribeAddressModule(void* address, char* buffer, size_t bufferSize);

void LogFatalExitCallerStack(const char* source, DWORD exitCode, void* callerAddress);

std::string QuoteCommandLineArgument(const std::string& value);

std::filesystem::path GetInstalledCaptureEnginePath();

ExternalPreTerminationDumpResult TryCapturePreTerminationDumpWithExternalHelper(const char* source, const char* dumpHint);

// Publishes the external-helper capture and foreign-overlay presence to the
// shared crash handler so its dump worker never has to run an in-process dbghelp
// module walk through a foreign overlay's loader/version hooks.
void RegisterCrashDumpEnvironmentHooksForHook();

bool CapturePreTerminationDumpIfNeeded(const char* source, DWORD exitCode, bool targetIsCurrentProcess, PEXCEPTION_RECORD exceptionRecord, PCONTEXT contextRecord, void* callerAddress = nullptr);

bool ShouldCaptureExplicitFatalRaise(DWORD code);

bool CaptureExplicitFatalRaiseIfNeeded(const char* source, DWORD code, PEXCEPTION_RECORD exceptionRecord, PCONTEXT contextRecord, void* callerAddress = nullptr);

VOID WINAPI HookedRaiseFailFastException(PEXCEPTION_RECORD ExceptionRecord, PCONTEXT ContextRecord, DWORD Flags);

VOID WINAPI HookedRaiseException(DWORD ExceptionCode, DWORD ExceptionFlags, DWORD NumberOfArguments, const ULONG_PTR* Arguments);

VOID NTAPI HookedRtlRaiseException(PEXCEPTION_RECORD ExceptionRecord);

VOID NTAPI HookedRtlRaiseStatus(NTSTATUS Status);

NTSTATUS NTAPI HookedNtRaiseException(PEXCEPTION_RECORD ExceptionRecord, PCONTEXT ContextRecord, BOOLEAN FirstChance);

BOOL WINAPI HookedTerminateProcess(HANDLE hProcess, UINT uExitCode);

VOID WINAPI HookedExitProcess(UINT uExitCode);

VOID NTAPI HookedRtlExitUserProcess(NTSTATUS ExitStatus);

NTSTATUS NTAPI HookedNtTerminateProcess(HANDLE ProcessHandle, NTSTATUS ExitStatus);

void __cdecl HookedInvalidParameterNoInfoNoReturn();

void __cdecl HookedInvokeWatson(const wchar_t* expression, const wchar_t* functionName, const wchar_t* fileName, unsigned int line, uintptr_t reserved);

void __cdecl HookedAbort();

void __cdecl HookedTerminate();

int __cdecl HookedPurecall();

void TryInstallFatalTerminationDumpHooks();

ce::crash_dump_policy::ExternalDumpSignature BuildExternalDumpSignature( const char* sourcePath, DWORD processId, PMINIDUMP_EXCEPTION_INFORMATION exceptionParam);

ExternalDumpGateDecision BeginExternalDumpCaptureForSignature( const ce::crash_dump_policy::ExternalDumpSignature& signature);

void MarkExternalSupplementalDumpCaptured(const std::string& key);

void TerminateProcessAfterExternalDumpStorm(const ExternalDumpGateDecision& decision);

std::string BuildExternalDumpMirrorPath(const char* sourcePath);

void MirrorExternalDumpArtifactIfNeeded(const char* sourcePath, HANDLE hProcess, DWORD processId, MINIDUMP_TYPE dumpType, PMINIDUMP_EXCEPTION_INFORMATION exceptionParam, PMINIDUMP_USER_STREAM_INFORMATION userStreamParam, PMINIDUMP_CALLBACK_INFORMATION callbackParam);

void TryInstallMiniDumpWriteDumpHookForModule(HMODULE module, const char* moduleNameOrPath);

BOOL WINAPI HookedMiniDumpWriteDump(HANDLE hProcess, DWORD ProcessId, HANDLE hFile, MINIDUMP_TYPE DumpType, PMINIDUMP_EXCEPTION_INFORMATION ExceptionParam, PMINIDUMP_USER_STREAM_INFORMATION UserStreamParam, PMINIDUMP_CALLBACK_INFORMATION CallbackParam);

bool IsProcessTerminating();

bool IsDXVKD3D11WrapperLoaded();

void EnsureLocalConfigAllocated();

void InjectIntoChild(HANDLE hProcess, HANDLE hThread);

bool ShouldInjectChild(const char *exePath);

BOOL WINAPI HookedCreateProcessA(LPCSTR lpApp, LPSTR lpCmd, LPSECURITY_ATTRIBUTES lpPA, LPSECURITY_ATTRIBUTES lpTA, BOOL bInherit, DWORD dwFlags, LPVOID lpEnv, LPCSTR lpDir, LPSTARTUPINFOA lpSI, LPPROCESS_INFORMATION lpPI);

BOOL WINAPI HookedCreateProcessW(LPCWSTR lpApp, LPWSTR lpCmd, LPSECURITY_ATTRIBUTES lpPA, LPSECURITY_ATTRIBUTES lpTA, BOOL bInherit, DWORD dwFlags, LPVOID lpEnv, LPCWSTR lpDir, LPSTARTUPINFOW lpSI, LPPROCESS_INFORMATION lpPI);

void CloseCheckHooksEvent();

void CheckAndInstallHooks();

std::string GetRedirectedPath(const std::string &requestedPath);

// Records which physical image provides a Streamline plugin. Called from the
// loader notification for every runtime-family load with its resolved full path.
// Once an image providing the Streamline core (sl.common, including the driver's
// hashed NGX model copy) is seen outside the configured override location, CE has
// lost the stack and every sl.* redirect is refused so the runtime keeps one
// coherent plugin set.
void NoteRuntimeModuleLoadedForOverridePolicy(const char *resolvedPath);

// Same question for modules that were already mapped when CE injected, which the
// loader notification cannot have observed.
void ScanLoadedModulesForForeignStreamlineCore();

bool NeedsLoaderRedirectionHook();

bool NeedsLowLevelModuleLoadObservationHook();

// Loads the configured Streamline/NGX runtime override DLLs (dlss_sr_dll_path,
// dlss_fg_dll_path, dlss_rr_dll_path, streamline_dll_path) into the process up
// front, before the game's own runtime loads, so later name-based loads resolve
// to the override copies instead of the game's older ones. Loads through the
// original loader entry and notifies CE's module hooks immediately. No-op when
// no override paths are configured.
void PreloadConfiguredGraphicsRuntimeDlls();
void PreloadConfiguredStreamlineBridgeNgxDlls();

// Loads the user-configured third-party tool DLLs (ThirdParty.reshade_dll_path,
// ThirdParty.optiscaler_dll_path, ThirdParty.specialk_dll_path) into the
// process early, in the fixed order Special K -> ReShade -> OptiScaler. Skips
// tools already loaded (by canonical base name or as a renamed graphics
// proxy), logs every outcome, and never fails CE's own hook initialization.
// No-op when no paths are configured.
void PreloadConfiguredThirdPartyDlls();

// Loads a runtime override DLL through the original (non-hooked) loader entry
// and notifies CE's module hooks. Used by the runtime preload so it does not
// re-enter the redirect hook.
HMODULE LoadRuntimeDllViaOriginal(const wchar_t *fullPath, const char *utf8Path);

// Patches the kernel32 LoadLibrary* IAT entries of a module that loaded after
// the initial IAT pass (e.g. sl.common.dll), so Streamline-internal loads also
// reach the redirect and module-load observation. No-op unless redirection
// overrides are configured. Never patches CE's own modules or third-party
// overlay modules.
void PatchLoadLibraryIatForLateLoadedModule(HMODULE module, const char* moduleNameOrPath);

void InitializeThirdPartyOverlayDetection();

void RefreshThirdPartyOverlayIdentityCache();

void InitializeHookLifecycleControl();

void MarkHookLifecycleBootstrapComplete();

// Transitions every CE entry point to pass-through, acknowledges the host, and
// blocks until a compatible host is published. The DLL deliberately remains
// resident because games and foreign hook chains can retain CE code pointers.
bool DeactivateHookRuntimeAndWaitForHost(const char* reason, bool previousHostDied, bool launcherOnly = false);

// Services host shutdown/reactivation for launcher-mode injections without
// enabling graphics hooks or publishing the launcher as a capture source.
void RunLauncherHookLifecycle();

void NotifyHookModuleLoaded(HMODULE module, const char *moduleNameOrPath);

void ArmManualReflexQueryHookIfConfigured(const char *source);

void ArmNgxFgPresetOverrideIfConfigured(const char *source);

HMODULE WINAPI HookedLoadLibraryA(LPCSTR lpLibFileName);

HMODULE WINAPI HookedLoadLibraryW(LPCWSTR lpLibFileName);

HMODULE WINAPI HookedLoadLibraryExA(LPCSTR lpLibFileName, HANDLE hFile, DWORD dwFlags);

HMODULE WINAPI HookedLoadLibraryExW(LPCWSTR lpLibFileName, HANDLE hFile, DWORD dwFlags);

NTSTATUS NTAPI HookedLdrLoadDll(PWSTR SearchPath, PULONG DllCharacteristics, PUNICODE_STRING DllName, PVOID *BaseAddress);

namespace UE5 {
void NotifyModuleLoaded(HMODULE module);
void NotifyModuleUnloaded(void* moduleBase, std::size_t moduleSize);
void RefreshOverrides(const GraphicsConfig& config);
void ShutdownOverrides();
}

void CheckAndInstallHooks();

DWORD WINAPI HookThread(LPVOID lpParam);

bool isProcessWhitelistedFast(const char *name);

bool IsServiceProcess(const char *name);

extern "C" BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD ul_reason_for_call, LPVOID lpReserved);

inline ProcessCategory main_g_ProcessCategory = ProcessCategory::PotentialGame;

inline bool main_g_isDormant = false;

inline bool main_g_isSkippedProcess = false;

extern HMODULE g_hModule;

extern std::atomic<bool> g_ProcessTerminating;

extern std::atomic<MiniDumpWriteDump_t> g_OriginalMiniDumpWriteDump;

extern std::atomic<bool> g_MiniDumpWriteDumpHookInstalled;

extern std::once_flag g_MiniDumpWriteDumpHookOnce;

extern std::atomic<RaiseFailFastException_t> g_OriginalRaiseFailFastException;

extern std::atomic<TerminateProcess_t> g_OriginalTerminateProcess;

extern std::atomic<ExitProcess_t> g_OriginalExitProcess;

extern std::atomic<RtlExitUserProcess_t> g_OriginalRtlExitUserProcess;

extern std::atomic<NtTerminateProcess_t> g_OriginalNtTerminateProcess;

extern std::atomic<InvalidParameterNoInfoNoReturn_t> g_OriginalInvalidParameterNoInfoNoReturn;

extern std::atomic<InvokeWatson_t> g_OriginalInvokeWatson;

extern std::atomic<Abort_t> g_OriginalAbort;

extern std::atomic<Terminate_t> g_OriginalTerminate;

extern std::atomic<Purecall_t> g_OriginalPurecall;

extern std::once_flag g_FatalTerminationDumpHookOnce;

extern thread_local bool t_InMiniDumpWriteDumpHook;

extern std::atomic<bool> g_HookThreadRunning;

// Global Hook Pointers
extern DX12Hook *g_DX12Hook;

extern DX11Hook *g_DX11Hook;

extern DX9Hook *g_DX9Hook;

extern DDrawHook *g_DDrawHook;

extern DX8Hook *g_DX8Hook;

extern OpenGLHook *g_OpenGLHook;

// Global Local Config. Constructed once by EnsureLocalConfigAllocated and never
// destroyed, so the pointer stays valid for every hook entry point that can
// still run while the process tears down.
extern AppConfig *g_pLocalConfig;

extern std::atomic<LoadLibraryA_t> OriginalLoadLibraryA;

extern std::atomic<LoadLibraryW_t> OriginalLoadLibraryW;

extern std::atomic<LoadLibraryExA_t> OriginalLoadLibraryExA;

extern std::atomic<LoadLibraryExW_t> OriginalLoadLibraryExW;

extern std::atomic<LdrLoadDll_t> OriginalLdrLoadDll;

extern std::atomic<CreateProcessA_t> OriginalCreateProcessA;

extern std::atomic<CreateProcessW_t> OriginalCreateProcessW;

extern std::mutex g_HookMutex;

extern HANDLE g_hCheckHooksEvent;

extern "C" {
NTSYSAPI VOID NTAPI RtlRaiseException(PEXCEPTION_RECORD ExceptionRecord);
NTSYSAPI VOID NTAPI RtlRaiseStatus(NTSTATUS Status);
NTSYSAPI VOID NTAPI RtlExitUserProcess(NTSTATUS ExitStatus);
NTSYSAPI NTSTATUS NTAPI NtRaiseException(PEXCEPTION_RECORD ExceptionRecord, PCONTEXT ContextRecord,
                                         BOOLEAN FirstChance);
NTSYSAPI NTSTATUS NTAPI NtTerminateProcess(HANDLE ProcessHandle, NTSTATUS ExitStatus);
}

template <typename Function>
void PublishFatalHookTrampoline(void* trampoline, void* context) {
  auto* originalSlot = static_cast<std::atomic<Function>*>(context);
  originalSlot->store(reinterpret_cast<Function>(trampoline), std::memory_order_release);
}

struct ExternalDumpStormRecord {
  ULONGLONG firstHitMs = 0;
  ULONGLONG lastHitMs = 0;
  uint32_t hitCount = 0;
  bool strongSignature = false;
  bool mirrorAttempted = false;
  bool supplementalAttempted = false;
  bool supplementalCaptured = false;
  bool terminationRequested = false;
};

struct ExternalDumpGateDecision {
  std::string key;
  uint32_t hitCount = 0;
  bool strongSignature = false;
  bool mirrorAllowed = false;
  bool supplementalAllowed = false;
  bool terminateProcess = false;
};

// Helper to safely delete hooks
template <typename T> void SafeShutdownHook(T *&hook, const char *name) {
  if (hook) {
    HookLog("DLL_DETACH: Shutting down %s...", name);
    hook->Shutdown();
    HookLog("DLL_DETACH: Deleting %s...", name);
    delete hook;
    hook = nullptr;
    HookLog("DLL_DETACH: %s shutdown complete", name);
  }
}

template <typename T>
T ResolveOriginalProc(std::atomic<T> &slot, const char *moduleName,
                      const char *procName) {
  T original = slot.load(std::memory_order_acquire);
  if (original) {
    return original;
  }

  HMODULE module = GetModuleHandleA(moduleName);
  if (!module) {
    return nullptr;
  }

  T resolved = reinterpret_cast<T>(GetProcAddress(module, procName));
  if (!resolved) {
    return nullptr;
  }

  T expected = nullptr;
  if (slot.compare_exchange_strong(expected, resolved,
                                   std::memory_order_release,
                                   std::memory_order_acquire)) {
    return resolved;
  }

  return slot.load(std::memory_order_acquire);
}

inline CreateProcessA_t GetOriginalCreateProcessA() {
  return ResolveOriginalProc(OriginalCreateProcessA, "kernel32.dll",
                             "CreateProcessA");
}

inline CreateProcessW_t GetOriginalCreateProcessW() {
  return ResolveOriginalProc(OriginalCreateProcessW, "kernel32.dll",
                             "CreateProcessW");
}

// Helper: Inject our DLL into a suspended child process.
// Runs on a dedicated worker thread so the calling thread (possibly render
// thread) is not blocked by the 5-second WaitForSingleObject.
struct ChildInjectParams {
  HANDLE hProcess;
  HANDLE hThread;
  char dllPath[MAX_PATH];
};

// Helper for QueueUserWorkItem (requires DWORD return, LPVOID param)
inline DWORD WINAPI HookThreadWrapper(LPVOID lpParam) {
  timeBeginPeriod(1);
  return HookThread(lpParam);
}
