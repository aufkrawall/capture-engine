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
#include "common/hook_common.h"
#include "common/hook_context.h"
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
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

extern "C" {
NTSYSAPI VOID NTAPI RtlRaiseException(PEXCEPTION_RECORD ExceptionRecord);
NTSYSAPI VOID NTAPI RtlRaiseStatus(NTSTATUS Status);
NTSYSAPI VOID NTAPI RtlExitUserProcess(NTSTATUS ExitStatus);
NTSYSAPI NTSTATUS NTAPI NtRaiseException(PEXCEPTION_RECORD ExceptionRecord, PCONTEXT ContextRecord,
                                         BOOLEAN FirstChance);
NTSYSAPI NTSTATUS NTAPI NtTerminateProcess(HANDLE ProcessHandle, NTSTATUS ExitStatus);
}

HMODULE g_hModule = NULL;
// Note: g_ShuttingDown is declared in hook/common/hook_common.h

static std::atomic<bool> g_ProcessTerminating{false};

namespace {

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

BOOL WINAPI HookedMiniDumpWriteDump(HANDLE hProcess, DWORD ProcessId, HANDLE hFile, MINIDUMP_TYPE DumpType,
                                    PMINIDUMP_EXCEPTION_INFORMATION ExceptionParam,
                                    PMINIDUMP_USER_STREAM_INFORMATION UserStreamParam,
                                    PMINIDUMP_CALLBACK_INFORMATION CallbackParam);
VOID WINAPI HookedRaiseFailFastException(PEXCEPTION_RECORD ExceptionRecord, PCONTEXT ContextRecord, DWORD Flags);
VOID WINAPI HookedRaiseException(DWORD ExceptionCode, DWORD ExceptionFlags, DWORD NumberOfArguments,
                                 const ULONG_PTR* Arguments);
VOID NTAPI HookedRtlRaiseException(PEXCEPTION_RECORD ExceptionRecord);
VOID NTAPI HookedRtlRaiseStatus(NTSTATUS Status);
NTSTATUS NTAPI HookedNtRaiseException(PEXCEPTION_RECORD ExceptionRecord, PCONTEXT ContextRecord, BOOLEAN FirstChance);
BOOL WINAPI HookedTerminateProcess(HANDLE hProcess, UINT uExitCode);
VOID WINAPI HookedExitProcess(UINT uExitCode);
VOID NTAPI HookedRtlExitUserProcess(NTSTATUS ExitStatus);
NTSTATUS NTAPI HookedNtTerminateProcess(HANDLE ProcessHandle, NTSTATUS ExitStatus);
void __cdecl HookedInvalidParameterNoInfoNoReturn();
void __cdecl HookedInvokeWatson(const wchar_t* expression, const wchar_t* functionName, const wchar_t* fileName,
                                unsigned int line, uintptr_t reserved);
void __cdecl HookedAbort();
void __cdecl HookedTerminate();
int __cdecl HookedPurecall();

std::atomic<MiniDumpWriteDump_t> g_OriginalMiniDumpWriteDump{nullptr};
std::atomic<bool> g_MiniDumpWriteDumpHookInstalled{false};
std::once_flag g_MiniDumpWriteDumpHookOnce;

std::atomic<RaiseFailFastException_t> g_OriginalRaiseFailFastException{nullptr};
std::atomic<TerminateProcess_t> g_OriginalTerminateProcess{nullptr};
std::atomic<ExitProcess_t> g_OriginalExitProcess{nullptr};
std::atomic<RtlExitUserProcess_t> g_OriginalRtlExitUserProcess{nullptr};
std::atomic<NtTerminateProcess_t> g_OriginalNtTerminateProcess{nullptr};
std::atomic<InvalidParameterNoInfoNoReturn_t> g_OriginalInvalidParameterNoInfoNoReturn{nullptr};
std::atomic<InvokeWatson_t> g_OriginalInvokeWatson{nullptr};
std::atomic<Abort_t> g_OriginalAbort{nullptr};
std::atomic<Terminate_t> g_OriginalTerminate{nullptr};
std::atomic<Purecall_t> g_OriginalPurecall{nullptr};
std::atomic<bool> g_PreTerminationDumpAttempted{false};
std::once_flag g_FatalTerminationDumpHookOnce;

thread_local bool t_InMiniDumpWriteDumpHook = false;
thread_local bool t_InFatalTerminationDumpHook = false;

void* ResolveModuleExport(const char* moduleName, const char* functionName) {
  HMODULE module = GetModuleHandleA(moduleName);
  if (!module) {
    module = LoadLibraryA(moduleName);
  }
  return module ? reinterpret_cast<void*>(GetProcAddress(module, functionName)) : nullptr;
}

bool IsUcrtDynamicHookModule(const char* moduleBaseName, HMODULE module) {
  auto isUcrtBaseName = [](const char* name) {
    return name && _stricmp(name, "ucrtbase.dll") == 0;
  };

  if (isUcrtBaseName(moduleBaseName)) {
    return true;
  }

  char modulePath[MAX_PATH] = {};
  if (!module || !GetModuleFileNameA(module, modulePath, sizeof(modulePath))) {
    return false;
  }

  const char* baseName = modulePath;
  for (const char* cursor = modulePath; *cursor; ++cursor) {
    if (*cursor == '\\' || *cursor == '/') {
      baseName = cursor + 1;
    }
  }
  return isUcrtBaseName(baseName);
}

bool IsApplicationFatalIatModule(HMODULE, const wchar_t* modulePath) {
  // VEH and the narrow inline termination hooks cover Windows' internal
  // exception/exit paths. Rewriting imports inside the Windows directory creates
  // process-wide cycles between forwarded APIs and is unnecessary for
  // observing application-requested fatal exits.
  wchar_t windowsDirectory[MAX_PATH] = {};
  if (GetWindowsDirectoryW(windowsDirectory, MAX_PATH) == 0) {
    return false;
  }
  return !IATHook::IsPathUnderDirectoryRoot(modulePath, windowsDirectory);
}

bool IsCurrentProcessHandle(HANDLE processHandle) {
  if (!processHandle) {
    return false;
  }
  if (processHandle == GetCurrentProcess()) {
    return true;
  }

  const DWORD targetPid = GetProcessId(processHandle);
  return targetPid != 0 && targetPid == GetCurrentProcessId();
}

bool IsFrameGenerationRuntimeActiveForTerminationDump() {
  const auto runtimeMode = g_FGCompat.GetRuntimeMode();
  return g_FGCompat.IsFGActive() || g_FGCompat.IsDLSSFGApiActive() || g_FGCompat.IsFSRFGApiActive() ||
         g_FGCompat.IsStreamlineFGSignaled() || DXGIShared::g_StreamlineFGRunning.load(std::memory_order_acquire) ||
         ce::fg_runtime::IsRuntimeFGActive(runtimeMode) || DX12_IsRuntimeOwnedSwapchainActiveForFrameGeneration();
}

void DescribeAddressModule(void* address, char* buffer, size_t bufferSize) {
  if (!buffer || bufferSize == 0) {
    return;
  }
  buffer[0] = '\0';
  if (!address) {
    snprintf(buffer, bufferSize, "unknown");
    return;
  }

  HMODULE module = nullptr;
  if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                         reinterpret_cast<LPCSTR>(address), &module) &&
      module) {
    char modulePath[MAX_PATH] = {};
    if (GetModuleFileNameA(module, modulePath, sizeof(modulePath))) {
      snprintf(buffer, bufferSize, "%s+0x%llX", modulePath,
               static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(address) -
                                               reinterpret_cast<uintptr_t>(module)));
      return;
    }
  }

  snprintf(buffer, bufferSize, "unknown");
}

