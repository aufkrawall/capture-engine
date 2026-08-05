#include "main_internal.h"

HMODULE g_hModule = NULL;

static std::atomic<bool> g_ProcessTerminating{false};

namespace {
BOOL WINAPI HookedMiniDumpWriteDump(HANDLE hProcess, DWORD ProcessId, HANDLE hFile, MINIDUMP_TYPE DumpType,
                                    PMINIDUMP_EXCEPTION_INFORMATION ExceptionParam,
                                    PMINIDUMP_USER_STREAM_INFORMATION UserStreamParam,
                                    PMINIDUMP_CALLBACK_INFORMATION CallbackParam);
}

namespace {
VOID WINAPI HookedRaiseFailFastException(PEXCEPTION_RECORD ExceptionRecord, PCONTEXT ContextRecord, DWORD Flags);
}

namespace {
VOID WINAPI HookedRaiseException(DWORD ExceptionCode, DWORD ExceptionFlags, DWORD NumberOfArguments,
                                 const ULONG_PTR* Arguments);
}

namespace {
VOID NTAPI HookedRtlRaiseException(PEXCEPTION_RECORD ExceptionRecord);
}

namespace {
VOID NTAPI HookedRtlRaiseStatus(NTSTATUS Status);
}

namespace {
NTSTATUS NTAPI HookedNtRaiseException(PEXCEPTION_RECORD ExceptionRecord, PCONTEXT ContextRecord, BOOLEAN FirstChance);
}

namespace {
BOOL WINAPI HookedTerminateProcess(HANDLE hProcess, UINT uExitCode);
}

namespace {
VOID WINAPI HookedExitProcess(UINT uExitCode);
}

namespace {
VOID NTAPI HookedRtlExitUserProcess(NTSTATUS ExitStatus);
}

namespace {
NTSTATUS NTAPI HookedNtTerminateProcess(HANDLE ProcessHandle, NTSTATUS ExitStatus);
}

namespace {
void __cdecl HookedInvalidParameterNoInfoNoReturn();
}

namespace {
void __cdecl HookedInvokeWatson(const wchar_t* expression, const wchar_t* functionName, const wchar_t* fileName,
                                unsigned int line, uintptr_t reserved);
}

namespace {
void __cdecl HookedAbort();
}

namespace {
void __cdecl HookedTerminate();
}

namespace {
int __cdecl HookedPurecall();
}

namespace {
std::atomic<MiniDumpWriteDump_t> g_OriginalMiniDumpWriteDump{nullptr};
}

namespace {
std::atomic<bool> g_MiniDumpWriteDumpHookInstalled{false};
}

namespace {
std::once_flag g_MiniDumpWriteDumpHookOnce;
}

namespace {
std::atomic<RaiseFailFastException_t> g_OriginalRaiseFailFastException{nullptr};
}

namespace {
std::atomic<TerminateProcess_t> g_OriginalTerminateProcess{nullptr};
}

namespace {
std::atomic<ExitProcess_t> g_OriginalExitProcess{nullptr};
}

namespace {
std::atomic<RtlExitUserProcess_t> g_OriginalRtlExitUserProcess{nullptr};
}

namespace {
std::atomic<NtTerminateProcess_t> g_OriginalNtTerminateProcess{nullptr};
}

namespace {
std::atomic<InvalidParameterNoInfoNoReturn_t> g_OriginalInvalidParameterNoInfoNoReturn{nullptr};
}

namespace {
std::atomic<InvokeWatson_t> g_OriginalInvokeWatson{nullptr};
}

namespace {
std::atomic<Abort_t> g_OriginalAbort{nullptr};
}

namespace {
std::atomic<Terminate_t> g_OriginalTerminate{nullptr};
}

namespace {
std::atomic<Purecall_t> g_OriginalPurecall{nullptr};
}

namespace {
std::atomic<bool> g_PreTerminationDumpAttempted{false};
}

namespace {
std::once_flag g_FatalTerminationDumpHookOnce;
}

namespace {
thread_local bool t_InMiniDumpWriteDumpHook = false;
}

namespace {
thread_local bool t_InFatalTerminationDumpHook = false;
}

namespace {
void* ResolveModuleExport(const char* moduleName, const char* functionName) {
  HMODULE module = GetModuleHandleA(moduleName);
  if (!module) {
    module = LoadLibraryA(moduleName);
  }
  return module ? reinterpret_cast<void*>(GetProcAddress(module, functionName)) : nullptr;
}
}

namespace {
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
}

namespace {
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
}

namespace {
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
}

namespace {
bool IsFrameGenerationRuntimeActiveForTerminationDump() {
  const auto runtimeMode = g_FGCompat.GetRuntimeMode();
  return g_FGCompat.IsFGActive() || g_FGCompat.IsDLSSFGApiActive() || g_FGCompat.IsFSRFGApiActive() ||
         g_FGCompat.IsStreamlineFGSignaled() || DXGIShared::g_StreamlineFGRunning.load(std::memory_order_acquire) ||
         ce::fg_runtime::IsRuntimeFGActive(runtimeMode) || DX12_IsRuntimeOwnedSwapchainActiveForFrameGeneration();
}
}

namespace {
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
}

namespace {
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
}

namespace {
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
}

namespace {
std::filesystem::path GetInstalledCaptureEnginePath() {
  char modulePath[MAX_PATH] = {};
  if (!GetModuleFileNameA(g_hModule, modulePath, static_cast<DWORD>(sizeof(modulePath)))) {
    return {};
  }

  std::filesystem::path baseDir = std::filesystem::path(modulePath).parent_path();
  return baseDir / "captureengine.exe";
}
}

namespace {
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
}

namespace {
bool CapturePreTerminationDumpIfNeeded(const char* source, DWORD exitCode, bool targetIsCurrentProcess,
                                       PEXCEPTION_RECORD exceptionRecord, PCONTEXT contextRecord,
                                       void* callerAddress ) {
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
}

namespace {
bool ShouldCaptureExplicitFatalRaise(DWORD code) {
  return code == ce::crash_dump_policy::kFailFastExceptionExitCode || code == EXCEPTION_STACK_OVERFLOW ||
         code == EXCEPTION_ILLEGAL_INSTRUCTION;
}
}

namespace {
bool CaptureExplicitFatalRaiseIfNeeded(const char* source, DWORD code, PEXCEPTION_RECORD exceptionRecord,
                                       PCONTEXT contextRecord, void* callerAddress ) {
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
}

namespace {
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
}

namespace {
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
}

namespace {
VOID NTAPI HookedRtlRaiseException(PEXCEPTION_RECORD ExceptionRecord) {
  const DWORD code = ExceptionRecord ? ExceptionRecord->ExceptionCode : 0;
  CaptureExplicitFatalRaiseIfNeeded("RtlRaiseException", code, ExceptionRecord, nullptr, __builtin_return_address(0));

  ::RtlRaiseException(ExceptionRecord);
}
}

namespace {
VOID NTAPI HookedRtlRaiseStatus(NTSTATUS Status) {
  CaptureExplicitFatalRaiseIfNeeded("RtlRaiseStatus", static_cast<DWORD>(Status), nullptr, nullptr,
                                    __builtin_return_address(0));

  ::RtlRaiseStatus(Status);
}
}

namespace {
NTSTATUS NTAPI HookedNtRaiseException(PEXCEPTION_RECORD ExceptionRecord, PCONTEXT ContextRecord, BOOLEAN FirstChance) {
  const DWORD code = ExceptionRecord ? ExceptionRecord->ExceptionCode : 0;
  CaptureExplicitFatalRaiseIfNeeded("NtRaiseException", code, ExceptionRecord, ContextRecord,
                                    __builtin_return_address(0));

  return ::NtRaiseException(ExceptionRecord, ContextRecord, FirstChance);
}
}

namespace {
BOOL WINAPI HookedTerminateProcess(HANDLE hProcess, UINT uExitCode) {
  CapturePreTerminationDumpIfNeeded("TerminateProcess", static_cast<DWORD>(uExitCode), IsCurrentProcessHandle(hProcess),
                                    nullptr, nullptr, __builtin_return_address(0));

  const auto original = g_OriginalTerminateProcess.load(std::memory_order_acquire);
  if (original) {
    return original(hProcess, uExitCode);
  }

  return TerminateProcess(hProcess, uExitCode);
}
}

namespace {
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
}

namespace {
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
}

namespace {
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
}

namespace {
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
}

namespace {
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
}

namespace {
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
}

namespace {
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
}

namespace {
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
}

namespace {
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

        patchedAny = true;
      }
    };

    auto patchExit = [&patchedAny](const char* sourceModule) {
      void* patchedOriginal = nullptr;
      if (IATHook::PatchIATAllModulesFiltered(sourceModule, "ExitProcess",
                                              reinterpret_cast<void*>(&HookedExitProcess), &patchedOriginal,
                                              &IsApplicationFatalIatModule)) {
        patchedAny = true;
      }
    };

    auto patchRtlExit = [&patchedAny](const char* sourceModule) {
      void* patchedOriginal = nullptr;
      if (IATHook::PatchIATAllModulesFiltered(sourceModule, "RtlExitUserProcess",
                                              reinterpret_cast<void*>(&HookedRtlExitUserProcess), &patchedOriginal,
                                              &IsApplicationFatalIatModule)) {
        patchedAny = true;
      }
    };

    auto patchNtTerminate = [&patchedAny](const char* sourceModule, const char* functionName = "NtTerminateProcess") {
      void* patchedOriginal = nullptr;
      if (IATHook::PatchIATAllModulesFiltered(sourceModule, functionName,
                                              reinterpret_cast<void*>(&HookedNtTerminateProcess), &patchedOriginal,
                                              &IsApplicationFatalIatModule)) {
        patchedAny = true;
      }
    };

    auto patchRaiseException = [&patchedAny](const char* sourceModule) {
      void* patchedOriginal = nullptr;
      if (IATHook::PatchIATAllModulesFiltered(sourceModule, "RaiseException",
                                              reinterpret_cast<void*>(&HookedRaiseException), &patchedOriginal,
                                              &IsApplicationFatalIatModule)) {
        patchedAny = true;
      }
    };

    auto patchRtlRaiseException = [&patchedAny](const char* sourceModule) {
      void* patchedOriginal = nullptr;
      if (IATHook::PatchIATAllModulesFiltered(sourceModule, "RtlRaiseException",
                                              reinterpret_cast<void*>(&HookedRtlRaiseException), &patchedOriginal,
                                              &IsApplicationFatalIatModule)) {
        patchedAny = true;
      }
    };

    auto patchRtlRaiseStatus = [&patchedAny](const char* sourceModule) {
      void* patchedOriginal = nullptr;
      if (IATHook::PatchIATAllModulesFiltered(sourceModule, "RtlRaiseStatus",
                                              reinterpret_cast<void*>(&HookedRtlRaiseStatus), &patchedOriginal,
                                              &IsApplicationFatalIatModule)) {
        patchedAny = true;
      }
    };

    auto patchNtRaiseException = [&patchedAny](const char* sourceModule, const char* functionName = "NtRaiseException") {
      void* patchedOriginal = nullptr;
      if (IATHook::PatchIATAllModulesFiltered(sourceModule, functionName,
                                              reinterpret_cast<void*>(&HookedNtRaiseException), &patchedOriginal,
                                              &IsApplicationFatalIatModule)) {
        patchedAny = true;
      }
    };

    auto patchCrtFatal = [&patchedAny](const char* functionName, void* hookFunction) {
      void* patchedOriginal = nullptr;
      if (IATHook::PatchIATAllModulesFiltered("ucrtbase.dll", functionName, hookFunction, &patchedOriginal,
                                              &IsApplicationFatalIatModule)) {
        patchedAny = true;
      }
    };

    std::vector<void*> inlineHookTargets;
    auto installInlineHook = [&patchedAny, &inlineHookTargets](const char* moduleName, const char* functionName,
                                                               void* hookFunction,
                                                               InlineHook::TrampolinePublisher publisher,
                                                               void* publisherContext) -> void* {
      void* target = ResolveModuleExport(moduleName, functionName);
      if (!target || target == hookFunction) {
        return nullptr;
      }
      if (std::find(inlineHookTargets.begin(), inlineHookTargets.end(), target) != inlineHookTargets.end()) {
        return nullptr;
      }

      void* trampoline = nullptr;
      if (!InlineHook::InstallPublished(target, hookFunction, &trampoline, publisher, publisherContext)) {
        HookLog("FatalExitDump: Inline pre-termination hook failed for %s!%s at %p", moduleName, functionName,
                target);
        return nullptr;
      }

      inlineHookTargets.push_back(target);
      patchedAny = true;
      HookLogImportant("FatalExitDump: Installed inline pre-termination hook for %s!%s at %p (trampoline=%p)",
                       moduleName, functionName, target, trampoline);
      return trampoline;
    };

