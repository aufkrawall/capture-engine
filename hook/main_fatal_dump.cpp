#include "main_internal.h"

#include "apis/dx12_hook_internal.h"
#include "../common/crash_first_chance.h"

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

// Bounds of the modules the termination path has to tell apart, captured while
// the loader is quiet so that path can classify a frame by range check alone. A
// loader query there would be far riskier: it can run with the loader lock held
// by whatever is tearing the process down, and NtTerminateProcess is reached
// from RtlExitUserProcess with that lock already held by this very thread.
struct TerminationModuleRange {
  std::atomic<uintptr_t> base{0};
  std::atomic<size_t> size{0};
};

static std::atomic<uintptr_t> g_PrimaryModuleBase{0};
static std::atomic<size_t> g_PrimaryModuleSize{0};
static TerminationModuleRange g_CaptureEngineModuleRange;
static TerminationModuleRange g_TerminationPlumbingRanges[ce::crash_dump_policy::kTerminationPlumbingModuleCount];
static std::atomic<bool> g_SuppressedTerminationDumpLogged{false};

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

static const char* DescribeTerminationOrigin(ce::crash_dump_policy::TerminationOrigin origin) {
  switch (origin) {
    case ce::crash_dump_policy::TerminationOrigin::kPrimaryModule:
      return "primary-module";
    case ce::crash_dump_policy::TerminationOrigin::kLoadedModule:
      return "loaded-module";
    case ce::crash_dump_policy::TerminationOrigin::kUnknown:
      break;
  }
  return "unknown";
}

static void LogSuppressedPreTerminationDump(const char* source, DWORD exitCode, const void* callerAddress,
                                           const void* requesterAddress) {
  if (g_SuppressedTerminationDumpLogged.exchange(true, std::memory_order_acq_rel)) {
    return;
  }
  char callerModule[MAX_PATH + 64] = {};
  DescribeAddressModule(const_cast<void*>(callerAddress), callerModule, sizeof(callerModule));
  char requesterModule[MAX_PATH + 64] = {};
  DescribeAddressModule(const_cast<void*>(requesterAddress), requesterModule, sizeof(requesterModule));
  HookLogImportant(
      "FatalExitDump: Skipping pre-termination dump - the application terminated itself from its own image with a "
      "non-crash exit code (source=%s code=0x%08lX caller=%p module=%s requester=%p requesterModule=%s "
      "fgRuntimeActiveOrRecent=1)",
      source ? source : "unknown", static_cast<unsigned long>(exitCode), callerAddress, callerModule,
      requesterAddress, requesterModule);
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

ExternalPreTerminationDumpResult TryCapturePreTerminationDumpWithExternalHelper(
    const char* source, const char* dumpHint, bool stackOnly, const ExternalDumpException* exception) {
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
  if (stackOnly) {
    commandLine += " --dump-helper-scope=stacks";
  }
#ifdef _WIN64
  // The helper is x64 and reads the pointers straight out of this process
  // (ClientPointers). A WoW64 target's 32-bit EXCEPTION_POINTERS layout is not
  // what an x64 dbghelp would read there, so only a same-bitness target passes
  // them; a 32-bit dump keeps its plain thread list.
  if (exception && exception->pointers && exception->threadId != 0) {
    char exceptionArguments[96] = {};
    snprintf(exceptionArguments, sizeof(exceptionArguments), " --dump-helper-exception=0x%llX --dump-helper-tid=%lu",
             static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(exception->pointers)),
             static_cast<unsigned long>(exception->threadId));
    commandLine += exceptionArguments;
  }
#else
  (void)exception;
#endif

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

// Crash-handler services for the injected case. The VEH dump worker cannot use
// an in-process dbghelp walk while a foreign overlay hooks the loader/version
// APIs (that is the ~62 s all-threads-suspended freeze from session
// 20260817_052857), so hand it the same external helper the fatal-exit path
// already prefers, plus the overlay presence it has to decide on.
bool CaptureCrashDumpWithExternalHelperForCrashHandler(const char* dumpFileNameHint, bool stackOnly,
                                                      const ExternalDumpException* exception) {
  return TryCapturePreTerminationDumpWithExternalHelper("crash-handler", dumpFileNameHint, stackOnly, exception) ==
         ExternalPreTerminationDumpResult::kCaptured;
}

bool IsForeignOverlayLoadedForCrashHandler() {
  return ce::overlay_compat::IsThirdPartyOverlayLoaded();
}

void RegisterCrashDumpEnvironmentHooksForHook() {
  CrashDumpEnvironmentHooks hooks;
  hooks.captureWithExternalHelper = &CaptureCrashDumpWithExternalHelperForCrashHandler;
  hooks.foreignOverlayLoaded = &IsForeignOverlayLoadedForCrashHandler;
  RegisterCrashDumpEnvironmentHooks(hooks);
}

static bool TryGetLoadedImageRange(HMODULE module, uintptr_t& imageBase, size_t& imageSize) {
  const auto* base = reinterpret_cast<const uint8_t*>(module);
  if (!base) {
    return false;
  }
  const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
  if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0) {
    return false;
  }
  const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
  if (nt->Signature != IMAGE_NT_SIGNATURE) {
    return false;
  }
  const size_t size = nt->OptionalHeader.SizeOfImage;
  if (size == 0) {
    return false;
  }
  imageBase = reinterpret_cast<uintptr_t>(base);
  imageSize = size;
  return true;
}