void LogFatalExitCallerStack(const char* source, DWORD exitCode, void* callerAddress) {
  char callerModule[MAX_PATH + 64] = {};
  DescribeAddressModule(callerAddress, callerModule, sizeof(callerModule));

  if (source && std::strcmp(source, "_purecall") == 0) {
    HookLogImportant("FatalExitDump: _purecall caller stack before pre-termination dump");
  }

  void* frames[16] = {};
  const USHORT frameCount =
      RtlCaptureStackBackTrace(0, static_cast<DWORD>(sizeof(frames) / sizeof(frames[0])), frames, nullptr);
  HookLogImportant(
      "FatalExitDump: %s caller stack before pre-termination dump "
      "(code=0x%08lX caller=%p module=%s frames=%u)",
      source ? source : "unknown", static_cast<unsigned long>(exitCode), callerAddress, callerModule,
      static_cast<unsigned>(frameCount));

  const USHORT framesToLog = std::min<USHORT>(frameCount, 12);
  for (USHORT i = 0; i < framesToLog; ++i) {
    char frameModule[MAX_PATH + 64] = {};
    DescribeAddressModule(frames[i], frameModule, sizeof(frameModule));
    HookLogImportant("FatalExitDump: %s stack[%u]=%p %s", source ? source : "unknown",
                     static_cast<unsigned>(i), frames[i], frameModule);
  }
}

