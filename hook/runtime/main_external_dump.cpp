#include "main_internal.h"

namespace {

void PublishMiniDumpWriteDumpTrampoline(void* trampoline, void*) {
  g_OriginalMiniDumpWriteDump.store(reinterpret_cast<MiniDumpWriteDump_t>(trampoline), std::memory_order_release);
}

}  // namespace

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

bool CopyCompletedDumpFile(HANDLE sourceFile, const char* destinationPath, const char* sourcePath) {
  if (!sourceFile || sourceFile == INVALID_HANDLE_VALUE || !destinationPath || !destinationPath[0]) {
    return false;
  }

  // The game's dump file is still open on this handle (MiniDumpWriteDump just returned). Stream-copy
  // through the open handle so file-share modes chosen by the game cannot block the mirror; the game
  // only closes the handle afterwards, so a positional restore is not observable to it.
  LARGE_INTEGER originalPosition = {};
  LARGE_INTEGER zeroOffset = {};
  if (!SetFilePointerEx(sourceFile, zeroOffset, &originalPosition, FILE_BEGIN)) {
    HookLog("CrashMirror: Failed to rewind source dump %s for mirror copy (err=%lu)", sourcePath,
            GetLastError());
    return false;
  }

  HANDLE mirrorFile =
      CreateFileA(destinationPath, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS,
                  FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, nullptr);
  if (mirrorFile == INVALID_HANDLE_VALUE) {
    HookLog("CrashMirror: Failed to create mirror dump %s (err=%lu)", destinationPath, GetLastError());
    SetFilePointerEx(sourceFile, originalPosition, nullptr, FILE_BEGIN);
    return false;
  }

  bool copied = false;
  std::vector<unsigned char> buffer(64 * 1024);
  for (;;) {
    DWORD bytesRead = 0;
    if (!ReadFile(sourceFile, buffer.data(), static_cast<DWORD>(buffer.size()), &bytesRead, nullptr)) {
      copied = false;
      break;
    }
    if (bytesRead == 0) {
      copied = true;
      break;
    }
    DWORD bytesWritten = 0;
    if (!WriteFile(mirrorFile, buffer.data(), bytesRead, &bytesWritten, nullptr) || bytesWritten != bytesRead) {
      copied = false;
      break;
    }
  }

  if (copied) {
    FlushFileBuffers(mirrorFile);
  }
  CloseHandle(mirrorFile);
  SetFilePointerEx(sourceFile, originalPosition, nullptr, FILE_BEGIN);
  if (!copied) {
    HookLog("CrashMirror: Mirror copy of %s -> %s failed (err=%lu)", sourcePath, destinationPath, GetLastError());
    DeleteFileA(destinationPath);
  }
  return copied;
}

void MirrorExternalDumpArtifactIfNeeded(const char* sourcePath, HANDLE hProcess, DWORD processId, MINIDUMP_TYPE dumpType,
                                        PMINIDUMP_EXCEPTION_INFORMATION exceptionParam,
                                        PMINIDUMP_USER_STREAM_INFORMATION userStreamParam,
                                        PMINIDUMP_CALLBACK_INFORMATION callbackParam) {
  (void)dumpType;
  (void)exceptionParam;
  (void)userStreamParam;
  (void)callbackParam;
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

  if (gateDecision.mirrorAllowed) {
    const std::filesystem::path tempDestination =
        destination.parent_path() /
        ce::crash_dump_policy::BuildInProgressDumpFileName(destination.filename().string().c_str());
    const std::string tempMirrorPath = tempDestination.string();
    DeleteFileA(tempMirrorPath.c_str());

    // Re-entering MiniDumpWriteDump inside the game's own dump call deadlocks with foreign overlay
    // hooks (Steam intercepts the module/version enumeration dbghelp performs and blocks, sessions
    // 20260813_220022 / 20260813_222058) and re-dumps a large capture on the crash path. The game's
    // dump file is complete and open on the hook's own handle, so the session mirror is a plain
    // stream copy of that file — byte-identical content, no dbghelp re-entry.
    if (!CopyCompletedDumpFile(hProcess, tempMirrorPath.c_str(), sourcePath)) {
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
    // The rich supplemental CE-owned capture must not run in-process inside the game's dump call:
    // the nested MiniDumpWriteDump froze the render thread for ~36 s on the quick-assert flags
    // (session 20260813_220022) and can deadlock against foreign overlay hooks. The external helper
    // process has neither overlay loaded, so its dbghelp enumeration cannot block. When the helper is
    // unavailable or the dump targets another process, skip the supplemental instead of falling back
    // to an in-process capture.
    const bool targetsCurrentProcess = IsCurrentProcessHandle(hProcess);
    if (targetsCurrentProcess) {
      char supplementalHint[128] = {};
      const char* sourceBaseName = sourcePath ? ce::crash_dump_policy::GetPathFileName(sourcePath) : nullptr;
      snprintf(supplementalHint, sizeof(supplementalHint), "mirrored_%s",
               (sourceBaseName && sourceBaseName[0]) ? sourceBaseName : "external_dump.dmp");
      const ExternalPreTerminationDumpResult helperResult =
          TryCapturePreTerminationDumpWithExternalHelper("CrashMirror supplemental", supplementalHint);
      if (helperResult == ExternalPreTerminationDumpResult::kCaptured) {
        MarkExternalSupplementalDumpCaptured(gateDecision.key);
        HookLogImportant("CrashMirror: Captured supplemental CE-owned dump via external helper for %s",
                         sourcePath);
      } else {
        HookLogImportant(
            "CrashMirror: Supplemental CE-owned dump skipped for %s (helperResult=%d) — never re-enter "
            "MiniDumpWriteDump in-process from the dump hook",
            sourcePath, static_cast<int>(helperResult));
      }
    } else {
      HookLogImportant(
          "CrashMirror: Supplemental CE-owned dump skipped for foreign-process dump %s (pid=%lu)",
          sourcePath, static_cast<unsigned long>(processId));
    }
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
    if (!InlineHook::InstallPublished(target, reinterpret_cast<void*>(&HookedMiniDumpWriteDump), &trampoline,
                                      PublishMiniDumpWriteDumpTrampoline, nullptr)) {
      HookLog("CrashMirror: Failed to install MiniDumpWriteDump inline hook at %p", target);
      return;
    }

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
