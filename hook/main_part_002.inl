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

std::mutex g_ExternalDumpStormMutex;
std::unordered_map<std::string, ExternalDumpStormRecord> g_ExternalDumpStormRecords;

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

void MarkExternalSupplementalDumpCaptured(const std::string& key) {
  std::lock_guard<std::mutex> lock(g_ExternalDumpStormMutex);
  auto it = g_ExternalDumpStormRecords.find(key);
  if (it != g_ExternalDumpStormRecords.end()) {
    it->second.supplementalCaptured = true;
  }
}

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

std::string BuildExternalDumpMirrorPath(const char* sourcePath) {
  const std::string dumpDir = GetCrashDumpDirectory();
  if (dumpDir.empty()) {
    return {};
  }

  std::filesystem::path mirrorPath(dumpDir);
  mirrorPath /= ce::crash_dump_policy::BuildMirroredExternalDumpFileName(sourcePath);
  return mirrorPath.string();
}

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

} // namespace

bool IsProcessTerminating() { return g_ProcessTerminating.load(std::memory_order_acquire); }

enum class ProcessCategory {
  PotentialGame,
  Launcher,
  InternalTool,
  Blacklisted
};
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
// Vulkan hook removed - using VK_LAYER_CE_overlay (ICD layer approach) instead

// Global Local Config
AppConfig *g_pLocalConfig = nullptr;

namespace {
std::unique_ptr<AppConfig> g_LocalConfigOwner;

bool IsDXVKD3D11WrapperLoaded() {
  return IsDllFromProject("d3d11.dll", "dxvk");
}

void EnsureLocalConfigAllocated() {
  if (!g_LocalConfigOwner) {
    g_LocalConfigOwner = std::make_unique<AppConfig>();
  }
  g_pLocalConfig = g_LocalConfigOwner.get();
}
} // namespace

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

// Check if we're in shutdown (DLL detach) - use to guard rendering/init
// Note: g_ShuttingDown can be accessed via hook_common.h

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

std::atomic<LoadLibraryA_t> OriginalLoadLibraryA{nullptr};
std::atomic<LoadLibraryW_t> OriginalLoadLibraryW{nullptr};
std::atomic<LoadLibraryExA_t> OriginalLoadLibraryExA{nullptr};
std::atomic<LoadLibraryExW_t> OriginalLoadLibraryExW{nullptr};
std::atomic<LdrLoadDll_t> OriginalLdrLoadDll{nullptr};

namespace {
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

LoadLibraryA_t GetOriginalLoadLibraryA() {
  return ResolveOriginalProc(OriginalLoadLibraryA, "kernel32.dll",
                             "LoadLibraryA");
}

LoadLibraryW_t GetOriginalLoadLibraryW() {
  return ResolveOriginalProc(OriginalLoadLibraryW, "kernel32.dll",
                             "LoadLibraryW");
}

LoadLibraryExA_t GetOriginalLoadLibraryExA() {
  return ResolveOriginalProc(OriginalLoadLibraryExA, "kernel32.dll",
                             "LoadLibraryExA");
}

LoadLibraryExW_t GetOriginalLoadLibraryExW() {
  return ResolveOriginalProc(OriginalLoadLibraryExW, "kernel32.dll",
                             "LoadLibraryExW");
}

LdrLoadDll_t GetOriginalLdrLoadDll() {
  return ResolveOriginalProc(OriginalLdrLoadDll, "ntdll.dll", "LdrLoadDll");
}
} // namespace

typedef LPSTR(WINAPI *GetCommandLineA_t)();
typedef LPWSTR(WINAPI *GetCommandLineW_t)();
std::atomic<GetCommandLineA_t> OriginalGetCommandLineA{nullptr};
std::atomic<GetCommandLineW_t> OriginalGetCommandLineW{nullptr};



// CreateProcess Hook Typedefs for child process injection
typedef BOOL(WINAPI *CreateProcessA_t)(LPCSTR, LPSTR, LPSECURITY_ATTRIBUTES,