enum class ExternalPreTerminationDumpResult { kUnavailable, kCaptured, kFailed, kTimedOut };

std::string QuoteCommandLineArgument(const std::string& value) {
  std::string quoted = "\"";
  for (char ch : value) {
    if (ch == '"') {
      quoted.push_back('\\');
    }
    quoted.push_back(ch);
  }
  if (!value.empty() && value.back() == '\\') {
    quoted.push_back('\\');
  }
  quoted.push_back('"');
  return quoted;
}

std::filesystem::path GetInstalledCaptureEnginePath() {
  char modulePath[MAX_PATH] = {};
  if (!GetModuleFileNameA(g_hModule, modulePath, static_cast<DWORD>(sizeof(modulePath)))) {
    return {};
  }

  std::filesystem::path baseDir = std::filesystem::path(modulePath).parent_path();
  return baseDir / "captureengine.exe";
}

ExternalPreTerminationDumpResult TryCapturePreTerminationDumpWithExternalHelper(const char* source,
                                                                                const char* dumpHint) {
  const std::string dumpDir = GetCrashDumpDirectory();
  if (dumpDir.empty() || !dumpHint || dumpHint[0] == '\0') {
    return ExternalPreTerminationDumpResult::kUnavailable;
  }

  const std::filesystem::path helperPath = GetInstalledCaptureEnginePath();
  std::error_code ec;
  if (helperPath.empty() || !std::filesystem::exists(helperPath, ec)) {
    HookLogImportant(
        "FatalExitDump: External pre-termination dump helper unavailable "
        "(source=%s helper=%s dumpDir=%s)",
        source ? source : "unknown", helperPath.string().c_str(), dumpDir.c_str());
    return ExternalPreTerminationDumpResult::kUnavailable;
  }

  std::string commandLine = QuoteCommandLineArgument(helperPath.string());
  commandLine += " --dump-helper --dump-helper-pid=";
  commandLine += std::to_string(GetCurrentProcessId());
  commandLine += " --dump-helper-dir=";
  commandLine += QuoteCommandLineArgument(dumpDir);
  commandLine += " --dump-helper-hint=";
  commandLine += QuoteCommandLineArgument(dumpHint);

  std::vector<char> mutableCommandLine(commandLine.begin(), commandLine.end());
  mutableCommandLine.push_back('\0');

  STARTUPINFOA si = {};
  si.cb = sizeof(si);
  si.dwFlags = STARTF_USESHOWWINDOW | STARTF_FORCEOFFFEEDBACK;
  si.wShowWindow = SW_HIDE;
  PROCESS_INFORMATION pi = {};

  HookLogImportant(
      "FatalExitDump: Launching external pre-termination dump helper "
      "(source=%s helper=%s hint=%s)",
      source ? source : "unknown", helperPath.string().c_str(), dumpHint);

  const std::string workingDir = helperPath.parent_path().string();
  if (!CreateProcessA(helperPath.string().c_str(), mutableCommandLine.data(), nullptr, nullptr, FALSE,
                      CREATE_NO_WINDOW, nullptr, workingDir.empty() ? nullptr : workingDir.c_str(), &si, &pi)) {
    HookLogImportant("FatalExitDump: External pre-termination dump helper launch failed "
                     "(source=%s error=%lu helper=%s)",
                     source ? source : "unknown", GetLastError(), helperPath.string().c_str());
    return ExternalPreTerminationDumpResult::kFailed;
  }

  constexpr DWORD kExternalDumpHelperWaitMs = 8000;
  const DWORD waitResult = WaitForSingleObject(pi.hProcess, kExternalDumpHelperWaitMs);
  if (waitResult == WAIT_TIMEOUT) {
    HookLogImportant(
        "FatalExitDump: External pre-termination dump helper still running after timeout "
        "(source=%s timeoutMs=%lu hint=%s) — skipping in-process fallback to avoid dump-path hang",
        source ? source : "unknown", static_cast<unsigned long>(kExternalDumpHelperWaitMs), dumpHint);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return ExternalPreTerminationDumpResult::kTimedOut;
  }

  DWORD helperExitCode = 0xFFFFFFFFu;
  GetExitCodeProcess(pi.hProcess, &helperExitCode);
  CloseHandle(pi.hThread);
  CloseHandle(pi.hProcess);

  if (waitResult == WAIT_OBJECT_0 && helperExitCode == 0) {
    HookLogImportant("FatalExitDump: External pre-termination dump helper captured dump "
                     "(source=%s hint=%s)",
                     source ? source : "unknown", dumpHint);
    return ExternalPreTerminationDumpResult::kCaptured;
  }

  HookLogImportant("FatalExitDump: External pre-termination dump helper failed "
                   "(source=%s wait=%lu exit=%lu hint=%s)",
                   source ? source : "unknown", waitResult, helperExitCode, dumpHint);
  return ExternalPreTerminationDumpResult::kFailed;
}