    if (!installInlineHook("KERNELBASE.dll", "RaiseFailFastException",
                           reinterpret_cast<void*>(&HookedRaiseFailFastException),
                           &PublishFatalHookTrampoline<RaiseFailFastException_t>,
                           &g_OriginalRaiseFailFastException)) {
      installInlineHook("kernel32.dll", "RaiseFailFastException",
                        reinterpret_cast<void*>(&HookedRaiseFailFastException),
                        &PublishFatalHookTrampoline<RaiseFailFastException_t>, &g_OriginalRaiseFailFastException);
    }
    if (!installInlineHook("KERNELBASE.dll", "TerminateProcess", reinterpret_cast<void*>(&HookedTerminateProcess),
                           &PublishFatalHookTrampoline<TerminateProcess_t>, &g_OriginalTerminateProcess)) {
      installInlineHook("kernel32.dll", "TerminateProcess", reinterpret_cast<void*>(&HookedTerminateProcess),
                        &PublishFatalHookTrampoline<TerminateProcess_t>, &g_OriginalTerminateProcess);
    }
    if (!installInlineHook("KERNELBASE.dll", "ExitProcess", reinterpret_cast<void*>(&HookedExitProcess),
                           &PublishFatalHookTrampoline<ExitProcess_t>, &g_OriginalExitProcess)) {
      installInlineHook("kernel32.dll", "ExitProcess", reinterpret_cast<void*>(&HookedExitProcess),
                        &PublishFatalHookTrampoline<ExitProcess_t>, &g_OriginalExitProcess);
    }
    installInlineHook("ntdll.dll", "RtlExitUserProcess", reinterpret_cast<void*>(&HookedRtlExitUserProcess),
                      &PublishFatalHookTrampoline<RtlExitUserProcess_t>, &g_OriginalRtlExitUserProcess);
    if (!installInlineHook("ntdll.dll", "NtTerminateProcess", reinterpret_cast<void*>(&HookedNtTerminateProcess),
                           &PublishFatalHookTrampoline<NtTerminateProcess_t>, &g_OriginalNtTerminateProcess)) {
      installInlineHook("ntdll.dll", "ZwTerminateProcess", reinterpret_cast<void*>(&HookedNtTerminateProcess),
                        &PublishFatalHookTrampoline<NtTerminateProcess_t>, &g_OriginalNtTerminateProcess);
    }
    installInlineHook("ucrtbase.dll", "_invalid_parameter_noinfo_noreturn",
                      reinterpret_cast<void*>(&HookedInvalidParameterNoInfoNoReturn),
                      &PublishFatalHookTrampoline<InvalidParameterNoInfoNoReturn_t>,
                      &g_OriginalInvalidParameterNoInfoNoReturn);
    installInlineHook("ucrtbase.dll", "_invoke_watson", reinterpret_cast<void*>(&HookedInvokeWatson),
                      &PublishFatalHookTrampoline<InvokeWatson_t>, &g_OriginalInvokeWatson);
    installInlineHook("ucrtbase.dll", "abort", reinterpret_cast<void*>(&HookedAbort),
                      &PublishFatalHookTrampoline<Abort_t>, &g_OriginalAbort);
    installInlineHook("ucrtbase.dll", "terminate", reinterpret_cast<void*>(&HookedTerminate),
                      &PublishFatalHookTrampoline<Terminate_t>, &g_OriginalTerminate);
    installInlineHook("ucrtbase.dll", "_purecall", reinterpret_cast<void*>(&HookedPurecall),
                      &PublishFatalHookTrampoline<Purecall_t>, &g_OriginalPurecall);

    // Publish every callable trampoline before routing any imports to our
    // wrappers. Exception/termination paths can run on arbitrary threads while
    // this bootstrap is in progress. The raise primitives themselves stay
    // byte-identical: VEH plus IAT/dynamic routing provides coverage without a
    // process-wide exception-dispatch patch race.
    patchRaise("kernel32.dll");
    patchRaise("KERNELBASE.dll");
    patchRaiseException("kernel32.dll");
    patchRaiseException("KERNELBASE.dll");
    patchRtlRaiseException("ntdll.dll");
    patchRtlRaiseStatus("ntdll.dll");
    patchNtRaiseException("ntdll.dll");
    patchNtRaiseException("ntdll.dll", "ZwRaiseException");
    patchTerminate("kernel32.dll");
    patchTerminate("KERNELBASE.dll");
    patchExit("kernel32.dll");
    patchExit("KERNELBASE.dll");
    patchRtlExit("ntdll.dll");
    patchNtTerminate("ntdll.dll");
    patchNtTerminate("ntdll.dll", "ZwTerminateProcess");
    patchCrtFatal("_invalid_parameter_noinfo_noreturn", reinterpret_cast<void*>(&HookedInvalidParameterNoInfoNoReturn));
    patchCrtFatal("_invoke_watson", reinterpret_cast<void*>(&HookedInvokeWatson));
    patchCrtFatal("abort", reinterpret_cast<void*>(&HookedAbort));
    patchCrtFatal("terminate", reinterpret_cast<void*>(&HookedTerminate));
    patchCrtFatal("_purecall", reinterpret_cast<void*>(&HookedPurecall));

    IATHook::RegisterDynamicHook("RaiseFailFastException", reinterpret_cast<void*>(&HookedRaiseFailFastException),
                                 nullptr);
    IATHook::RegisterDynamicHook("RaiseException", reinterpret_cast<void*>(&HookedRaiseException), nullptr);
    IATHook::RegisterDynamicHook("RtlRaiseException", reinterpret_cast<void*>(&HookedRtlRaiseException), nullptr);
    IATHook::RegisterDynamicHook("RtlRaiseStatus", reinterpret_cast<void*>(&HookedRtlRaiseStatus), nullptr);
    IATHook::RegisterDynamicHook("NtRaiseException", reinterpret_cast<void*>(&HookedNtRaiseException), nullptr);
    IATHook::RegisterDynamicHook("ZwRaiseException", reinterpret_cast<void*>(&HookedNtRaiseException), nullptr);
    IATHook::RegisterDynamicHook("TerminateProcess", reinterpret_cast<void*>(&HookedTerminateProcess), nullptr);
    IATHook::RegisterDynamicHook("ExitProcess", reinterpret_cast<void*>(&HookedExitProcess), nullptr);
    IATHook::RegisterDynamicHook("RtlExitUserProcess", reinterpret_cast<void*>(&HookedRtlExitUserProcess), nullptr);
    IATHook::RegisterDynamicHook("NtTerminateProcess", reinterpret_cast<void*>(&HookedNtTerminateProcess), nullptr);
    IATHook::RegisterDynamicHook("ZwTerminateProcess", reinterpret_cast<void*>(&HookedNtTerminateProcess), nullptr);
    IATHook::RegisterDynamicHookFiltered("_invalid_parameter_noinfo_noreturn",
                                         reinterpret_cast<void*>(&HookedInvalidParameterNoInfoNoReturn), nullptr,
                                         IsUcrtDynamicHookModule);
    IATHook::RegisterDynamicHookFiltered("_invoke_watson", reinterpret_cast<void*>(&HookedInvokeWatson), nullptr,
                                         IsUcrtDynamicHookModule);
    IATHook::RegisterDynamicHookFiltered("abort", reinterpret_cast<void*>(&HookedAbort), nullptr,
                                         IsUcrtDynamicHookModule);
    IATHook::RegisterDynamicHookFiltered("terminate", reinterpret_cast<void*>(&HookedTerminate), nullptr,
                                         IsUcrtDynamicHookModule);
    IATHook::RegisterDynamicHookFiltered("_purecall", reinterpret_cast<void*>(&HookedPurecall), nullptr,
                                         IsUcrtDynamicHookModule);

    HookLogImportant(
        "FatalExitDump: Installed pre-termination dump hooks (patched=%d inline=%zu raiseOriginal=%p terminateOriginal=%p "
        "exitOriginal=%p rtlExitOriginal=%p ntTerminateOriginal=%p crtFatalOriginal=%p)",
        patchedAny ? 1 : 0, inlineHookTargets.size(),
        reinterpret_cast<void*>(g_OriginalRaiseFailFastException.load(std::memory_order_acquire)),
        reinterpret_cast<void*>(g_OriginalTerminateProcess.load(std::memory_order_acquire)),
        reinterpret_cast<void*>(g_OriginalExitProcess.load(std::memory_order_acquire)),
        reinterpret_cast<void*>(g_OriginalRtlExitUserProcess.load(std::memory_order_acquire)),
        reinterpret_cast<void*>(g_OriginalNtTerminateProcess.load(std::memory_order_acquire)),
        reinterpret_cast<void*>(g_OriginalInvokeWatson.load(std::memory_order_acquire)));
  });
}
}

namespace {
std::mutex g_ExternalDumpStormMutex;
}

namespace {
std::unordered_map<std::string, ExternalDumpStormRecord> g_ExternalDumpStormRecords;
}

namespace {
ce::crash_dump_policy::ExternalDumpSignature BuildExternalDumpSignature(
    const char* sourcePath, DWORD processId, PMINIDUMP_EXCEPTION_INFORMATION exceptionParam) {
  ce::crash_dump_policy::ExternalDumpSignature signature;
  signature.processId = processId;
  const char* fileName = ce::crash_dump_policy::GetPathFileName(sourcePath);
  signature.dumpBaseName = fileName && fileName[0] ? fileName : "external_dump";

  if (exceptionParam && exceptionParam->ExceptionPointers && exceptionParam->ExceptionPointers->ExceptionRecord) {
    const EXCEPTION_RECORD* record = exceptionParam->ExceptionPointers->ExceptionRecord;
    signature.hasExceptionInfo = true;
    signature.exceptionCode = record->ExceptionCode;
    signature.exceptionAddress = reinterpret_cast<uintptr_t>(record->ExceptionAddress);
    signature.exceptionThreadId = exceptionParam->ThreadId;
  }
  return signature;
}
}

namespace {
ExternalDumpGateDecision BeginExternalDumpCaptureForSignature(
    const ce::crash_dump_policy::ExternalDumpSignature& signature) {
  ExternalDumpGateDecision decision;
  decision.key = ce::crash_dump_policy::BuildExternalDumpSignatureKey(signature);
  decision.strongSignature = ce::crash_dump_policy::IsStrongExternalDumpSignature(signature);

  const ULONGLONG nowMs = GetTickCount64();
  std::lock_guard<std::mutex> lock(g_ExternalDumpStormMutex);
  ExternalDumpStormRecord& record = g_ExternalDumpStormRecords[decision.key];
  if (record.hitCount == 0 || (nowMs >= record.firstHitMs &&
                               (nowMs - record.firstHitMs) > ce::crash_dump_policy::kExternalDumpStormWindowMs)) {
    record = {};
    record.firstHitMs = nowMs;
    record.strongSignature = decision.strongSignature;
  }

  record.lastHitMs = nowMs;
  record.hitCount += 1;
  decision.hitCount = record.hitCount;

  decision.mirrorAllowed =
      !ce::crash_dump_policy::ShouldSuppressDuplicateExternalDumpArtifacts(record.hitCount, record.mirrorAttempted);
  if (decision.mirrorAllowed) {
    record.mirrorAttempted = true;
  }

  decision.supplementalAllowed = !ce::crash_dump_policy::ShouldSuppressDuplicateExternalDumpArtifacts(
      record.hitCount, record.supplementalAttempted);
  if (decision.supplementalAllowed) {
    record.supplementalAttempted = true;
  }

  decision.terminateProcess = ce::crash_dump_policy::ShouldTerminateAfterExternalDumpStorm(
      record.strongSignature, record.hitCount, record.firstHitMs, nowMs, record.supplementalCaptured,
      record.terminationRequested);
  if (decision.terminateProcess) {
    record.terminationRequested = true;
  }

  return decision;
}
}

namespace {
void MarkExternalSupplementalDumpCaptured(const std::string& key) {
  std::lock_guard<std::mutex> lock(g_ExternalDumpStormMutex);
  auto it = g_ExternalDumpStormRecords.find(key);
  if (it != g_ExternalDumpStormRecords.end()) {
    it->second.supplementalCaptured = true;
  }
}
}

namespace {
void TerminateProcessAfterExternalDumpStorm(const ExternalDumpGateDecision& decision) {
  if (!decision.terminateProcess) {
    return;
  }

  bool expected = false;
  if (!g_ProcessTerminating.compare_exchange_strong(expected, true, std::memory_order_acq_rel,
                                                     std::memory_order_acquire)) {
    return;
  }

  HookLogImportant(
      "CrashMirror: External dump storm threshold reached — terminating hung process "
      "(hits=%u strong=%d signature=%s)",
      decision.hitCount, decision.strongSignature ? 1 : 0, decision.key.c_str());
  OutputDebugStringA("[CrashMirror] External dump storm threshold reached; terminating process.\n");
  TerminateProcess(GetCurrentProcess(), ce::crash_dump_policy::kExternalDumpStormTerminationExitCode);
}
}

namespace {
std::string BuildExternalDumpMirrorPath(const char* sourcePath) {
  const std::string dumpDir = GetCrashDumpDirectory();
  if (dumpDir.empty()) {
    return {};
  }

  std::filesystem::path mirrorPath(dumpDir);
  mirrorPath /= ce::crash_dump_policy::BuildMirroredExternalDumpFileName(sourcePath);
  return mirrorPath.string();
}
}