// Publish size before base so a reader that sees a non-zero base also sees the
// matching size.
static void StoreTerminationModuleRange(TerminationModuleRange& range, uintptr_t base, size_t size) {
  range.size.store(size, std::memory_order_release);
  range.base.store(base, std::memory_order_release);
}

static bool AddressIsInTerminationModuleRange(uintptr_t address, const std::atomic<uintptr_t>& baseAtomic,
                                              const std::atomic<size_t>& sizeAtomic) {
  const uintptr_t base = baseAtomic.load(std::memory_order_acquire);
  const size_t size = sizeAtomic.load(std::memory_order_acquire);
  return base != 0 && size != 0 && address >= base && address < base + size;
}

void CacheTerminationOriginModuleBounds() {
  uintptr_t base = 0;
  size_t size = 0;
  if (TryGetLoadedImageRange(GetModuleHandleW(nullptr), base, size)) {
    g_PrimaryModuleSize.store(size, std::memory_order_release);
    g_PrimaryModuleBase.store(base, std::memory_order_release);
  }
  if (TryGetLoadedImageRange(g_hModule, base, size)) {
    StoreTerminationModuleRange(g_CaptureEngineModuleRange, base, size);
  }

  // The layers a termination request travels through on its way down. This uses
  // GetModuleHandleA, which never loads: a CRT this process does not use simply
  // stays uncached, and its frames then classify as an ordinary module - the
  // direction that still captures a dump.
  for (size_t i = 0; i < ce::crash_dump_policy::kTerminationPlumbingModuleCount; ++i) {
    HMODULE module = GetModuleHandleA(ce::crash_dump_policy::kTerminationPlumbingModuleNames[i]);
    if (!module || !TryGetLoadedImageRange(module, base, size)) {
      continue;
    }
    StoreTerminationModuleRange(g_TerminationPlumbingRanges[i], base, size);
  }
}

ce::crash_dump_policy::TerminationFrameKind ClassifyTerminationFrame(const void* address) {
  using Kind = ce::crash_dump_policy::TerminationFrameKind;
  if (!address) {
    return Kind::kUnresolved;
  }
  const uintptr_t value = reinterpret_cast<uintptr_t>(address);
  if (AddressIsInTerminationModuleRange(value, g_PrimaryModuleBase, g_PrimaryModuleSize)) {
    return Kind::kPrimaryModule;
  }
  if (AddressIsInTerminationModuleRange(value, g_CaptureEngineModuleRange.base, g_CaptureEngineModuleRange.size)) {
    return Kind::kCaptureEngine;
  }
  for (const auto& range : g_TerminationPlumbingRanges) {
    if (AddressIsInTerminationModuleRange(value, range.base, range.size)) {
      return Kind::kTerminationPlumbing;
    }
  }
  return Kind::kOtherModule;
}