bool CapturePreTerminationDumpIfNeeded(const char* source, DWORD exitCode, bool targetIsCurrentProcess,
                                       PEXCEPTION_RECORD exceptionRecord, PCONTEXT contextRecord,
                                       void* callerAddress = nullptr) {
  const bool alreadyAttempted = g_PreTerminationDumpAttempted.load(std::memory_order_acquire);
  const bool frameGenerationRuntimeActiveOrRecent = IsFrameGenerationRuntimeActiveForTerminationDump();
  if (!ce::crash_dump_policy::ShouldCapturePreTerminationDump(targetIsCurrentProcess, exitCode, alreadyAttempted,
                                                              frameGenerationRuntimeActiveOrRecent)) {
    return false;
  }

  if (t_InFatalTerminationDumpHook) {
    return false;
  }

  bool expected = false;
  if (!g_PreTerminationDumpAttempted.compare_exchange_strong(expected, true, std::memory_order_acq_rel,
                                                             std::memory_order_acquire)) {
    return false;
  }

  t_InFatalTerminationDumpHook = true;

  if (!callerAddress) {
    callerAddress = __builtin_return_address(0);
  }
  LogFatalExitCallerStack(source, exitCode, callerAddress);

  CONTEXT capturedContext = {};
  if (!contextRecord) {
    RtlCaptureContext(&capturedContext);
    contextRecord = &capturedContext;
  }

  EXCEPTION_RECORD synthesizedRecord = {};
  if (!exceptionRecord) {
    synthesizedRecord.ExceptionCode = exitCode;
    synthesizedRecord.ExceptionAddress = callerAddress;
    exceptionRecord = &synthesizedRecord;
  }

  EXCEPTION_POINTERS pointers = {};
  pointers.ExceptionRecord = exceptionRecord;
  pointers.ContextRecord = contextRecord;

  MINIDUMP_EXCEPTION_INFORMATION exceptionInfo = {};
  exceptionInfo.ThreadId = GetCurrentThreadId();
  exceptionInfo.ExceptionPointers = &pointers;
  exceptionInfo.ClientPointers = FALSE;

  char dumpHint[160] = {};
  snprintf(dumpHint, sizeof(dumpHint), "fatal_exit_%s_%08lx.dmp", source ? source : "unknown",
           static_cast<unsigned long>(exitCode));

  HookLogImportant(
      "FatalExitDump: Capturing pre-termination dump before crash-like process exit or active FG runtime exit "
      "(source=%s code=0x%08lX exceptionAddr=%p crashLike=%d fgRuntimeActiveOrRecent=%d)",
      source ? source : "unknown", static_cast<unsigned long>(exitCode), exceptionRecord->ExceptionAddress,
      ce::crash_dump_policy::IsCrashLikeProcessExitCode(exitCode) ? 1 : 0,
      frameGenerationRuntimeActiveOrRecent ? 1 : 0);
  OutputDebugStringA("[FatalExitDump] Capturing pre-termination crash dump.\n");

  HookLogImportant("FatalExitDump: Using minimal-first pre-termination dump attempt (source=%s hint=%s)",
                   source ? source : "unknown", dumpHint);
  const ExternalPreTerminationDumpResult externalDumpResult =
      TryCapturePreTerminationDumpWithExternalHelper(source, dumpHint);
  bool wroteDump = externalDumpResult == ExternalPreTerminationDumpResult::kCaptured;
  if (!wroteDump && externalDumpResult != ExternalPreTerminationDumpResult::kTimedOut) {
    HookLogImportant("FatalExitDump: Falling back to in-process pre-termination dump attempt "
                     "(source=%s hint=%s externalResult=%d)",
                     source ? source : "unknown", dumpHint, static_cast<int>(externalDumpResult));
    wroteDump = WriteSupplementalCrashDump(dumpHint, GetCurrentProcess(), GetCurrentProcessId(),
                                           ce::crash_dump_policy::kMinimalDumpType, &exceptionInfo);
  }
  HookLogImportant("FatalExitDump: Pre-termination dump %s (source=%s code=0x%08lX hint=%s)",
                   wroteDump ? "captured" : "failed", source ? source : "unknown",
                   static_cast<unsigned long>(exitCode), dumpHint);

  t_InFatalTerminationDumpHook = false;
  return wroteDump;
}

