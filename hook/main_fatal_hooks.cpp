#include "main_internal.h"
#include "common/perf_logger.h"

#include <array>

bool ShouldCaptureExplicitFatalRaise(DWORD code) {
  return code == ce::crash_dump_policy::kFailFastExceptionExitCode || code == EXCEPTION_STACK_OVERFLOW ||
         code == EXCEPTION_ILLEGAL_INSTRUCTION;
}

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

// CE's own session artifacts have to be finalized on every exit of this
// process, not only on the dump-worthy ones: CapturePreTerminationDumpIfNeeded
// deliberately returns early for an ordinary exit code, and the CSV writer's
// only other finalizer is a CRT static destructor that a TerminateProcess exit
// never reaches. Once-only and non-blocking; this runs while the rest of the
// process is still live.
void FinalizeSessionArtifactsBeforeTermination(const char* source, bool targetIsCurrentProcess) {
  if (!targetIsCurrentProcess) {
    return;
  }
  static std::atomic<bool> s_finalized{false};
  bool expected = false;
  if (!s_finalized.compare_exchange_strong(expected, true, std::memory_order_acq_rel, std::memory_order_acquire)) {
    return;
  }
  PerfLogger::Get().FlushForTermination(source);
}

BOOL WINAPI HookedTerminateProcess(HANDLE hProcess, UINT uExitCode) {
  FinalizeSessionArtifactsBeforeTermination("TerminateProcess", IsCurrentProcessHandle(hProcess));
  CapturePreTerminationDumpIfNeeded("TerminateProcess", static_cast<DWORD>(uExitCode), IsCurrentProcessHandle(hProcess),
                                    nullptr, nullptr, __builtin_return_address(0));

  const auto original = g_OriginalTerminateProcess.load(std::memory_order_acquire);
  if (original) {
    return original(hProcess, uExitCode);
  }

  return TerminateProcess(hProcess, uExitCode);
}