namespace {
void MirrorExternalDumpArtifactIfNeeded(const char* sourcePath, HANDLE hProcess, DWORD processId, MINIDUMP_TYPE dumpType,
                                        PMINIDUMP_EXCEPTION_INFORMATION exceptionParam,
                                        PMINIDUMP_USER_STREAM_INFORMATION userStreamParam,
                                        PMINIDUMP_CALLBACK_INFORMATION callbackParam) {
  if (!sourcePath || sourcePath[0] == '\0') {
    return;
  }

  const std::string dumpDir = GetCrashDumpDirectory();
  if (!ce::crash_dump_policy::ShouldMirrorExternalDumpToSessionDirectory(sourcePath, dumpDir.c_str())) {
    return;
  }

  const auto signature = BuildExternalDumpSignature(sourcePath, processId, exceptionParam);
  const ExternalDumpGateDecision gateDecision = BeginExternalDumpCaptureForSignature(signature);
  if (!gateDecision.mirrorAllowed && !gateDecision.supplementalAllowed) {
    HookLogImportant(
        "CrashMirror: Suppressed duplicate external dump storm artifact "
        "(hits=%u strong=%d source=%s signature=%s)",
        gateDecision.hitCount, gateDecision.strongSignature ? 1 : 0, sourcePath, gateDecision.key.c_str());
    TerminateProcessAfterExternalDumpStorm(gateDecision);
    return;
  }

  const std::string mirrorPath = BuildExternalDumpMirrorPath(sourcePath);
  if (mirrorPath.empty()) {
    return;
  }

  const std::filesystem::path destination(mirrorPath);
  std::error_code ec;
  std::filesystem::create_directories(destination.parent_path(), ec);
  if (ec) {
    HookLog("CrashMirror: Failed to create mirror directory for %s (err=%d)", mirrorPath.c_str(),
            static_cast<int>(ec.value()));
    return;
  }

  const auto original = g_OriginalMiniDumpWriteDump.load(std::memory_order_acquire);
  if (!original) {
    HookLog("CrashMirror: Missing MiniDumpWriteDump trampoline while mirroring %s", sourcePath);
    return;
  }

  if (gateDecision.mirrorAllowed) {
    const std::filesystem::path tempDestination =
        destination.parent_path() /
        ce::crash_dump_policy::BuildInProgressDumpFileName(destination.filename().string().c_str());
    const std::string tempMirrorPath = tempDestination.string();
    DeleteFileA(tempMirrorPath.c_str());

    HANDLE mirrorFile =
        CreateFileA(tempMirrorPath.c_str(), GENERIC_READ | GENERIC_WRITE,
                    FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, CREATE_ALWAYS,
                    FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, nullptr);
    if (mirrorFile == INVALID_HANDLE_VALUE) {
      HookLog("CrashMirror: Failed to create mirror dump %s (err=%lu)", tempMirrorPath.c_str(), GetLastError());
      return;
    }

    const BOOL mirrorResult = original(hProcess, processId, mirrorFile, dumpType, exceptionParam, userStreamParam,
                                       callbackParam);
    const DWORD mirrorError = mirrorResult ? ERROR_SUCCESS : GetLastError();
    if (mirrorResult) {
      FlushFileBuffers(mirrorFile);
    }

    LARGE_INTEGER mirrorSize = {};
    const bool mirrorNonEmpty = mirrorResult && GetFileSizeEx(mirrorFile, &mirrorSize) && mirrorSize.QuadPart > 0;
    CloseHandle(mirrorFile);

    if (!mirrorNonEmpty) {
      DeleteFileA(tempMirrorPath.c_str());
      HookLog("CrashMirror: Failed to re-emit external dump %s -> %s (err=%lu nonEmpty=%d)", sourcePath,
              mirrorPath.c_str(), mirrorError, mirrorNonEmpty ? 1 : 0);
      return;
    }

    if (!MoveFileExA(tempMirrorPath.c_str(), mirrorPath.c_str(), MOVEFILE_WRITE_THROUGH)) {
      const DWORD moveError = GetLastError();
      if (CopyFileA(tempMirrorPath.c_str(), mirrorPath.c_str(), FALSE)) {
        DeleteFileA(tempMirrorPath.c_str());
        HookLogImportant("CrashMirror: Mirrored external dump %s -> %s via CopyFile fallback", sourcePath,
                         mirrorPath.c_str());
      } else {
        const DWORD copyError = GetLastError();
        HookLog("CrashMirror: Failed to promote mirror dump %s -> %s (moveErr=%lu copyErr=%lu); preserving temp",
                tempMirrorPath.c_str(), mirrorPath.c_str(), moveError, copyError);
      }
      return;
    }

    HookLogImportant("CrashMirror: Mirrored external dump %s -> %s", sourcePath, mirrorPath.c_str());
  }

  if (gateDecision.supplementalAllowed) {
    if (!WriteSupplementalCrashDump(sourcePath, hProcess, processId, ce::crash_dump_policy::kRichCrashDumpType,
                                    exceptionParam, userStreamParam, callbackParam)) {
      HookLog("CrashMirror: Supplemental CE-owned dump capture did not succeed for %s", sourcePath);
      return;
    }

    MarkExternalSupplementalDumpCaptured(gateDecision.key);
    HookLogImportant("CrashMirror: Captured supplemental CE-owned dump for externally handled crash %s", sourcePath);
  }

  TerminateProcessAfterExternalDumpStorm(gateDecision);
}
}

namespace {
void TryInstallMiniDumpWriteDumpHookForModule(HMODULE module, const char* moduleNameOrPath) {
  if (g_MiniDumpWriteDumpHookInstalled.load(std::memory_order_acquire) || !module || !moduleNameOrPath) {
    return;
  }

  const char* baseName = std::strrchr(moduleNameOrPath, '\\');
  baseName = baseName ? baseName + 1 : moduleNameOrPath;
  const char* slash = std::strrchr(baseName, '/');
  baseName = slash ? slash + 1 : baseName;
  if (_stricmp(baseName, "dbghelp.dll") != 0) {
    return;
  }

  std::call_once(g_MiniDumpWriteDumpHookOnce, [module]() {
    auto target = reinterpret_cast<void*>(GetProcAddress(module, "MiniDumpWriteDump"));
    if (!target) {
      HookLog("CrashMirror: dbghelp.dll loaded but MiniDumpWriteDump export was not found");
      return;
    }

    void* trampoline = nullptr;
    if (!InlineHook::Install(target, reinterpret_cast<void*>(&HookedMiniDumpWriteDump), &trampoline)) {
      HookLog("CrashMirror: Failed to install MiniDumpWriteDump inline hook at %p", target);
      return;
    }

    g_OriginalMiniDumpWriteDump.store(reinterpret_cast<MiniDumpWriteDump_t>(trampoline), std::memory_order_release);
    g_MiniDumpWriteDumpHookInstalled.store(true, std::memory_order_release);
    HookLogImportant("CrashMirror: Installed MiniDumpWriteDump hook at %p (trampoline=%p)", target, trampoline);
  });
}
}

namespace {
BOOL WINAPI HookedMiniDumpWriteDump(HANDLE hProcess, DWORD ProcessId, HANDLE hFile, MINIDUMP_TYPE DumpType,
                                    PMINIDUMP_EXCEPTION_INFORMATION ExceptionParam,
                                    PMINIDUMP_USER_STREAM_INFORMATION UserStreamParam,
                                    PMINIDUMP_CALLBACK_INFORMATION CallbackParam) {
  const auto original = g_OriginalMiniDumpWriteDump.load(std::memory_order_acquire);
  if (!original) {
    SetLastError(ERROR_PROC_NOT_FOUND);
    return FALSE;
  }

  if (t_InMiniDumpWriteDumpHook) {
    return original(hProcess, ProcessId, hFile, DumpType, ExceptionParam, UserStreamParam, CallbackParam);
  }

  t_InMiniDumpWriteDumpHook = true;

  char targetPath[MAX_PATH * 4] = {};
  bool hasTargetPath = false;
  if (hFile && hFile != INVALID_HANDLE_VALUE) {
    const DWORD pathLength = GetFinalPathNameByHandleA(hFile, targetPath, static_cast<DWORD>(sizeof(targetPath)),
                                                       FILE_NAME_NORMALIZED);
    if (pathLength > 0 && pathLength < sizeof(targetPath)) {
      constexpr const char* kLongPathPrefix = "\\\\?\\";
      if (std::strncmp(targetPath, kLongPathPrefix, 4) == 0) {
        std::memmove(targetPath, targetPath + 4, std::strlen(targetPath + 4) + 1);
      }
      hasTargetPath = true;
    }
  }

  const BOOL result = original(hProcess, ProcessId, hFile, DumpType, ExceptionParam, UserStreamParam, CallbackParam);
  const DWORD lastError = result ? ERROR_SUCCESS : GetLastError();

  if (result && ProcessId == GetCurrentProcessId() && hasTargetPath) {
    MirrorExternalDumpArtifactIfNeeded(targetPath, hProcess, ProcessId, DumpType, ExceptionParam, UserStreamParam,
                                       CallbackParam);
  }

  t_InMiniDumpWriteDumpHook = false;
  if (!result) {
    SetLastError(lastError);
  }
  return result;
}
}

bool IsProcessTerminating() { return g_ProcessTerminating.load(std::memory_order_acquire); }

static ProcessCategory g_ProcessCategory = ProcessCategory::PotentialGame;

static bool g_isDormant = false;

static bool g_isSkippedProcess = false;

std::atomic<bool> g_HookThreadRunning{false}; // Track if HookThread is active

// Global Hook Pointers
DX12Hook *g_DX12Hook = nullptr;

DX11Hook *g_DX11Hook = nullptr;

DX9Hook *g_DX9Hook = nullptr;

DDrawHook *g_DDrawHook = nullptr;

DX8Hook *g_DX8Hook = nullptr;

OpenGLHook *g_OpenGLHook = nullptr;

// Global Local Config
AppConfig *g_pLocalConfig = nullptr;

namespace {
std::unique_ptr<AppConfig> g_LocalConfigOwner;
}

namespace {
bool IsDXVKD3D11WrapperLoaded() {
  return IsDllFromProject("d3d11.dll", "dxvk");
}
}

namespace {
void EnsureLocalConfigAllocated() {
  if (!g_LocalConfigOwner) {
    g_LocalConfigOwner = std::make_unique<AppConfig>();
  }
  g_pLocalConfig = g_LocalConfigOwner.get();
}
}

std::atomic<GetCommandLineA_t> OriginalGetCommandLineA{nullptr};

std::atomic<GetCommandLineW_t> OriginalGetCommandLineW{nullptr};

std::atomic<CreateProcessA_t> OriginalCreateProcessA{nullptr};

std::atomic<CreateProcessW_t> OriginalCreateProcessW{nullptr};

RegQueryValueExW_t OriginalRegQueryValueExW = nullptr;

std::mutex g_HookMutex;

HANDLE g_hCheckHooksEvent = NULL;

namespace {
void CloseCheckHooksEvent() {
  HANDLE hEvent = reinterpret_cast<HANDLE>(InterlockedExchangePointer(
      reinterpret_cast<PVOID volatile *>(&g_hCheckHooksEvent), nullptr));
  if (hEvent) {
    CloseHandle(hEvent);
  }
}
}

// DLL Redirection Helper
std::string GetRedirectedPath(const std::string &requestedPath) {
  if (requestedPath.empty())
    return "";

  try {
    // Basic path parsing without std::filesystem
    std::string filename;
    size_t lastSlash = requestedPath.find_last_of("\\/");
    if (lastSlash != std::string::npos) {
      filename = requestedPath.substr(lastSlash + 1);
    } else {
      filename = requestedPath;
    }

    std::string filenameLower = filename;
    std::transform(filenameLower.begin(), filenameLower.end(),
                   filenameLower.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });

    std::string overridePath;
    bool isStreamlineMatch = false;

    // 1. DLSS/Streamline Logic - Only if no custom detour set
    if (overridePath.empty() && g_pLocalConfig) {
      if (filenameLower == "nvngx_dlss.dll") {
        overridePath = g_pLocalConfig->graphics.dlssSrDllPath;
      }
      // 2. DLSS Frame Generation
      else if (filenameLower == "nvngx_dlssg.dll") {
        overridePath = g_pLocalConfig->graphics.dlssFgDllPath;
      }
      // 3. DLSS Ray Reconstruction (Denoiser)
      else if (filenameLower == "nvngx_dlssd.dll") {
        overridePath = g_pLocalConfig->graphics.dlssRrDllPath;
      }
      // 4. Streamline and related components
      else if (filenameLower.find("sl.") == 0 ||
               filenameLower == "nvngx_deepdvc.dll" ||
               filenameLower == "nvlowlatencyvk.dll") {
        overridePath = g_pLocalConfig->graphics.streamlineDllPath;
        isStreamlineMatch = true;
      }
    }

    if (!overridePath.empty()) {
      std::string finalPath;

      // Check if overridePath has an extension (heuristic for file vs dir)
      size_t overrideLastSlash = overridePath.find_last_of("\\/");
      size_t overrideLastDot = overridePath.find_last_of('.');
      bool hasExtension = (overrideLastDot != std::string::npos &&
                           (overrideLastSlash == std::string::npos ||
                            overrideLastDot > overrideLastSlash));

      if (hasExtension) {
        // It looks like a file.
        // If it ends with the SAME filename as requested, just use it.
        std::string cfgFilename;
        size_t cfgLastSlash = overridePath.find_last_of("\\/");
        if (cfgLastSlash != std::string::npos) {
          cfgFilename = overridePath.substr(cfgLastSlash + 1);
        } else {
          cfgFilename = overridePath;
        }

        std::string cfgFilenameLower = cfgFilename;
        std::transform(cfgFilenameLower.begin(), cfgFilenameLower.end(),
                       cfgFilenameLower.begin(),
                       [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });

        if (cfgFilenameLower == filenameLower) {
          finalPath = overridePath;
        } else {
          // Config points to a file, but we want a potentially different file
          // Take parent folder, then append requested filename.
          if (cfgLastSlash != std::string::npos) {
            finalPath = overridePath.substr(0, cfgLastSlash) + "\\" + filename;
          } else {
            finalPath = filename; // Should not happen if full path
          }
        }
      } else {
        // It looks like a directory. Append the requested filename.
        if (overridePath.back() == '\\' || overridePath.back() == '/') {
          finalPath = overridePath + filename;
        } else {
          finalPath = overridePath + "\\" + filename;
        }
      }

      // For streamline DLLs, verify the file exists at the redirect path.
      // If absent, fall back gracefully to the default load path.
      if (isStreamlineMatch && !finalPath.empty()) {
        if (GetFileAttributesA(finalPath.c_str()) == INVALID_FILE_ATTRIBUTES) {
          HookLog("Streamline DLL %s not found at redirect path %s - "
                  "falling back to default load path",
                  filename.c_str(), finalPath.c_str());
          return "";
        }
      }

      HookLog("Redirecting %s to: %s", filename.c_str(), finalPath.c_str());
      return finalPath;
    }

  } catch (...) {
    // Never let a redirect-resolution failure escape into the game's loader.
    // Falling back to the default load path is correct, but silently doing so
    // hid why a Streamline/FG DLL was not redirected.
    HookLog("Loader redirect resolution threw for %s - falling back to "
            "default load path",
            requestedPath.c_str());
  }
  return "";
}