// A termination request reaches CE at every layer it passes through, and only
// the outermost one is called by the module that made it. Portal RTX session
// 20260914_130052 is the regression: NvRemixBridge.exe called
// TerminateProcess(1) from its own image, the TerminateProcess hook correctly
// suppressed the dump, and then KERNELBASE's own implementation called
// NtTerminateProcess - whose hook saw KERNELBASE as the caller, called that a
// loaded module, and wrote the 183 MB dump the outer hook had just refused. So
// when the immediate caller is only carrying the request, attribute it to the
// first frame that is neither CE's own hook nor a carrying layer.
ce::crash_dump_policy::TerminationOrigin ResolveTerminationOrigin(const void* callerAddress,
                                                                  const void** requesterAddress) {
  using Kind = ce::crash_dump_policy::TerminationFrameKind;
  if (requesterAddress) {
    *requesterAddress = callerAddress;
  }
  const uintptr_t base = g_PrimaryModuleBase.load(std::memory_order_acquire);
  const size_t size = g_PrimaryModuleSize.load(std::memory_order_acquire);
  if (!callerAddress || base == 0 || size == 0) {
    return ce::crash_dump_policy::TerminationOrigin::kUnknown;
  }

  switch (ClassifyTerminationFrame(callerAddress)) {
    case Kind::kPrimaryModule:
      return ce::crash_dump_policy::TerminationOrigin::kPrimaryModule;
    case Kind::kOtherModule:
      return ce::crash_dump_policy::TerminationOrigin::kLoadedModule;
    case Kind::kUnresolved:
      return ce::crash_dump_policy::TerminationOrigin::kUnknown;
    case Kind::kCaptureEngine:
    case Kind::kTerminationPlumbing:
      break;
  }

  void* frames[24] = {};
  const USHORT frameCount =
      RtlCaptureStackBackTrace(0, static_cast<DWORD>(sizeof(frames) / sizeof(frames[0])), frames, nullptr);
  Kind kinds[sizeof(frames) / sizeof(frames[0])] = {};
  for (USHORT i = 0; i < frameCount; ++i) {
    kinds[i] = ClassifyTerminationFrame(frames[i]);
  }

  size_t requesterFrameIndex = frameCount;
  const ce::crash_dump_policy::TerminationOrigin origin =
      ce::crash_dump_policy::ResolveTerminationOriginFromFrames(kinds, frameCount, &requesterFrameIndex);
  if (requesterAddress && requesterFrameIndex < frameCount) {
    *requesterAddress = frames[requesterFrameIndex];
  }
  return origin;
}