VOID WINAPI HookedExitProcess(UINT uExitCode) {
  FinalizeSessionArtifactsBeforeTermination("ExitProcess", true);
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
  FinalizeSessionArtifactsBeforeTermination("RtlExitUserProcess", true);
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
  FinalizeSessionArtifactsBeforeTermination("NtTerminateProcess", targetIsCurrentProcess);
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

void TryInstallFatalTerminationDumpHooks() {
  std::call_once(g_FatalTerminationDumpHookOnce, []() {
    // Resolve the bounds of the executable, of CE's own image and of the modules
    // a termination request is forwarded through here, not on the termination
    // path, where the loader lock may already be held by the teardown in
    // progress.
    CacheTerminationOriginModuleBounds();
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

    struct FatalInlineHookRequest {
      const char* moduleName;
      const char* functionName;
      const char* fallbackModuleName;
      const char* fallbackFunctionName;
      void* hookFunction;
      InlineHook::TrampolinePublisher publisher;
      void* publisherContext;
      const char* selectedModuleName = nullptr;
      const char* selectedFunctionName = nullptr;
      void* target = nullptr;
      void* trampoline = nullptr;
      size_t batchIndex = 0;
      bool queued = false;
      bool selectedFallback = false;
    };
    std::array<FatalInlineHookRequest, 10> requests = {{
        {"KERNELBASE.dll", "RaiseFailFastException", "kernel32.dll", "RaiseFailFastException",
         reinterpret_cast<void*>(&HookedRaiseFailFastException),
         &PublishFatalHookTrampoline<RaiseFailFastException_t>, &g_OriginalRaiseFailFastException},
        {"KERNELBASE.dll", "TerminateProcess", "kernel32.dll", "TerminateProcess",
         reinterpret_cast<void*>(&HookedTerminateProcess), &PublishFatalHookTrampoline<TerminateProcess_t>,
         &g_OriginalTerminateProcess},
        {"KERNELBASE.dll", "ExitProcess", "kernel32.dll", "ExitProcess",
         reinterpret_cast<void*>(&HookedExitProcess), &PublishFatalHookTrampoline<ExitProcess_t>,
         &g_OriginalExitProcess},
        {"ntdll.dll", "RtlExitUserProcess", nullptr, nullptr, reinterpret_cast<void*>(&HookedRtlExitUserProcess),
         &PublishFatalHookTrampoline<RtlExitUserProcess_t>, &g_OriginalRtlExitUserProcess},
        {"ntdll.dll", "NtTerminateProcess", "ntdll.dll", "ZwTerminateProcess",
         reinterpret_cast<void*>(&HookedNtTerminateProcess), &PublishFatalHookTrampoline<NtTerminateProcess_t>,
         &g_OriginalNtTerminateProcess},
        {"ucrtbase.dll", "_invalid_parameter_noinfo_noreturn", nullptr, nullptr,
         reinterpret_cast<void*>(&HookedInvalidParameterNoInfoNoReturn),
         &PublishFatalHookTrampoline<InvalidParameterNoInfoNoReturn_t>,
         &g_OriginalInvalidParameterNoInfoNoReturn},
        {"ucrtbase.dll", "_invoke_watson", nullptr, nullptr, reinterpret_cast<void*>(&HookedInvokeWatson),
         &PublishFatalHookTrampoline<InvokeWatson_t>, &g_OriginalInvokeWatson},
        {"ucrtbase.dll", "abort", nullptr, nullptr, reinterpret_cast<void*>(&HookedAbort),
         &PublishFatalHookTrampoline<Abort_t>, &g_OriginalAbort},
        {"ucrtbase.dll", "terminate", nullptr, nullptr, reinterpret_cast<void*>(&HookedTerminate),
         &PublishFatalHookTrampoline<Terminate_t>, &g_OriginalTerminate},
        {"ucrtbase.dll", "_purecall", nullptr, nullptr, reinterpret_cast<void*>(&HookedPurecall),
         &PublishFatalHookTrampoline<Purecall_t>, &g_OriginalPurecall},
    }};

    std::array<InlineHook::PublishedHookSpec, 10> inlineHooks = {};
    std::array<size_t, 10> requestForBatchEntry = {};
    size_t batchEntryCount = 0;
    for (size_t requestIndex = 0; requestIndex < requests.size(); ++requestIndex) {
      auto& request = requests[requestIndex];
      request.selectedModuleName = request.moduleName;
      request.selectedFunctionName = request.functionName;
      request.target = ResolveModuleExport(request.moduleName, request.functionName);
      if ((!request.target || request.target == request.hookFunction) && request.fallbackModuleName) {
        request.selectedModuleName = request.fallbackModuleName;
        request.selectedFunctionName = request.fallbackFunctionName;
        request.target = ResolveModuleExport(request.fallbackModuleName, request.fallbackFunctionName);
        request.selectedFallback = true;
      }
      if (!request.target || request.target == request.hookFunction) {
        continue;
      }

      bool duplicateTarget = false;
      for (size_t earlierIndex = 0; earlierIndex < requestIndex; ++earlierIndex) {
        if (requests[earlierIndex].queued && requests[earlierIndex].target == request.target) {
          duplicateTarget = true;
          break;
        }
      }
      if (duplicateTarget) {
        continue;
      }

      request.batchIndex = batchEntryCount;
      request.queued = true;
      requestForBatchEntry[batchEntryCount] = requestIndex;
      inlineHooks[batchEntryCount] = {request.target, request.hookFunction, &request.trampoline,
                                      request.publisher, request.publisherContext};
      ++batchEntryCount;
    }

    InlineHook::InstallPublishedBatch(inlineHooks.data(), batchEntryCount);
    std::vector<void*> inlineHookTargets;
    for (size_t batchIndex = 0; batchIndex < batchEntryCount; ++batchIndex) {
      auto& request = requests[requestForBatchEntry[batchIndex]];
      if (!inlineHooks[batchIndex].installed) {
        HookLog("FatalExitDump: Inline pre-termination hook failed for %s!%s at %p",
                request.selectedModuleName, request.selectedFunctionName, request.target);
        continue;
      }
      inlineHookTargets.push_back(request.target);
      patchedAny = true;
      HookLogImportant("FatalExitDump: Installed inline pre-termination hook for %s!%s at %p (trampoline=%p)",
                       request.selectedModuleName, request.selectedFunctionName, request.target,
                       request.trampoline);
    }

    // Preserve the established export fallback on the rare preparation or
    // exact-byte commit failure. The common path above still quiesces peers
    // only once for the complete fatal-hook family.
    for (auto& request : requests) {
      if (!request.queued || inlineHooks[request.batchIndex].installed || !request.fallbackModuleName ||
          request.selectedFallback) {
        continue;
      }
      void* fallbackTarget = ResolveModuleExport(request.fallbackModuleName, request.fallbackFunctionName);
      if (!fallbackTarget || fallbackTarget == request.target || fallbackTarget == request.hookFunction ||
          std::find(inlineHookTargets.begin(), inlineHookTargets.end(), fallbackTarget) != inlineHookTargets.end()) {
        continue;
      }
      request.trampoline = nullptr;
      if (!InlineHook::InstallPublished(fallbackTarget, request.hookFunction, &request.trampoline,
                                        request.publisher, request.publisherContext)) {
        HookLog("FatalExitDump: Inline fallback hook failed for %s!%s at %p", request.fallbackModuleName,
                request.fallbackFunctionName, fallbackTarget);
        continue;
      }
      inlineHookTargets.push_back(fallbackTarget);
      patchedAny = true;
      HookLogImportant("FatalExitDump: Installed inline fallback hook for %s!%s at %p (trampoline=%p)",
                       request.fallbackModuleName, request.fallbackFunctionName, fallbackTarget,
                       request.trampoline);
    }

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