static bool NeedsLoaderRedirectionHook() {
  if (!g_pLocalConfig) {
    return false;
  }

  const auto &gfx = g_pLocalConfig->graphics;
  return !gfx.dlssSrDllPath.empty() || !gfx.dlssFgDllPath.empty() ||
         !gfx.dlssRrDllPath.empty() || !gfx.streamlineDllPath.empty();
}

static bool NeedsLowLevelModuleLoadObservationHook() {
  // Some launchers and overlays load native FG runtimes through ntdll directly.
  // Observing LdrLoadDll lets us arm FFX/Streamline hooks before the game can
  // cache API pointers such as ffxConfigure.
  return true;
}

// Hooked RegQueryValueExW - For DLSS Debug Overlay
LSTATUS WINAPI HookedRegQueryValueExW(HKEY hKey, LPCWSTR lpValueName,
                                      LPDWORD lpReserved, LPDWORD lpType,
                                      LPBYTE lpData, LPDWORD lpcbData) {
  LSTATUS status = OriginalRegQueryValueExW(hKey, lpValueName, lpReserved,
                                            lpType, lpData, lpcbData);

  // Check if probing for DLSS Indicator
  if (lpValueName && _wcsicmp(lpValueName, L"ShowDlssIndicator") == 0) {
    // Only if we have a config override
    if (g_pLocalConfig && !g_pLocalConfig->graphics.dlssDebugOverlay.empty() &&
        g_pLocalConfig->graphics.dlssDebugOverlay != "default") {
      // If caller provided buffer to read data
      if (lpData && lpcbData && *lpcbData >= 4) {
        DWORD *outData = (DWORD *)lpData;
        if (g_pLocalConfig->graphics.dlssDebugOverlay == "on") {
          *outData = 0x400; // Force ON
          // HookLog("RegQueryValueExW: Force-enabled DLSS Indicator");
        } else if (g_pLocalConfig->graphics.dlssDebugOverlay == "off") {
          *outData = 0; // Force OFF
        }
        return ERROR_SUCCESS; // Pretend we succeeded even if registry key
                              // didn't exist
      }
    }
  }
  return status;
}

static PVOID g_DllNotificationCookie = nullptr;

static VOID CALLBACK OverlayDllNotificationCallback(ULONG reason,
                                                    PCLDR_DLL_NOTIFICATION_DATA data,
                                                    PVOID /*context*/) {
  if (!data || !data->BaseDllName || !data->BaseDllName->Buffer ||
      data->BaseDllName->Length == 0) {
    return;
  }
  // Narrow the (short) base name on the stack — loader-safe, no allocation.
  char base[128];
  const wchar_t *wide = data->BaseDllName->Buffer;
  const size_t wideChars = data->BaseDllName->Length / sizeof(wchar_t);
  size_t n = 0;
  for (; n < wideChars && n < (sizeof(base) - 1); ++n) {
    const wchar_t c = wide[n];
    base[n] = (c > 0 && c < 128) ? static_cast<char>(c) : '?';
  }
  base[n] = '\0';

  if (reason == LDR_DLL_NOTIFICATION_REASON_LOADED) {
    const char *matched = ce::overlay_compat::NoteModuleLoadedForOverlayCache(base);
    if (matched) {
      HookLog("DllNotification: third-party overlay module loaded: %s", matched);
    }
  } else if (reason == LDR_DLL_NOTIFICATION_REASON_UNLOADED) {
    const char *matched = ce::overlay_compat::NoteModuleUnloadedForOverlayCache(base);
    if (matched) {
      HookLog("DllNotification: third-party overlay module unloaded: %s", matched);
    }
    // Games can unload the whole Streamline stack when toggling DLSS FG off.
    // Stale CE hook slots pointing into the departing image generation must be
    // invalidated here (loader-safe: interlocked/atomic writes + light logging),
    // or the next reload can land a different sl.* module inside the old range
    // and stale trampolines jump mid-instruction into it (20260612_003407).
    if (ce::streamline_runtime_policy::ShouldInvalidateStreamlineHooksOnModuleUnload(base)) {
      StreamlineHook::OnModuleUnloaded(data->DllBase, data->SizeOfImage, base);
    }
  }
}

// Seeds already-loaded overlays and registers the load/unload notification. MUST be called
// off the Present thread and after DllMain returns (i.e. from HookThread) — registering and
// the one-time GetModuleHandleA seed walk are not safe under the DllMain loader lock.
static void InitializeThirdPartyOverlayDetection() {
  static std::atomic<bool> s_initialized{false};
  bool expected = false;
  if (!s_initialized.compare_exchange_strong(expected, true)) {
    return;
  }

  // 1) Seed overlays already present before our hooks installed (one-time loader walk).
  const uint32_t seeded = ce::overlay_compat::SeedThirdPartyOverlayModuleCacheFromLoader();
  const char *seededName = ce::overlay_compat::GetLoadedThirdPartyOverlayModuleName();
  HookLog("Third-party overlay detection: seed scan bits=0x%X active=%s", seeded,
          seededName ? seededName : "none");

  // 2) Register for all subsequent load/unload events (covers every load mechanism + unloads).
  HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
  auto registerFn = ntdll ? reinterpret_cast<PFN_LdrRegisterDllNotification>(
                                GetProcAddress(ntdll, "LdrRegisterDllNotification"))
                          : nullptr;
  if (registerFn) {
    const NTSTATUS status =
        registerFn(0, &OverlayDllNotificationCallback, nullptr, &g_DllNotificationCookie);
    if (status == 0) {
      HookLog("Third-party overlay detection: LdrRegisterDllNotification active (cookie=%p)",
              g_DllNotificationCookie);
    } else {
      HookLog("Third-party overlay detection: LdrRegisterDllNotification FAILED (0x%lX) — "
              "falling back to LoadLibrary/LdrLoadDll notifications only",
              static_cast<unsigned long>(status));
    }
  } else {
    HookLog("Third-party overlay detection: LdrRegisterDllNotification unavailable — falling "
            "back to LoadLibrary/LdrLoadDll notifications only");
  }
}

void NotifyHookModuleLoaded(HMODULE module, const char *moduleNameOrPath) {
  if (!module)
    return;

  // A DLL just loaded. Update third-party-overlay detection ONLY if this module is itself a
  // known overlay module — a cheap base-name compare, no loader walk. Unrelated loads (e.g.
  // d3d11.dll churn during the Alt+Tab mode switch) must NOT touch the detection state, so the
  // Present hot path never has to re-walk the loader. Full load/unload coverage is provided by
  // the LdrRegisterDllNotification callback; this is the belt-and-suspenders load path.
  ce::overlay_compat::NoteModuleLoadedForOverlayCache(moduleNameOrPath);

  TryInstallMiniDumpWriteDumpHookForModule(module, moduleNameOrPath);
  StreamlineHook::OnModuleLoaded(module, moduleNameOrPath);

  // Detect nvapi64.dll loading — trigger Reflex limiter initialization immediately
  // so our dynamic hook is registered before the game calls GetProcAddress.
  if (moduleNameOrPath) {
    const char *baseName = strrchr(moduleNameOrPath, '\\');
    baseName = baseName ? baseName + 1 : moduleNameOrPath;
    const char *slash = strrchr(baseName, '/');
    baseName = slash ? slash + 1 : baseName;
    if (_stricmp(baseName, "d3d8.dll") == 0) {
      HookLog("NotifyHookModuleLoaded: d3d8.dll detected - preparing DX8 hooks");
      DX8Hook_OnModuleLoaded();
    }
    if (_stricmp(baseName, "nvapi64.dll") == 0 || _stricmp(baseName, "nvapi.dll") == 0) {
      HookLog("NotifyHookModuleLoaded: %s detected — initializing Reflex limiter", baseName);
      g_ReflexLimiter.Init();
    }
    if (ce::overlay_compat::IsFFXFrameGenerationModulePath(moduleNameOrPath)) {
      HookLogImportant(
          "NotifyHookModuleLoaded: %s detected - initializing FFX hooks immediately for native FSR callback bridge",
          baseName);
      FFXHook::Init();
    }
    // CRITICAL FIX: Hook d3d11.dll at LoadLibrary time, BEFORE the game calls
    // GetProcAddress. UE3 caches D3D11 context vtable function pointers (Draw
    // etc.) at startup. If our IAT/dynamic hooks are installed too late (e.g.
    // from the HookThread's periodic scan), the game gets the real
    // D3D11CreateDevice pointer, creates the device, and UE3 caches the
    // original vtable entries — completely bypassing our vtable detours.
    //
    // By installing D3D11 hooks here, inside LoadLibrary before it returns,
    // our GetProcAddress hook intercepts the game's subsequent
    // GetProcAddress(d3d11.dll, "D3D11CreateDevice") and returns
    // Wrapped_D3D11CreateDevice instead. The wrapper creates a wrapped
    // device/context, and vtable hooks installed during device creation
    // actually intercept Draw calls.
    if (_stricmp(baseName, "d3d11.dll") == 0) {
      HookLog("NotifyHookModuleLoaded: d3d11.dll detected — installing D3D11 hooks "
              "immediately (pre-GetProcAddress)");
      IATHook::InitializeD3D11Hooks();
      // CRITICAL: Install the full GetProcAddress hook NOW, before LoadLibrary
      // returns. This ensures GetProcAddress(d3d11.dll, "D3D11CreateDevice")
      // called by ANY module (EXE or game DLL) is intercepted.
      //
      // We do NOT use PatchEAT() — the real 32-bit D3D11CreateDeviceAndSwapChain
      // on Win10/11 reads its own EAT entry internally, causing infinite
      // recursion if the EAT is patched.
      //
      // Prior crash (Steam overlay) was caused by EAT patching combined with
      // GetProcAddress hook, not by the hook alone. Without EAT patching,
      // oGetProcAddress returns the real function address, and the system-DLL
      // bypass in DetourGetProcAddress correctly returns the real function
      // to system callers.
      FFXHook::RegisterDynamicHooks();
      IATHook::InitializeGetProcAddressHook();
    }
  }


  if (g_hCheckHooksEvent)
    SetEvent(g_hCheckHooksEvent);
}

static void ArmManualReflexQueryHookIfConfigured(const char *source) {
  if (!g_ReflexLimiter.IsManualLimiterConfiguredOrActive())
    return;

  // Games can cache NvAPI function pointers long before CE's first Present.
  // Arm the filtered QueryInterface path as soon as config/shared-memory state
  // proves manual Reflex mode is wanted, while keeping the existing caller
  // filter inside the Reflex limiter.
  g_ReflexLimiter.SetManualLimiterConfiguredOrActive(true);

  static std::atomic<bool> s_loggedEarlyManualReflexArm{false};
  if (!s_loggedEarlyManualReflexArm.exchange(true, std::memory_order_acq_rel)) {
    HookLogImportant(
        "ReflexLimiter: Early filtered nvapi_QueryInterface hook armed from %s "
        "manual Reflex configuration",
        source && source[0] ? source : "current");
  }
}

