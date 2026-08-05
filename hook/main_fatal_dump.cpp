#include "main_internal.h"

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