bool ShouldCaptureExplicitFatalRaise(DWORD code) {
  return code == ce::crash_dump_policy::kFailFastExceptionExitCode || code == EXCEPTION_STACK_OVERFLOW ||
         code == EXCEPTION_ILLEGAL_INSTRUCTION;
}

bool CaptureExplicitFatalRaiseIfNeeded(const char* source, DWORD code, PEXCEPTION_RECORD exceptionRecord,
                                       PCONTEXT contextRecord, void* callerAddress = nullptr) {
  if (!ShouldCaptureExplicitFatalRaise(code)) {
    return false;
  }

  if (!callerAddress) {
    callerAddress = __builtin_return_address(0);
  }

  EXCEPTION_RECORD synthesizedRecord = {};
  if (!exceptionRecord) {
    synthesizedRecord.ExceptionCode = code;
    synthesizedRecord.ExceptionAddress = callerAddress;
    exceptionRecord = &synthesizedRecord;
  }

  return CapturePreTerminationDumpIfNeeded(source, code, true, exceptionRecord, contextRecord, callerAddress);
}

VOID WINAPI HookedRaiseFailFastException(PEXCEPTION_RECORD ExceptionRecord, PCONTEXT ContextRecord, DWORD Flags) {
  const DWORD code = ExceptionRecord ? ExceptionRecord->ExceptionCode : ce::crash_dump_policy::kFailFastExceptionExitCode;
  CapturePreTerminationDumpIfNeeded("RaiseFailFastException", code, true, ExceptionRecord, ContextRecord,
                                    __builtin_return_address(0));

  const auto original = g_OriginalRaiseFailFastException.load(std::memory_order_acquire);
  if (original) {
    original(ExceptionRecord, ContextRecord, Flags);
    return;
  }

  RaiseFailFastException(ExceptionRecord, ContextRecord, Flags);
}