// Centralized Hook Detection Logic (Executed by HookThread)
void CheckAndInstallHooks() {
  std::lock_guard<std::mutex> lock(g_HookMutex);

  const bool dxvkD3D11WrapperLoaded = IsDXVKD3D11WrapperLoaded();
  const bool dxvkD3D9WrapperLoaded = IsDXVKD3D9WrapperLoaded();

  // CRITICAL FIX: Skip all D3D/DXGI hooks when Vulkan is the primary API.
  // Vulkan games using WSI-to-DXGI mapping can freeze if we hook DXGI/D3D.
  // The Vulkan layer (VK_LAYER_CE_overlay) handles overlay for Vulkan apps.
  //
  // NOTE: Many games load vulkan-1.dll as a transitive dependency without using
  // Vulkan for rendering (e.g., games with Vulkan support flags but running DX12).
  // Only treat Vulkan as the primary renderer if:
  //   - vulkan-1.dll is loaded, AND
  //   - No D3D usage evidence is present
  // D3D usage evidence includes D3D11/12 device creation and legacy D3D module
  // presence (DX9/DX8/DDraw), then we must not stay locked in Vulkan mode.
  static bool s_checkedForVulkan = false;
  static bool s_vulkanActive = false;
  if (!s_checkedForVulkan || s_vulkanActive) {
    HMODULE hVulkan = GetModuleHandleW(L"vulkan-1.dll");
    bool vulkanLayerOwned = false;
    if (g_pSharedMem) {
      uint64_t lastVulkan = g_pSharedMem->runtimeState.vulkanPresentTick.load(std::memory_order_acquire);
      vulkanLayerOwned = g_pSharedMem->runtimeState.vulkanLayerActive.load(std::memory_order_acquire) ||
                         (lastVulkan != 0 && (GetTickCount64() - lastVulkan) < 2000);
    }
    bool legacyD3DLoaded = (GetModuleHandleA("d3d9.dll") != nullptr) ||
                           (GetModuleHandleA("d3d8.dll") != nullptr) ||
                           (GetModuleHandleA("ddraw.dll") != nullptr);
    // DXVK's d3d11.dll is only a D3D front-end over Vulkan. Treat it as Vulkan-backed
    // so the implicit Vulkan layer can take ownership once the loader finishes startup.
    bool d3dDeviceCreated = false;
    if (dxvkD3D11WrapperLoaded) {
      d3dDeviceCreated = WasD3D12DeviceCreated();
    } else {
      // Also treat d3d12.dll/d3d11.dll presence as D3D evidence — UE5 loads
      // vulkan-1.dll even for DX12 games, and our D3D12CreateDevice wrapper may
      // not be installed yet if d3d12.dll loaded after our initial IAT scan.
      bool d3dDllPresent = (GetModuleHandleA("d3d12.dll") != nullptr) ||
                           (GetModuleHandleA("d3d11.dll") != nullptr);
      d3dDeviceCreated = WasD3D12DeviceCreated() || d3dDllPresent ||
                         WasD3D11Or10DeviceCreated() || legacyD3DLoaded;
    }
    if (vulkanLayerOwned) {
      if (!s_vulkanActive) {
        EarlyLog("CheckAndInstallHooks: Vulkan layer ownership established, skipping D3D/DXGI hooks");
      }
      s_vulkanActive = true;
      s_checkedForVulkan = true;
    } else if (hVulkan && !d3dDeviceCreated) {
      if (!s_vulkanActive) {
        EarlyLog("CheckAndInstallHooks: Vulkan detected (vulkan-1.dll, no D3D usage evidence), "
                 "skipping D3D/DXGI hooks");
      }
      s_vulkanActive = true;
      s_checkedForVulkan = true;
    } else if (d3dDeviceCreated) {
      // D3D usage evidence present — even if vulkan-1.dll is present, use D3D hooks
      if (s_vulkanActive) {
        EarlyLog("CheckAndInstallHooks: D3D evidence appeared after Vulkan detection; enabling "
                 "D3D/DXGI hooks");
      }
      s_vulkanActive = false;
      s_checkedForVulkan = true;
    }
    // If neither Vulkan nor D3D evidence is present yet, don't lock in.
  }

  // WRAPPER-ONLY ARCHITECTURE: We use IAT-patched wrapper hooks for ALL games.
  // This is more robust than vtable hooks and avoids Steam overlay recursion
  // issues. The wrappers (CWrapDXGISwapChain, CWrapDXGIFactory2) handle all
  // interception. NOTE: InitializeWrapperHooks is skipped for Vulkan to prevent
  // DXGI interference
  if (!s_vulkanActive && !dxvkD3D11WrapperLoaded) {
    InitializeWrapperHooks();
  } else if (!s_vulkanActive && dxvkD3D11WrapperLoaded) {
    static bool s_loggedDXVKD3D11WrapperDeferral = false;
    if (!s_loggedDXVKD3D11WrapperDeferral) {
      EarlyLog("CheckAndInstallHooks: DXVK d3d11 detected, deferring DXGI/D3D wrapper init to Vulkan layer");
      s_loggedDXVKD3D11WrapperDeferral = true;
    }
  }

  // DX12: Only initialize the hook instance for state tracking and
  // ExecuteCommandLists hooking. We do NOT install DXGI vtable hooks anymore -
  // wrappers handle Present/ResizeBuffers. NOTE: Skip for Vulkan games to
  // prevent DXGI interference
  {
    HMODULE hD3D12 = GetModuleHandleA("d3d12.dll");
    const bool d3d12DeviceCreated = WasD3D12DeviceCreated();
#ifdef ENABLE_D3D12_WRAPPER
    const bool shouldInitDX12Hook = d3d12DeviceCreated;
#else
#  ifdef _WIN64
    const bool shouldInitDX12Hook = true;
#  else
    // x86: init DX12 hooks unconditionally when d3d12.dll is loaded (matching
    // 64-bit behavior). The global DXGI factory vtable hooks on
    // CreateSwapChain/CreateSwapChainForHwnd MUST be installed during DllMain
    // (synchronously) before any game code runs — otherwise the swapchain is
    // never intercepted and the overlay has no target. The earlier conditional
    // (WasD3D11Or10DeviceCreated()) was too late: it only became true after the
    // HookThread retry loop (1s tick), by which point the swapchain already
    // existed. The third-party overlay interference concern (nvspcap.dll) is
    // handled inside DX12Hook::Init() which defers only the eager Present hook
    // install, NOT InstallGlobalVTableHooks().
    const bool shouldInitDX12Hook = true;
#  endif
#endif
    const bool suppressDX12HookForDXVK = dxvkD3D11WrapperLoaded && !d3d12DeviceCreated;
    if (!s_vulkanActive && !g_DX12Hook && hD3D12 && shouldInitDX12Hook && !suppressDX12HookForDXVK) {
      HookLogImportant(
          "Detected D3D12 runtime presence. Initializing DX12 hook instance... "
          "(deviceCreated=%d)",
          d3d12DeviceCreated ? 1 : 0);

      // STATIC DESTRUCTOR FIX: Dynamically allocate the hook instance
      if (!g_dx12HookInstance) {
        g_dx12HookInstance = new DX12Hook();
      }
      g_DX12Hook = g_dx12HookInstance;

      // In no-wrapper builds WasD3D12DeviceCreated() never flips true, so late
      // injection would otherwise skip DX12Hook::Init() entirely and never arm
      // the Present/swapchain recovery path.
      g_DX12Hook->Init();
      HookLogImportant("DX12 hook instance ready");
    } else if (!s_vulkanActive && !g_DX12Hook && hD3D12 && suppressDX12HookForDXVK) {
      static bool s_loggedDX12SkipForDXVK = false;
      if (!s_loggedDX12SkipForDXVK) {
        HookLog("DX12 hook init deferred: d3d12.dll is present, but DXVK d3d11 owns rendering");
        s_loggedDX12SkipForDXVK = true;
      }
    } else if (!s_vulkanActive && !g_DX12Hook && hD3D12 && !shouldInitDX12Hook) {
      static bool s_loggedWaitingForRealDX12Use = false;
      if (!s_loggedWaitingForRealDX12Use) {
#  ifdef _WIN64
        HookLog("DX12 hook init deferred: waiting for confirmed D3D12 device creation");
#  else
        HookLog("DX12 hook init skipped (x86): d3d12.dll present but no D3D12 device created");
#  endif
        s_loggedWaitingForRealDX12Use = true;
      }
    } else if (!s_vulkanActive && !g_DX12Hook && !hD3D12) {
      static bool s_loggedNoD3D12Yet = false;
      if (!s_loggedNoD3D12Yet) {
        HookLog("DX12 hook init deferred: d3d12.dll not loaded yet");
        s_loggedNoD3D12Yet = true;
      }
    }
  }

  // IMPORTANT: Install DX11 hooks based on ACTUAL API usage, not just DLL
  // presence. On modern Windows, d3d12.dll is often loaded by the D3D11 runtime
  // (D3D11On12), even for pure DX11 applications. The old check
  // (!GetModuleHandleA("d3d12.dll")) was incorrectly preventing DX11 hooks from
  // being installed in DX11 apps.
  //
  // New logic: Install DX11 hooks if:
  //   1. D3D11/D3D10 DLLs are present, AND
  //   2. Either D3D11/D3D10 device creation was actually called, OR
  //      D3D12CreateDevice was NOT actually called (so it's not a real DX12

  //      app)
  bool d3d11Or10DllPresent =
      (!dxvkD3D11WrapperLoaded &&
       (GetModuleHandleA("d3d11.dll") || GetModuleHandleA("d3d10.dll") ||
        GetModuleHandleA("d3d10_1.dll")));
  bool legacyD3DLoaded = (GetModuleHandleA("d3d9.dll") != nullptr) ||
                         (GetModuleHandleA("d3d8.dll") != nullptr) ||
                         (GetModuleHandleA("ddraw.dll") != nullptr);
  bool d3d11Or10DeviceCreated = !dxvkD3D11WrapperLoaded && WasD3D11Or10DeviceCreated();
  bool d3d12DeviceCreated = WasD3D12DeviceCreated();

  // Log third-party overlay presence for diagnostics
  {
    HMODULE hGameoverlay = GetModuleHandleA("gameoverlayrenderer.dll");
    if (hGameoverlay) {
      char overlayPath[MAX_PATH] = {};
      GetModuleFileNameA(hGameoverlay, overlayPath, MAX_PATH);
      HookLog("Third-party overlay detected: gameoverlayrenderer.dll (%s)", overlayPath);
    }
  }

  // NOTE: Skip D3D11 hooks for Vulkan games to prevent DXGI interference
  // Also avoid DX11 hook install in legacy D3D processes unless actual
  // D3D11/D3D10 device creation was observed (prevents DX9 interop false
  // positives when recording starts).
  //
  // Many DX11 games load d3d9.dll as a transitive dependency (for audio
  // codecs, Windows version checks, etc.) without using it for rendering.
  // The old (!d3d12DeviceCreated && !legacyD3DLoaded) fallback was too
  // conservative — it prevented DX11 hook installation in DX11 games that
  // happened to have d3d9.dll loaded.  Now we install DX11 hooks whenever
  // d3d11/d3d10 is present and D3D12 was NOT actually used (legacyD3DLoaded
  // is no longer a blocker: a true DX9-only game never hits DX11 hook paths
  // because it never calls D3D11 functions).
  {
    static int s_dx11CheckCount = 0;
    ++s_dx11CheckCount;
    bool dx11CondVulkanOk = !s_vulkanActive;
    bool dx11CondNoHookYet  = !g_DX11Hook;
    bool dx11CondDllPresent = d3d11Or10DllPresent;
    bool dx11CondDeviceOk   = (d3d11Or10DeviceCreated || !d3d12DeviceCreated);
    bool dx11CondAll        = dx11CondVulkanOk && dx11CondNoHookYet && dx11CondDllPresent && dx11CondDeviceOk;
    if (s_dx11CheckCount <= 5 || dx11CondAll) {
      HookLogImportant(
          "DX11 check #%d: vulkan=%d noHook=%d dllPresent=%d device=%d legacy=%d "
          "d3d12Created=%d => %s",
          s_dx11CheckCount, s_vulkanActive ? 1 : 0, g_DX11Hook ? 1 : 0,
          d3d11Or10DllPresent ? 1 : 0, d3d11Or10DeviceCreated ? 1 : 0,
          legacyD3DLoaded ? 1 : 0, d3d12DeviceCreated ? 1 : 0,
          dx11CondAll ? "INSTALL" : "skip");
    }
    if (dx11CondAll) {
      HookLog("Detected D3D10/11. Installing hooks... (D3D11/10 API called: %d, "
              "D3D12 API called: %d, LegacyD3D loaded: %d)",
              d3d11Or10DeviceCreated ? 1 : 0, d3d12DeviceCreated ? 1 : 0,
              legacyD3DLoaded ? 1 : 0);
      g_DX11Hook = new DX11Hook();
      LARGE_INTEGER _t1, _t2, _freq;
      QueryPerformanceFrequency(&_freq);
      QueryPerformanceCounter(&_t1);
      g_DX11Hook->Init();
      QueryPerformanceCounter(&_t2);
      // NOLINTNEXTLINE(bugprone-narrowing-conversions) - intentional narrowing; value is range-bounded by the surrounding API/geometry contract
      double _initMs = (double)(_t2.QuadPart - _t1.QuadPart) * 1000.0 / _freq.QuadPart;
      HookLog("D3D10/11 hooks installed (init=%.1f ms)", _initMs);
    }
  }

  // For other APIs, skip if D3D12 was actually used (not just loaded).
  // d3d12.dll can be loaded by D3D11On12 even in non-DX12 apps.
  // We use the actual device creation flag instead of just DLL presence.
  bool dx12ActuallyUsed = WasD3D12DeviceCreated();

  // NOTE: Skip D3D9 hooks for Vulkan games, except DXVK D3D9. That path still
  // needs the DX9 hook for game-thread Present pacing and overlay integration
  // while Vulkan remains the primary capture path.
  // Also skip D3D9 hooks for DX11 games — d3d9.dll is often a transitive system
  // dependency (audio codecs, Windows version checks) in DX11 titles.
  const bool dx11DllLoaded = GetModuleHandleA("d3d11.dll") != nullptr;
  if ((!s_vulkanActive || dxvkD3D9WrapperLoaded) && !g_DX9Hook && !dx12ActuallyUsed && !dx11DllLoaded &&
      GetModuleHandleA("d3d9.dll")) {
    EarlyLog(
        "DX9 Hook Check: Installing DX9 hooks (d3d9.dll loaded, vulkanActive=%d, dx12Used=%d, dxvkD3D9=%d)",
        s_vulkanActive ? 1 : 0, dx12ActuallyUsed ? 1 : 0, dxvkD3D9WrapperLoaded ? 1 : 0);
    HookLog("Detected d3d9.dll. Installing DX9 hooks...");
    g_DX9Hook = new DX9Hook();
    LARGE_INTEGER _t1, _t2, _freq;
    QueryPerformanceFrequency(&_freq);
    QueryPerformanceCounter(&_t1);
    g_DX9Hook->Init();
    QueryPerformanceCounter(&_t2);
    // NOLINTNEXTLINE(bugprone-narrowing-conversions) - intentional narrowing; value is range-bounded by the surrounding API/geometry contract
    double _initMs = (double)(_t2.QuadPart - _t1.QuadPart) * 1000.0 / _freq.QuadPart;
    HookLog("DX9 hooks installed (hook ptr=%p, init=%.1f ms)", (void*)g_DX9Hook, _initMs);
  } else if (!g_DX9Hook && GetModuleHandleA("d3d9.dll")) {
    EarlyLog("DX9 Hook Check: Skipping DX9 hooks (vulkanActive=%d, dx12Used=%d, dx11Loaded=%d, dxvkD3D9=%d)",
             s_vulkanActive ? 1 : 0, dx12ActuallyUsed ? 1 : 0, dx11DllLoaded ? 1 : 0, dxvkD3D9WrapperLoaded ? 1 : 0);
  }

  // DirectDraw titles can still load or probe D3D12 through DXGI/driver helper
  // components. That must not suppress the actual DirectDraw hook path.
  // Skip DirectDraw hooks when the Vulkan layer owns presentation. In the DXVK
  // D3D9 case the DX9 hook stays active, and synthesizing DirectDraw objects on
  // our worker thread can recurse into external overlays and crash.
  // Also skip DDraw hooks when a higher-level D3D API (d3d9, d3d8) is present,
  // because ddraw.dll is often a transitive system dependency and bootstrapping
  // DDraw (which internally creates a D3D9 device) can crash third-party overlays
  // that have already hooked Direct3DCreate9 (see DDrawHook::Init for details).
  if (!s_vulkanActive && !g_DDrawHook && GetModuleHandleA("ddraw.dll") &&
      !GetModuleHandleA("d3d9.dll") && !GetModuleHandleA("d3d8.dll")) {
    HookLog("Detected ddraw.dll. Installing DirectDraw hooks... (dx12Used=%d)",
            dx12ActuallyUsed ? 1 : 0);
    g_DDrawHook = new DDrawHook();
    LARGE_INTEGER _t1, _t2, _freq;
    QueryPerformanceFrequency(&_freq);
    QueryPerformanceCounter(&_t1);
    g_DDrawHook->Init();
    // NOLINTNEXTLINE(bugprone-narrowing-conversions) - intentional narrowing; value is range-bounded by the surrounding API/geometry contract
    QueryPerformanceCounter(&_t2);
    // NOLINTNEXTLINE(bugprone-narrowing-conversions) - intentional narrowing; value is range-bounded by the surrounding API/geometry contract
    double _initMs = (double)(_t2.QuadPart - _t1.QuadPart) * 1000.0 / _freq.QuadPart;
    HookLog("DDraw hooks installed (init=%.1f ms)", _initMs);
  } else if (!s_vulkanActive && !g_DDrawHook && GetModuleHandleA("ddraw.dll")) {
    HookLog("DDraw hooks skipped (higher-level D3D API present: d3d9=%d d3d8=%d)",
            GetModuleHandleA("d3d9.dll") ? 1 : 0,
            GetModuleHandleA("d3d8.dll") ? 1 : 0);
  }

  if (!g_DX8Hook && !dx12ActuallyUsed && GetModuleHandleA("d3d8.dll")) {
    HookLog("Detected d3d8.dll. Installing DX8 hooks...");
    g_DX8Hook = new DX8Hook();
    LARGE_INTEGER _t1, _t2, _freq;
    QueryPerformanceFrequency(&_freq);
    QueryPerformanceCounter(&_t1);
    // NOLINTNEXTLINE(bugprone-narrowing-conversions) - intentional narrowing; value is range-bounded by the surrounding API/geometry contract
    g_DX8Hook->Init();
    // NOLINTNEXTLINE(bugprone-narrowing-conversions) - intentional narrowing; value is range-bounded by the surrounding API/geometry contract
    QueryPerformanceCounter(&_t2);
    double _initMs = (double)(_t2.QuadPart - _t1.QuadPart) * 1000.0 / _freq.QuadPart;  // NOLINT(bugprone-narrowing-conversions)
    HookLog("DX8 hooks installed (init=%.1f ms)", _initMs);
  }

  if (!g_OpenGLHook && !dx12ActuallyUsed && GetModuleHandleA("opengl32.dll")) {
    HookLog("Detected opengl32.dll. Installing OpenGL hooks...");
    g_OpenGLHook = new OpenGLHook();
    LARGE_INTEGER _t1, _t2, _freq;
    QueryPerformanceFrequency(&_freq);
    // NOLINTNEXTLINE(bugprone-narrowing-conversions) - intentional narrowing; value is range-bounded by the surrounding API/geometry contract
    QueryPerformanceCounter(&_t1);
    // NOLINTNEXTLINE(bugprone-narrowing-conversions) - intentional narrowing; value is range-bounded by the surrounding API/geometry contract
    g_OpenGLHook->Init();
    QueryPerformanceCounter(&_t2);
    double _initMs = (double)(_t2.QuadPart - _t1.QuadPart) * 1000.0 / _freq.QuadPart;  // NOLINT(bugprone-narrowing-conversions)
    HookLog("OpenGL hooks installed (init=%.1f ms)", _initMs);
  }

  // Vulkan is handled by VK_LAYER_CE_overlay (ICD layer)
  // No hooking needed - the layer is loaded automatically by the Vulkan loader

  // FFX hooks for FSR FG detection
  // These hooks intercept ffxCreateContext/ffxDestroyContext to detect FSR FG
  // activation. Now safe with dedicated overlay queue - no race conditions with
  // game queue.
  FFXHook::Init();
  StreamlineHook::Init();

  // Install NVNGX and D3DKMT hooks for all games (injection delay prevents
  // D3D12 init crashes)
  {
    // Install NGX hooks if DLL is present
    NVNGXHook::Get().Install();

    // Install D3DKMT hooks for VRAM override (universal solution)
    // This hooks kernel-mode driver calls that games use to query VRAM
    // independently of DXGI (a common VRAM-reporting override technique)
    static bool s_D3DKMTHooksInstalled = false;
    if (!s_D3DKMTHooksInstalled) {
      if (D3DKMTHooks::Install()) {
        s_D3DKMTHooksInstalled = true;
        EarlyLog("D3DKMT hooks installed for VRAM override");
      }
    }
  }
}