bool CapturePreTerminationDumpIfNeeded(const char* source, DWORD exitCode, bool targetIsCurrentProcess,
                                       PEXCEPTION_RECORD exceptionRecord, PCONTEXT contextRecord,
                                       void* callerAddress ) {
  if (!callerAddress) {
    callerAddress = __builtin_return_address(0);
  }
  const bool alreadyAttempted = g_PreTerminationDumpAttempted.load(std::memory_order_acquire);
  const bool frameGenerationRuntimeActiveOrRecent = IsFrameGenerationRuntimeActiveForTerminationDump();
  const void* terminationRequester = callerAddress;
  const ce::crash_dump_policy::TerminationOrigin origin =
      ResolveTerminationOrigin(callerAddress, &terminationRequester);

  // The vectored handler only records first-chance faults (see
  // ClassifyFirstChanceException); this is where a fault the process actually
  // dies of becomes a dump, with the recorded faulting context.
  EXCEPTION_RECORD pendingFaultRecord = {};
  CONTEXT pendingFaultContext = {};
  const bool pendingFault =
      targetIsCurrentProcess && !exceptionRecord &&
      ce::crash_first_chance::CopyFaultForCurrentThread(&pendingFaultRecord, &pendingFaultContext);
  const bool insideExceptionDispatch =
      targetIsCurrentProcess && !alreadyAttempted && ce::crash_first_chance::IsCurrentThreadInsideExceptionDispatch();
  const bool followsUnresolvedFault =
      ce::crash_dump_policy::IsTerminationFollowingUnresolvedFault(exitCode, insideExceptionDispatch, pendingFault);
  if (!ce::crash_dump_policy::ShouldCapturePreTerminationDump(targetIsCurrentProcess, exitCode, alreadyAttempted,
                                                              frameGenerationRuntimeActiveOrRecent, origin,
                                                              followsUnresolvedFault)) {
    // Only the FG fallback can suppress a dump the old policy would have taken,
    // so record that decision once instead of leaving a silent gap.
    if (targetIsCurrentProcess && !alreadyAttempted && frameGenerationRuntimeActiveOrRecent && exitCode != 0 &&
        origin == ce::crash_dump_policy::TerminationOrigin::kPrimaryModule &&
        !ce::crash_dump_policy::IsCrashLikeProcessExitCode(exitCode)) {
      LogSuppressedPreTerminationDump(source, exitCode, callerAddress, terminationRequester);
    }
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

  LogFatalExitCallerStack(source, exitCode, callerAddress);

  CONTEXT capturedContext = {};
  if (followsUnresolvedFault && pendingFault) {
    exceptionRecord = &pendingFaultRecord;
    contextRecord = &pendingFaultContext;
    HookLogImportant(
        "FatalExitDump: Termination follows unresolved first-chance fault 0x%08lX at %p (insideDispatch=%d) - "
        "dumping with the recorded faulting context",
        static_cast<unsigned long>(pendingFaultRecord.ExceptionCode), pendingFaultRecord.ExceptionAddress,
        insideExceptionDispatch ? 1 : 0);
  } else if (followsUnresolvedFault) {
    HookLogImportant("FatalExitDump: Termination requested from inside exception dispatch (source=%s code=0x%08lX)",
                     source ? source : "unknown", static_cast<unsigned long>(exitCode));
  }
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

  char requesterModule[MAX_PATH + 64] = {};
  DescribeAddressModule(const_cast<void*>(terminationRequester), requesterModule, sizeof(requesterModule));
  HookLogImportant(
      "FatalExitDump: Capturing pre-termination dump before crash-like process exit or active FG runtime exit "
      "(source=%s code=0x%08lX exceptionAddr=%p crashLike=%d fgRuntimeActiveOrRecent=%d origin=%s requester=%p "
      "requesterModule=%s)",
      source ? source : "unknown", static_cast<unsigned long>(exitCode), exceptionRecord->ExceptionAddress,
      ce::crash_dump_policy::IsCrashLikeProcessExitCode(exitCode) ? 1 : 0,
      frameGenerationRuntimeActiveOrRecent ? 1 : 0, DescribeTerminationOrigin(origin), terminationRequester,
      requesterModule);
  OutputDebugStringA("[FatalExitDump] Capturing pre-termination crash dump.\n");

  HookLogImportant("FatalExitDump: Using minimal-first pre-termination dump attempt (source=%s hint=%s)",
                   source ? source : "unknown", dumpHint);
  // This thread waits for the helper, so `pointers` (on this stack) stays valid
  // for the helper's ClientPointers read.
  ExternalDumpException externalException;
  externalException.pointers = &pointers;
  externalException.threadId = GetCurrentThreadId();
  const ExternalPreTerminationDumpResult externalDumpResult =
      TryCapturePreTerminationDumpWithExternalHelper(source, dumpHint, false, &externalException);
  bool wroteDump = externalDumpResult == ExternalPreTerminationDumpResult::kCaptured;
  if (!wroteDump && externalDumpResult != ExternalPreTerminationDumpResult::kTimedOut &&
      ce::crash_dump_policy::ShouldUseInProcessMiniDumpFallbackAfterExternalHelperFailure(
          ce::overlay_compat::IsThirdPartyOverlayLoaded())) {
    HookLogImportant("FatalExitDump: Falling back to in-process pre-termination dump attempt "
                     "(source=%s hint=%s externalResult=%d overlays=0)",
                     source ? source : "unknown", dumpHint, static_cast<int>(externalDumpResult));
    wroteDump = WriteSupplementalCrashDump(dumpHint, GetCurrentProcess(), GetCurrentProcessId(),
                                           ce::crash_dump_policy::kMinimalDumpType, &exceptionInfo);
  } else if (!wroteDump) {
    HookLogImportant(
        "FatalExitDump: Skipping in-process pre-termination dump fallback (source=%s externalResult=%d "
        "overlays=%d) — in-process dbghelp enumeration can deadlock against foreign overlay hooks",
        source ? source : "unknown", static_cast<int>(externalDumpResult),
        ce::overlay_compat::IsThirdPartyOverlayLoaded() ? 1 : 0);
  }
  HookLogImportant("FatalExitDump: Pre-termination dump %s (source=%s code=0x%08lX hint=%s)",
                   wroteDump ? "captured" : "failed", source ? source : "unknown",
                   static_cast<unsigned long>(exitCode), dumpHint);

  t_InFatalTerminationDumpHook = false;
  return wroteDump;
}

void CaptureCreateSwapchainAccessDeniedExhaustedDump(HWND hWnd, const char* context) {
  static std::atomic<bool> s_attempted{false};
  bool expected = false;
  if (!s_attempted.compare_exchange_strong(expected, true, std::memory_order_acq_rel,
                                           std::memory_order_acquire)) {
    return;
  }

  const char* dumpHint = "swapchain_access_denied_exhausted.dmp";
  HookLogImportant(
      "CreateSwapChainForHwnd: E_ACCESSDENIED recovery exhausted (hwnd=%p context=%s) — capturing a diagnostic "
      "dump for the fatal FG-switch failure (external helper first, so the game thread is not suspended)",
      hWnd, context && context[0] ? context : "unknown");

  // Prefer the EXTERNAL dump helper: it captures from a separate process, so the game's threads are
  // never suspended for the duration of a large dump write. An in-process MiniDumpWriteDump on the
  // render thread froze dx12_fg_switch_test for ~36 s (session 20260813_220022, 114 MB dump) and
  // tripped the FreezeWatchdog — the external helper finished the same capture without freezing.
  const ExternalPreTerminationDumpResult externalResult =
      TryCapturePreTerminationDumpWithExternalHelper(context && context[0] ? context : "CreateSwapChainForHwnd",
                                                     dumpHint);
  bool wroteDump = externalResult == ExternalPreTerminationDumpResult::kCaptured;
  if (!wroteDump && externalResult != ExternalPreTerminationDumpResult::kTimedOut &&
      ce::crash_dump_policy::ShouldUseInProcessMiniDumpFallbackAfterExternalHelperFailure(
          ce::overlay_compat::IsThirdPartyOverlayLoaded())) {
    // Minimal in-process fallback (stacks + module list only) so a missing helper still yields a
    // small, fast dump instead of a long full-memory capture.
    CONTEXT capturedContext = {};
    RtlCaptureContext(&capturedContext);
    EXCEPTION_RECORD synthesizedRecord = {};
    synthesizedRecord.ExceptionCode = 0xE000EACC;  // "EACC" - the E_ACCESSDENIED exhaustion sentinel
    synthesizedRecord.ExceptionAddress = __builtin_return_address(0);
    EXCEPTION_POINTERS pointers = {};
    pointers.ExceptionRecord = &synthesizedRecord;
    pointers.ContextRecord = &capturedContext;
    MINIDUMP_EXCEPTION_INFORMATION exceptionInfo = {};
    exceptionInfo.ThreadId = GetCurrentThreadId();
    exceptionInfo.ExceptionPointers = &pointers;
    exceptionInfo.ClientPointers = FALSE;
    wroteDump = WriteSupplementalCrashDump(dumpHint, GetCurrentProcess(), GetCurrentProcessId(),
                                           ce::crash_dump_policy::kMinimalDumpType, &exceptionInfo);
  } else if (!wroteDump) {
    HookLogImportant(
        "CreateSwapChainForHwnd: Skipping in-process exhaustion dump fallback (context=%s "
        "externalResult=%d overlays=%d)",
        context && context[0] ? context : "unknown", static_cast<int>(externalResult),
        ce::overlay_compat::IsThirdPartyOverlayLoaded() ? 1 : 0);
  }
  HookLogImportant("CreateSwapChainForHwnd: E_ACCESSDENIED exhaustion diagnostic dump %s (context=%s)",
                   wroteDump ? "captured" : "failed", context && context[0] ? context : "unknown");
}