VOID WINAPI HookedRaiseException(DWORD ExceptionCode, DWORD ExceptionFlags, DWORD NumberOfArguments,
                                 const ULONG_PTR* Arguments) {
  EXCEPTION_RECORD record = {};
  record.ExceptionCode = ExceptionCode;
  record.ExceptionFlags = ExceptionFlags;
  record.ExceptionAddress = __builtin_return_address(0);
  record.NumberParameters = std::min<DWORD>(NumberOfArguments, EXCEPTION_MAXIMUM_PARAMETERS);
  for (DWORD i = 0; i < record.NumberParameters && Arguments; ++i) {
    record.ExceptionInformation[i] = Arguments[i];
  }
  CaptureExplicitFatalRaiseIfNeeded("RaiseException", ExceptionCode, &record, nullptr, __builtin_return_address(0));

  ::RaiseException(ExceptionCode, ExceptionFlags, NumberOfArguments, Arguments);
}

VOID NTAPI HookedRtlRaiseException(PEXCEPTION_RECORD ExceptionRecord) {
  const DWORD code = ExceptionRecord ? ExceptionRecord->ExceptionCode : 0;
  CaptureExplicitFatalRaiseIfNeeded("RtlRaiseException", code, ExceptionRecord, nullptr, __builtin_return_address(0));

  ::RtlRaiseException(ExceptionRecord);
}

VOID NTAPI HookedRtlRaiseStatus(NTSTATUS Status) {
  CaptureExplicitFatalRaiseIfNeeded("RtlRaiseStatus", static_cast<DWORD>(Status), nullptr, nullptr,
                                    __builtin_return_address(0));

  ::RtlRaiseStatus(Status);
}

NTSTATUS NTAPI HookedNtRaiseException(PEXCEPTION_RECORD ExceptionRecord, PCONTEXT ContextRecord, BOOLEAN FirstChance) {
  const DWORD code = ExceptionRecord ? ExceptionRecord->ExceptionCode : 0;
  CaptureExplicitFatalRaiseIfNeeded("NtRaiseException", code, ExceptionRecord, ContextRecord,
                                    __builtin_return_address(0));

  return ::NtRaiseException(ExceptionRecord, ContextRecord, FirstChance);
}

BOOL WINAPI HookedTerminateProcess(HANDLE hProcess, UINT uExitCode) {
  CapturePreTerminationDumpIfNeeded("TerminateProcess", static_cast<DWORD>(uExitCode), IsCurrentProcessHandle(hProcess),
                                    nullptr, nullptr, __builtin_return_address(0));

  const auto original = g_OriginalTerminateProcess.load(std::memory_order_acquire);
  if (original) {
    return original(hProcess, uExitCode);
  }

  return TerminateProcess(hProcess, uExitCode);
}

VOID WINAPI HookedExitProcess(UINT uExitCode) {
  CapturePreTerminationDumpIfNeeded("ExitProcess", static_cast<DWORD>(uExitCode), true, nullptr, nullptr,
                                    __builtin_return_address(0));

  const auto original = g_OriginalExitProcess.load(std::memory_order_acquire);
  if (original) {
    original(uExitCode);
    return;
  }

  ExitProcess(uExitCode);
}

VOID NTAPI HookedRtlExitUserProcess(NTSTATUS ExitStatus) {
  CapturePreTerminationDumpIfNeeded("RtlExitUserProcess", static_cast<DWORD>(ExitStatus), true, nullptr, nullptr,
                                    __builtin_return_address(0));

  const auto original = g_OriginalRtlExitUserProcess.load(std::memory_order_acquire);
  if (original) {
    original(ExitStatus);
    return;
  }

  // Preserve the full ntdll exit sequence through this DLL's deliberately
  // unpatched static import. Calling ExitProcess here re-enters
  // RtlExitUserProcess through KernelBase and recurses when no dedicated Rtl
  // trampoline was installed (for example when it aliases another exit
  // target).
  ::RtlExitUserProcess(ExitStatus);
}

NTSTATUS NTAPI HookedNtTerminateProcess(HANDLE ProcessHandle, NTSTATUS ExitStatus) {
  const bool targetIsCurrentProcess = ProcessHandle == nullptr || IsCurrentProcessHandle(ProcessHandle);
  CapturePreTerminationDumpIfNeeded("NtTerminateProcess", static_cast<DWORD>(ExitStatus), targetIsCurrentProcess,
                                    nullptr, nullptr, __builtin_return_address(0));

  const auto original = g_OriginalNtTerminateProcess.load(std::memory_order_acquire);
  if (original) {
    return original(ProcessHandle, ExitStatus);
  }

  return ::NtTerminateProcess(ProcessHandle, ExitStatus);
}