DWORD WINAPI HookThread(LPVOID lpParam) {
  g_HookThreadRunning = true;

  // HookThread continues normally for all games (injection delay prevents D3D12
  // init crashes)

  // Load Local Config (to support per-app overrides) EARLY
  {
    char dllPath[MAX_PATH];
    GetModuleFileNameA(g_hModule, dllPath, MAX_PATH);
    std::string pathString = dllPath;
    std::string dir = pathString.substr(0, pathString.find_last_of("\\/"));
    std::string configPath = dir + "\\config.ini";

    EnsureLocalConfigAllocated();
    LoadConfig(configPath, *g_pLocalConfig);
    // Prime the graphics override state immediately
    GetActiveGraphicsConfig();
    ArmManualReflexQueryHookIfConfigured("config.ini");

    // Load wrapper DLLs for all graphics APIs
    {
      // DEFERRED LOADING: Load wrapper DLLs here instead of DllMain
      // Loading DLLs in DllMain can cause loader lock deadlocks.
      // HookThread runs after DllMain returns, so it's safe to call LoadLibrary
      // here.
      [[maybe_unused]] bool hasGraphicsAPI = (GetModuleHandleA("d3d12.dll") != NULL ||
                             GetModuleHandleA("d3d11.dll") != NULL ||
                             GetModuleHandleA("d3d10.dll") != NULL ||
                             GetModuleHandleA("d3d9.dll") != NULL);

#ifdef ENABLE_D3D12_WRAPPER
      if (hasGraphicsAPI) {
#ifdef _WIN64
        std::string wrapperDll = dir + "\\d3d12_wrappers.dll";
#else
        std::string wrapperDll = dir + "\\d3d12_wrappers_x86.dll";
#endif
        UINT oldMode = SetErrorMode(SEM_FAILCRITICALERRORS);
        HMODULE hWrapper = LoadLibraryA(wrapperDll.c_str());
        SetErrorMode(oldMode);

        if (!hWrapper) {
          EarlyLog("HookThread: Failed to load wrapper DLL from %s, Err=%d",
                   wrapperDll.c_str(), GetLastError());
        } else {
          EarlyLog("HookThread: Loaded wrapper DLL at %p", hWrapper);
        }
      }
#endif // ENABLE_D3D12_WRAPPER
    }

    // Seed third-party-overlay detection and register the loader-safe DLL load/unload
    // notification. Done here (HookThread, after DllMain) so the seed's one-time loader walk
    // and the registration are off both the DllMain loader lock and the Present hot path.
    InitializeThirdPartyOverlayDetection();

    // perf_metrics_logging now folds into debug_logging so a single switch
    // controls all hook-side diagnostics.
    if (g_pLocalConfig && IsTraceLoggingEnabled(g_pLocalConfig->logLevel)) {
      // Read session-specific logs path from DiscoveryInfo (set by inject process)
      std::string sessionLogsDir;
      HANDLE hDisc = OpenFileMappingW(FILE_MAP_READ, FALSE, SHARED_MEM_DISCOVERY);
      if (hDisc) {
        DiscoveryInfo *pDisc = (DiscoveryInfo *)MapViewOfFile(
            hDisc, FILE_MAP_READ, 0, 0, sizeof(DiscoveryInfo));
        if (ValidateDiscoveryInfo(pDisc) && pDisc->logsPath[0]) {
          sessionLogsDir = pDisc->logsPath;
        }
        if (pDisc) UnmapViewOfFile(pDisc);
        CloseHandle(hDisc);
      }

      // Fall back to {dllDir}\logs if DiscoveryInfo unavailable
      if (sessionLogsDir.empty())
        sessionLogsDir = dir + "\\logs";

      CreateDirectoryA(sessionLogsDir.c_str(), NULL);

      // Update crash dump directory to session folder
      SetCrashDumpDirectory(sessionLogsDir);

      char perfLogPath[MAX_PATH];
      snprintf(perfLogPath, sizeof(perfLogPath),
               "%s\\perf_metrics_%lu.csv", sessionLogsDir.c_str(),
               GetCurrentProcessId());
      PerfLogger::Get().Init(perfLogPath);
    }
  }

  EarlyLog("HookThread: Started (PID=%d)", GetCurrentProcessId());

  // Create Event for Async Hook Checks
  g_hCheckHooksEvent = CreateEvent(NULL, FALSE, FALSE, NULL); // Auto-reset
  if (!g_hCheckHooksEvent) {
    // Logic without event...
  }

  // --- BLACKLISTED PROCESSES ---
  if (g_ProcessCategory == ProcessCategory::Blacklisted) {
    CloseCheckHooksEvent();
    FreeLibraryAndExitThread(g_hModule, 0);
    return 0;
  }

  // --- LAUNCHERS ---
  if (g_ProcessCategory == ProcessCategory::Launcher) {
    // launchers only need CreateProcess hooks. No IPC, no graphics.
    // Use IAT patching
    OriginalCreateProcessA.store((CreateProcessA_t)GetProcAddress(
        GetModuleHandleA("kernel32.dll"), "CreateProcessA"), std::memory_order_release);
    OriginalCreateProcessW.store((CreateProcessW_t)GetProcAddress(
        GetModuleHandleA("kernel32.dll"), "CreateProcessW"), std::memory_order_release);

    void *dummy;
    IATHook::PatchIATAllModules("kernel32.dll", "CreateProcessA",
                                (void *)&HookedCreateProcessA, &dummy);
    IATHook::PatchIATAllModules("kernel32.dll", "CreateProcessW",
                                (void *)&HookedCreateProcessW, &dummy);

    // launchers don't have an IPC loop, they just stay alive to hook child
    // processes We still need to unload eventually if we want perfect cleanup,
    // but for launchers it's safer to just stay loaded until process exit
    // to avoid missing a CreateProcess call during transition.
    // However, we need to check for shutdown signal to allow DLL unload.
    // Use 100ms instead of 1000ms to respond quickly to shutdown.
    while (!HookIsShuttingDown()) {
      Sleep(100);
    }
    return 0;
  }

  // POTENTIAL GAMES
  EarlyLog("HookThread: Potential game detected. Watchdog started.");

  // Init IPC loop
  g_IPC = new IPCClient();

  if (g_isSkippedProcess) {
    // EarlyLog removed from here to prevent file locks in system processes
    while (true) {
      bool hasGraphicsAPI = (GetModuleHandleA("d3d12.dll") != NULL ||
                             GetModuleHandleA("d3d11.dll") != NULL ||
                             GetModuleHandleA("d3d10.dll") != NULL ||
                             GetModuleHandleA("d3d9.dll") != NULL ||
                             GetModuleHandleA("d3d8.dll") != NULL ||
                             GetModuleHandleA("ddraw.dll") != NULL ||
                             GetModuleHandleA("opengl32.dll") != NULL ||
                             GetModuleHandleA("vulkan-1.dll") != NULL);

      if (hasGraphicsAPI) {
        EarlyLog("HookThread: [%s] Late graphics API detection! Transitioning "
                 "to game mode.",
                 g_ProcessName);
        g_isSkippedProcess = false;
        break;
      }

      if (g_IPC->Connect()) {
        Sleep(1000); // 1s is aggressive enough without being a CPU hog/bomb
      } else {
        // Engine not found or closed - time to exit
        CloseCheckHooksEvent();
        FreeLibraryAndExitThread(g_hModule, 0);
        return 0;
      }
      Sleep(1000);
    }
  }

  EarlyLog("HookThread: [%s] IPCClient created, attempting connect...",
           g_ProcessName);
  if (g_IPC->Connect()) {
    EarlyLog("HookThread: IPC Connected successfully!");
    HookLog("IPC Connected successfully!");

    if (g_IPC->GetSharedMem()) {
      g_pSharedMem = g_IPC->GetSharedMem();
      g_pSharedMem->SetSourcePid(GetCurrentProcessId());
      ArmManualReflexQueryHookIfConfigured("shared memory");
    }

    // Initialize HookContext and sync with legacy globals
    // This bridges the old global-based approach with the new centralized
    // context
    ce::CreateHookContext();
    if (auto *ctx = ce::GetHookContext()) {
      ctx->hookModule = g_hModule;
      ce::SyncWithLegacyGlobals();

      // Transition lifecycle to Connected state
      ctx->hookLifecycle.TransitionTo(ce::HookState::Connected);
      EarlyLog("HookThread: HookContext initialized and synced");
    }
  } else {
    EarlyLog("HookThread: IPC Connection FAILED!");
    HookLog("IPC Connection FAILED!");
  }

  // Use IAT patching for kernel32/advapi32 hooks
  EarlyLog("HookThread: Initializing IAT-based kernel32 hooks...");

  // Install LoadLibrary and CreateProcess hooks via IAT patching
  // Use temporary plain pointers for IAT hook init, then store atomically
  HookLog("Installing LoadLibrary/CreateProcess hooks via IAT patching...");

  LoadLibraryA_t tmpLoadLibraryA = nullptr;
  LoadLibraryW_t tmpLoadLibraryW = nullptr;
  LoadLibraryExA_t tmpLoadLibraryExA = nullptr;
  LoadLibraryExW_t tmpLoadLibraryExW = nullptr;
  CreateProcessA_t tmpCreateProcessA = nullptr;
  CreateProcessW_t tmpCreateProcessW = nullptr;

  IATHook::InitializeKernel32Hooks(
      (void *)&HookedLoadLibraryA, (void **)&tmpLoadLibraryA,
      (void *)&HookedLoadLibraryW, (void **)&tmpLoadLibraryW,
      (void *)&HookedLoadLibraryExA, (void **)&tmpLoadLibraryExA,
      (void *)&HookedLoadLibraryExW, (void **)&tmpLoadLibraryExW,
      (void *)&HookedCreateProcessA, (void **)&tmpCreateProcessA,
      (void *)&HookedCreateProcessW, (void **)&tmpCreateProcessW);

  // Store atomically so other threads see consistent values
  OriginalLoadLibraryA.store(tmpLoadLibraryA, std::memory_order_release);
  OriginalLoadLibraryW.store(tmpLoadLibraryW, std::memory_order_release);
  OriginalLoadLibraryExA.store(tmpLoadLibraryExA, std::memory_order_release);
  OriginalLoadLibraryExW.store(tmpLoadLibraryExW, std::memory_order_release);
  OriginalCreateProcessA.store(tmpCreateProcessA, std::memory_order_release);
  OriginalCreateProcessW.store(tmpCreateProcessW, std::memory_order_release);

  // GTA and some middleware can terminate with fail-fast style status codes
  // before VEH/UEF crash filters get control. Keep this narrow and passive:
  // one CE-owned dump for current-process fatal exits, then forward.
  FFXHook::RegisterDynamicHooks();
  IATHook::InitializeGetProcAddressHook();
  TryInstallFatalTerminationDumpHooks();

  // Install RegQueryValueExW for DLSS Debug Overlay
  if (GetModuleHandleA("advapi32.dll")) {
    HookLog("Installing RegQueryValueExW hook via IAT patching...");
    IATHook::InitializeAdvapi32Hooks((void *)&HookedRegQueryValueExW,
                                     (void **)&OriginalRegQueryValueExW);
  } else {
    HookLog("advapi32.dll not loaded yet - skipping RegQueryValueExW hook");
  }

  // Install the low-level loader hook for DLL redirection and early module-load
  // observation. GTA Enhanced can bring up the official FFX runtime through a
  // path that reaches CE's periodic scan only after ffxConfigure has already
  // been cached, which prevents the native FSR present-callback bridge from
  // arming.
  if (NeedsLoaderRedirectionHook() || NeedsLowLevelModuleLoadObservationHook()) {
    if (!OriginalLdrLoadDll.load(std::memory_order_acquire)) {
      if (HMODULE hNtdll = GetModuleHandleA("ntdll.dll")) {
        if (void *pLdrLoadDll = (void *)GetProcAddress(hNtdll, "LdrLoadDll")) {
          void *trampoline = nullptr;
          if (InlineHook::Install(pLdrLoadDll, (void *)&HookedLdrLoadDll,
                                  &trampoline)) {
            OriginalLdrLoadDll.store((LdrLoadDll_t)trampoline, std::memory_order_release);
            HookLogImportant("Installed LdrLoadDll hook for module-load observation and optional DLL redirection");
          } else {
            HookLog("Failed to install LdrLoadDll hook");
          }
        }
      }
    }
  } else {
    HookLog("Skipping LdrLoadDll hook (no DLL redirection overrides configured)");
  }

  HookLogImportant("HookThread: IAT hooks installed");

  // Initial Check
  CheckAndInstallHooks();

  HookLogImportant("HookThread: All hooks installed, entering exit monitor loop");

  // Monitor Loop - Waits for Event OR Timeout (for Exit Checks)
  DWORD lastPeriodicHookCheck = GetTickCount();
  while (true) {
    // Wait for event (signaled by LoadLibrary) or timeout (100ms)
    DWORD waitResult = WAIT_TIMEOUT;
    if (g_hCheckHooksEvent) {
      waitResult = WaitForSingleObject(g_hCheckHooksEvent, 100);
    } else {
      Sleep(100);
    }

    DWORD now = GetTickCount();

    // Periodically update active graphics config state
    // This ensures g_GraphicsOverridesActive is updated even if no hooks are
    // calling it yet
    GetActiveGraphicsConfig();

    // Process deferred releases (D3D11) on background thread
    // This prevents render thread stalls when destroying capture resources
    if (g_DX11Hook)
      g_DX11Hook->ProcessDeferredReleases();

    // --- UE5 Enforce RR ---
    static DWORD s_LastRRCheck = 0;
    if (now - s_LastRRCheck > 2000) {
      s_LastRRCheck = now;
      UE5::EnforceRR();
    }

    bool periodicHookCheckDue = (now - lastPeriodicHookCheck) >= 1000;
    if (waitResult == WAIT_OBJECT_0 || periodicHookCheckDue) {
      if (periodicHookCheckDue) {
        lastPeriodicHookCheck = now;
      }
      // Event signaled or periodic tick - run detection
      CheckAndInstallHooks();
    }

    // Check for recording state changes
    static bool s_WasRecording = false;
    bool isRecording = false;
    if (g_IPC && g_IPC->IsRecording()) {
      isRecording = true;
    }

    if (isRecording != s_WasRecording) {
      s_WasRecording = isRecording;
      CaptureManager::Get().SetCaptureEnabled(isRecording);
      HookLog("Capture state changed: %s",
              isRecording ? "ENABLED" : "DISABLED");
    }

    // Always check for exit/IPC maintenance on every loop iteration
    bool shouldExit = false;
    uint32_t hostPID = 0;

    if (g_IPC && g_IPC->GetSharedMem()) {
      shouldExit = g_IPC->GetSharedMem()->GetRequestExit();
      hostPID = g_IPC->GetSharedMem()->GetHostPID();
    }

    if (shouldExit) {
      EarlyLog("HookThread: Exit requested by host");
      HookLog("Exit requested by host");
      break;
    }

    if (hostPID != 0) {
      HANDLE hHost = OpenProcess(SYNCHRONIZE, FALSE, hostPID);
      if (hHost) {
        DWORD waitResultHost = WaitForSingleObject(hHost, 0); // Immediate check
        CloseHandle(hHost);
        if (waitResultHost == WAIT_OBJECT_0) {
          EarlyLog("HookThread: Host process died");
          HookLog("Host process died. Cleaning up...");
          break;
        }
      } else {
        if (g_IPC->GetSharedMem()) {
          EarlyLog("HookThread: Can't open host process, assuming dead");
          HookLog("Host process inaccessible. Exiting...");
          break;
        }
      }
    } else {
      // Reconnect logic
      if (g_IPC) {
        if (g_IPC->Connect()) {
          EarlyLog("HookThread: Reconnected to new host");
          HookLog("IPC Reconnected to new captureengine instance!");
          CheckAndInstallHooks();
        } else {
          // Host not found, maybe it closed?
          // Target games get a longer grace period (30s) before self-unloading
          static int missedHeartbeats = 0;
          if (++missedHeartbeats > 300) { // 30s at 100ms per loop
            HookLog("HookThread: Host lost for 30s. Self-unloading...");
            break;
          }
        }
      }
    }
  }

  // Cleanup Event
  CloseCheckHooksEvent();

  // Self-unload to release file lock when host requests exit or dies
  // This is crucial for the CBT global hook to not pin the DLL forever
  g_HookThreadRunning = false;
  FreeLibraryAndExitThread(g_hModule, 0);
  return 0;
}

// Helper for QueueUserWorkItem (requires DWORD return, LPVOID param)
static DWORD WINAPI HookThreadWrapper(LPVOID lpParam) {
  timeBeginPeriod(1);
  return HookThread(lpParam);
}

static bool isProcessWhitelistedFast(const char *name) {
  if (!name)
    return false;

  // 1. Internal Whitelist
  if (_stricmp(name, "captureengine.exe") == 0 ||
      _stricmp(name, "captureengine_x86.exe") == 0) {
    return true;
  }

  // 2. Shared Memory Whitelist Cache (Fastest & Safest)
  // Reliance on Shared Memory avoids Disk I/O in DllMain.
  HANDLE hDisc = OpenFileMappingW(FILE_MAP_READ, FALSE, SHARED_MEM_DISCOVERY);
  if (hDisc) {
    DiscoveryInfo *pDisc = (DiscoveryInfo *)MapViewOfFile(
        hDisc, FILE_MAP_READ, 0, 0, sizeof(DiscoveryInfo));
    bool found = false;
    if (pDisc) {
      if (ValidateDiscoveryInfo(pDisc)) {
        const char *p = pDisc->processWhitelist;
        const char *end =
            pDisc->processWhitelist + sizeof(pDisc->processWhitelist);

        while (p < end && *p != '\0') {
          if (_stricmp(name, p) == 0) {
            found = true;
            break;
          }
          p += strlen(p) + 1;
        }
      }
      UnmapViewOfFile(pDisc);
    }
    CloseHandle(hDisc);
    if (found)
      return true;
  }

  // Config.ini fallback removed from DllMain - safer to stay dormant if
  // CaptureEngine hasn't explicitly whitelisted via Shared Memory yet.
  return false;
}

// Helper: Identify Service Processes for Safe Unload
static bool IsServiceProcess(const char *name) {
  if (!name)
    return false;
  // These processes are safe to unload from (services, non-interactive).
  // Returning FALSE in DllMain allows the OS to unload us cleanly.
  return (
      _stricmp(name, "svchost.exe") == 0 || _stricmp(name, "lsass.exe") == 0 ||
      _stricmp(name, "services.exe") == 0 || _stricmp(name, "smss.exe") == 0 ||
      _stricmp(name, "wininit.exe") == 0 || _stricmp(name, "csrss.exe") == 0 ||
      _stricmp(name, "conhost.exe") == 0 ||
      _stricmp(name, "dllhost.exe") == 0 || // COM Surrogate
      _stricmp(name, "sihost.exe") == 0 ||
      _stricmp(name, "pwahelper.exe") == 0 ||
      _stricmp(name, "PerfWatson.exe") == 0 ||
      _stricmp(name, "DataExchangeHost.exe") == 0 ||
      _stricmp(name, "GamebarFTServer.exe") == 0 ||
      _stricmp(name, "WerFault.exe") == 0 || // Windows Error Reporting
      _stricmp(name, "ApplicationFrameHost.exe") == 0); // SpecialK lists this
}