void __cdecl HookedInvalidParameterNoInfoNoReturn() {
  CapturePreTerminationDumpIfNeeded("_invalid_parameter_noinfo_noreturn",
                                    ce::crash_dump_policy::kFailFastExceptionExitCode, true, nullptr, nullptr,
                                    __builtin_return_address(0));

  const auto original = g_OriginalInvalidParameterNoInfoNoReturn.load(std::memory_order_acquire);
  if (original) {
    original();
    return;
  }

  RaiseFailFastException(nullptr, nullptr, 0);
}

void __cdecl HookedInvokeWatson(const wchar_t* expression, const wchar_t* functionName, const wchar_t* fileName,
                                unsigned int line, uintptr_t reserved) {
  (void)expression;
  (void)functionName;
  (void)fileName;
  (void)line;
  (void)reserved;
  CapturePreTerminationDumpIfNeeded("_invoke_watson", ce::crash_dump_policy::kFailFastExceptionExitCode, true, nullptr,
                                    nullptr, __builtin_return_address(0));

  const auto original = g_OriginalInvokeWatson.load(std::memory_order_acquire);
  if (original) {
    original(expression, functionName, fileName, line, reserved);
    return;
  }

  RaiseFailFastException(nullptr, nullptr, 0);
}

void __cdecl HookedAbort() {
  CapturePreTerminationDumpIfNeeded("abort", ce::crash_dump_policy::kFailFastExceptionExitCode, true, nullptr, nullptr,
                                    __builtin_return_address(0));

  const auto original = g_OriginalAbort.load(std::memory_order_acquire);
  if (original) {
    original();
    return;
  }

  TerminateProcess(GetCurrentProcess(), ce::crash_dump_policy::kFailFastExceptionExitCode);
}

void __cdecl HookedTerminate() {
  CapturePreTerminationDumpIfNeeded("terminate", ce::crash_dump_policy::kFailFastExceptionExitCode, true, nullptr,
                                    nullptr, __builtin_return_address(0));

  const auto original = g_OriginalTerminate.load(std::memory_order_acquire);
  if (original) {
    original();
    return;
  }

  TerminateProcess(GetCurrentProcess(), ce::crash_dump_policy::kFailFastExceptionExitCode);
}

int __cdecl HookedPurecall() {
  CapturePreTerminationDumpIfNeeded("_purecall", ce::crash_dump_policy::kFailFastExceptionExitCode, true, nullptr,
                                    nullptr, __builtin_return_address(0));

  const auto original = g_OriginalPurecall.load(std::memory_order_acquire);
  if (original) {
    return original();
  }

  TerminateProcess(GetCurrentProcess(), ce::crash_dump_policy::kFailFastExceptionExitCode);
  return 0;
}

template <typename Function>
void PublishFatalHookTrampoline(void* trampoline, void* context) {
  auto* originalSlot = static_cast<std::atomic<Function>*>(context);
  originalSlot->store(reinterpret_cast<Function>(trampoline), std::memory_order_release);
}

void TryInstallFatalTerminationDumpHooks() {
  std::call_once(g_FatalTerminationDumpHookOnce, []() {
    bool patchedAny = false;
    auto patchRaise = [&patchedAny](const char* sourceModule) {
      void* patchedOriginal = nullptr;
      if (IATHook::PatchIATAllModulesFiltered(sourceModule, "RaiseFailFastException",
                                              reinterpret_cast<void*>(&HookedRaiseFailFastException),
                                              &patchedOriginal, &IsApplicationFatalIatModule)) {
        patchedAny = true;
      }
    };

    auto patchTerminate = [&patchedAny](const char* sourceModule) {
      void* patchedOriginal = nullptr;
      if (IATHook::PatchIATAllModulesFiltered(sourceModule, "TerminateProcess",
                                              reinterpret_cast<void*>(&HookedTerminateProcess), &patchedOriginal,
                                              &IsApplicationFatalIatModule)) {