extern "C" BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD ul_reason_for_call,
                               LPVOID lpReserved) {
  if (ul_reason_for_call == DLL_PROCESS_ATTACH) {
    // D3D12 FIX: Delayed injection in captureengine now prevents early-init
    // crashes We can proceed normally since injection happens after D3D12
    // initialization

    g_hModule = hinstDLL;
    DisableThreadLibraryCalls(hinstDLL);

    // CRASH FIX: Register an atexit handler that sets g_ProcessTerminating = true.
    // atexit runs LIFO, so a handler registered here (after global constructors)
    // runs BEFORE global destructors. This lets CachedOverlayRenderer::Shutdown()
    // and similar destructors skip GPU resource Release() calls during process
    // exit, preventing crashes in nvwgf2umx when the D3D12 device is already torn down.
    std::atexit([]() { g_ProcessTerminating.store(true, std::memory_order_release); });

    char fullPath[MAX_PATH] = {0};
    char *fileName = (char *)"unknown";
    if (GetModuleFileNameA(NULL, fullPath, MAX_PATH)) {
      char *fileLastSlash = strrchr(fullPath, '\\');
      fileName = fileLastSlash ? (fileLastSlash + 1) : fullPath;
      strncpy(g_ProcessName, fileName, sizeof(g_ProcessName) - 1);
      g_ProcessName[sizeof(g_ProcessName) - 1] = '\0';
    }

    // Get my DLL path but DO NOT log yet
    char myDllPath[MAX_PATH] = {0};
    GetModuleFileNameA(hinstDLL, myDllPath, MAX_PATH);

    // PINNING STRATEGY:
    // We MUST pin the DLL in *every* process that loads it (except our own
    // tools). Why?
    // 1. If we allow the DLL to unload (refcount=0) while the CBT hook is still
    // active
    //    globally, Windows might unload us right before or during a hook
    //    callback, causing a crash (access violation executing freed memory).
    // 2. For service/system processes, if we unload, the global hook will just
    //    re-inject us immediately, causing a high-CPU "Load-Unload-Load" loop.
    //
    // By pinning, we ensure the DLL stays dormant in memory until the process
    // exits.

    bool isOurTool = (_stricmp(fileName, "captureengine.exe") == 0 ||
                      _stricmp(fileName, "captureengine_x86.exe") == 0);

    if (!isOurTool) {
      HMODULE hPin = NULL;
      GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                             GET_MODULE_HANDLE_EX_FLAG_PIN,
                         (LPCSTR)hinstDLL, &hPin);
    }

    // Install Crash Handler immediately to catch startup crashes
    // Use session-specific logs directory from DiscoveryInfo if available,
    // otherwise fall back to {captureEngineDir}/logs.
    std::string crashDir;
    char dllPath[MAX_PATH] = {0};
    if (GetModuleFileNameA(hinstDLL, dllPath, MAX_PATH)) {
      std::filesystem::path hookPath(dllPath);
      std::filesystem::path captureEngineDir = hookPath.parent_path();
      // If we're in testapp directory, navigate to captureengine instead
      if (captureEngineDir.filename() == "testapp") {
        captureEngineDir = captureEngineDir.parent_path() / "captureengine";
      }
      // Set process name for crash logging
      SetCrashProcessName(fileName);

      // Try DiscoveryInfo for session-specific logs path
      HANDLE hDisc = OpenFileMappingW(FILE_MAP_READ, FALSE, SHARED_MEM_DISCOVERY);
      if (hDisc) {
        DiscoveryInfo *pDisc = (DiscoveryInfo *)MapViewOfFile(
            hDisc, FILE_MAP_READ, 0, 0, sizeof(DiscoveryInfo));
        if (ValidateDiscoveryInfo(pDisc) && pDisc->logsPath[0]) {
          crashDir = pDisc->logsPath;
        }
        if (pDisc) UnmapViewOfFile(pDisc);
        CloseHandle(hDisc);
      }

      if (crashDir.empty())
        crashDir = (captureEngineDir / "logs").string();
    } else {
      crashDir = ".\\logs";
    }
    CreateDirectoryA(crashDir.c_str(), NULL);
    SetCrashDumpDirectory(crashDir);

    // CRITICAL FIX: Install crash handler IMMEDIATELY for all non-service
    // processes Don't wait for whitelist check or graphics DLL detection -
    // crashes happen during early initialization before those are available
    // Install crash handler for all non-service processes
    // (Injection delay in captureengine prevents D3D12 init crashes)
    if (!IsServiceProcess(fileName)) {
      InstallCrashHandler();
      if (HMODULE hDbgHelp = GetModuleHandleA("dbghelp.dll")) {
        TryInstallMiniDumpWriteDumpHookForModule(hDbgHelp, "dbghelp.dll");
      }
      OutputDebugStringA("[CaptureHook] Crash handler installed\n");
    }

    // 1. SAFE UNLOAD: Services and non-interactive helpers
    // These processes should unload the DLL immediately and cleanly.
    if (IsServiceProcess(fileName)) {
      g_isDormant = true;
      return TRUE; // Stay loaded but inert to prevent load/unload loop
    }

    // 2. DORMANT MODE: Shell, Critical UI, and Internal processes
    // These MUST stay loaded to avoid the "Unload Loop" (repeated injection),
    // but they must remain completely inert.
    if (_stricmp(fileName, "explorer.exe") == 0 ||
        _stricmp(fileName, "dwm.exe") == 0 ||
        _stricmp(fileName, "winlogon.exe") == 0 ||
        _stricmp(fileName, "captureengine.exe") == 0 ||
        _stricmp(fileName, "captureengine_x86.exe") == 0 ||
        _stricmp(fileName, "sihost.exe") == 0 ||
        _stricmp(fileName, "SearchUI.exe") == 0 ||
        _stricmp(fileName, "ShellExperienceHost.exe") == 0 ||
        _stricmp(fileName, "DllHost.exe") == 0 ||       // COM Surrogate
        _stricmp(fileName, "RuntimeBroker.exe") == 0 || // UWP Broker
        _stricmp(fileName, "taskhostw.exe") == 0) {     // Task Host

      g_isDormant = true;
      return TRUE; // Stay loaded but totally inert
    }

    // Now it is safe to log!
    if (myDllPath[0] != '\0') {
      EarlyLog("DllMain: Loaded hook DLL from: %s", myDllPath);
    }

    // 3. WHITELIST CHECK: Fast & Inert
    // Only proceed if process is whitelisted (Internal or via Shared Memory)
    if (isProcessWhitelistedFast(fileName)) {
      // Whitelisted Game (Shared Mem OR Config - but we only use ShMem now in
      // WhitelistFast)
      EnsureLocalConfigAllocated();

      // Crash handler already installed at DLL load (line 1503)

      _putenv("FERMI_UNOPT_LOD_SPREAD=1");
      _putenv("NIAGARA_UNOPT_LOD_SPREAD=1");
      EarlyLog("DllMain: Process '%s' is a whitelisted hook target", fileName);
    } else {
      // Not whitelisted - assume blacklist
      g_ProcessCategory = ProcessCategory::Blacklisted;
      g_isDormant = true;

      // DORMANT MODE: We return TRUE to stay loaded but remain completely
      // inert. Returning FALSE (unloading) triggers a "Loader Loop" where
      // Windows continuously re-injects the CBT hook for every window event,
      // causing massive system slowdowns. By staying loaded but doing nothing
      // (no threads, no hooks), we eliminate this overhead. EarlyLog("DllMain:
      // Process '%s' Blacklisted (Dormant Mode)", fileName);
      return TRUE;
    }

    if (g_isDormant) {
      // Silent return
      return TRUE;
    }

    if (g_ProcessCategory == ProcessCategory::PotentialGame) {
      if (!(GetModuleHandleA("d3d12.dll") || GetModuleHandleA("d3d11.dll") ||
            GetModuleHandleA("d3d9.dll") || GetModuleHandleA("vulkan-1.dll") ||
            GetModuleHandleA("opengl32.dll") || GetModuleHandleA("d3d8.dll"))) {
        g_isSkippedProcess = true;
        EarlyLog(
            "DllMain: Process '%s' skipped (No Graphics API modules found)",
            fileName);
      }
    }

    if (g_ProcessCategory != ProcessCategory::InternalTool) {
      // CRITICAL: IAT patching in DllMain is SAFE because:
      // 1. It only modifies memory in already-loaded modules (no LoadLibrary)
      // 2. It doesn't acquire additional locks beyond the loader lock
      // 3. It's idempotent (safe to call multiple times)
      //
      // The actual DLL loading (d3d12_wrappers.dll) is DEFERRED to HookThread
      // to avoid loader lock deadlocks - see HookThread's "DEFERRED LOADING"
      // section.
      bool hasGraphicsAPI = (GetModuleHandleA("d3d12.dll") != NULL ||
                             GetModuleHandleA("d3d11.dll") != NULL ||
                             GetModuleHandleA("d3d10.dll") != NULL ||
                             GetModuleHandleA("d3d9.dll") != NULL ||
                             GetModuleHandleA("vulkan-1.dll") != NULL ||
                             GetModuleHandleA("opengl32.dll") != NULL ||
                             GetModuleHandleA("d3d8.dll") != NULL ||
                             GetModuleHandleA("ddraw.dll") != NULL);

      // Initialize hooks for all graphics APIs (injection delay prevents D3D12
      // init crashes)
      if (hasGraphicsAPI && !IsDXVKD3D11WrapperLoaded()) {
        EarlyLog("DllMain: Graphics API detected - initializing IAT hooks "
                 "immediately...");
        InitializeWrapperHooks();
      } else if (hasGraphicsAPI) {
        EarlyLog("DllMain: DXVK d3d11 detected - skipping immediate DXGI/D3D wrapper init");
      } else {
        EarlyLog("DllMain: No graphics API detected - hooks will be installed "
                 "when API loads");
      }

      // Spawn HookThread for all games (injection delay prevents D3D12 init
      // crashes)
      EarlyLog("DllMain: Spawning HookThread for '%s'", fileName);
      HANDLE hThread = CreateThread(NULL, 0, HookThreadWrapper, NULL, 0, NULL);
      if (hThread) {
        SetThreadPriority(hThread, THREAD_PRIORITY_HIGHEST);
        CloseHandle(hThread);
      }
    }

    return TRUE;
  } else if (ul_reason_for_call == DLL_PROCESS_DETACH) {
    // CRITICAL: During process termination (lpReserved != NULL), do ABSOLUTELY
    // NOTHING. The loader lock is held, threads are being killed, and any
    // cleanup can crash.
    if (lpReserved != NULL) {
      // CRITICAL FIX: Set termination flag BEFORE returning
      // This allows hook entry points to detect termination and return early
      // preventing crashes when external DLLs (like opengl32.dll) call into
      // our code during their atexit destructors
      g_ProcessTerminating.store(true, std::memory_order_release);
      return TRUE;
    }

    // Only do cleanup for dynamic unload (FreeLibrary), not process exit
    if (g_isDormant) {
      return TRUE;
    }

    // CRITICAL: Remove DXGI factory vtable hooks FIRST before any other cleanup
    // This prevents game code from calling through our hooks during shutdown
    RemoveGlobalVTableHooks();

    // CRITICAL: Remove all inline hooks BEFORE removing vtable hooks
    // Inline hooks patch actual function code and must be restored early
    InlineHook::RemoveAll();

    // CRITICAL: Remove swapchain vtable hooks BEFORE setting g_ShuttingDown
    // This ensures DetourPresent/DetourPresent1 won't be called after we start
    // cleanup
    DXGIShared::RemoveSwapchainVTableHooks();

    RequestHookShutdown();
    ShutdownScreenshotWorker();

    // Signal HookThread to exit
    if (g_hCheckHooksEvent) {
      SetEvent(g_hCheckHooksEvent);
    }

    // CRITICAL FIX: Shutdown InputManager first to unhook WndProcs
    // This must happen before graphics hooks are shut down to prevent
    // the WndProc from calling into destroyed hook resources
    HookLog("DLL_DETACH: Shutting down InputManager...");
    InputManager::Get().Shutdown();
    HookLog("DLL_DETACH: InputManager shutdown complete");

    // Shutdown performance logger
    PerfLogger::Get().Shutdown();

    // CRITICAL FIX: Set swapchain wrapper shutdown flag BEFORE removing hooks
    // This prevents COM calls on the wrapper from accessing freed memory
    SetSwapchainWrapperShutdown();

    // CRITICAL FIX: Remove Present vtable hooks BEFORE destroying wrappers
    // This prevents DetourPresent from accessing freed wrapper memory
    DXGIShared::RemovePresentHooks();

    // CRITICAL FIX: Properly shutdown hooks using SafeShutdownHook template
    // This calls Shutdown() which releases resources in the correct order
    // Only do this for dynamic unload (lpReserved == NULL), not process exit
    SafeShutdownHook(g_DX12Hook, "DX12Hook");
    SafeShutdownHook(g_DX11Hook, "DX11Hook");
    SafeShutdownHook(g_DX9Hook, "DX9Hook");
    SafeShutdownHook(g_DDrawHook, "DDrawHook");
    SafeShutdownHook(g_DX8Hook, "DX8Hook");
    SafeShutdownHook(g_OpenGLHook, "OpenGLHook");

    // CRITICAL FIX: Don't delete g_IPC during detach
    // The IPC client may be used by other threads that are being terminated
    // Just set to nullptr and let the process cleanup handle it
    // Note: We're intentionally leaking g_IPC here to avoid crashes
    // The shared memory will be cleaned up when the process exits
    g_IPC = nullptr;
    g_LocalConfigOwner.reset();
    g_pLocalConfig = nullptr;

    timeEndPeriod(1);

    CloseCheckHooksEvent();

    // CRITICAL FIX: Clean up TLS index if it was allocated
    // (None currently used)
  }
  return TRUE;
}
