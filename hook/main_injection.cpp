#include "main_internal.h"

static DWORD WINAPI ChildInjectWorker(LPVOID param) {
  auto p = std::unique_ptr<ChildInjectParams>(
      static_cast<ChildInjectParams *>(param));

  SIZE_T pathLen = strlen(p->dllPath) + 1;
  LPVOID pRemote =
      VirtualAllocEx(p->hProcess, NULL, pathLen, MEM_COMMIT, PAGE_READWRITE);
  if (!pRemote) {
    HookLog("[ChildInject] VirtualAllocEx failed: %d", GetLastError());
    ResumeThread(p->hThread);
    CloseHandle(p->hProcess);
    CloseHandle(p->hThread);
    return 1;
  }

  if (!WriteProcessMemory(p->hProcess, pRemote, p->dllPath, pathLen, NULL)) {
    HookLog("[ChildInject] WriteProcessMemory failed: %d", GetLastError());
    VirtualFreeEx(p->hProcess, pRemote, 0, MEM_RELEASE);
    ResumeThread(p->hThread);
    CloseHandle(p->hProcess);
    CloseHandle(p->hThread);
    return 1;
  }

  LPVOID pLoadLib =
      (LPVOID)GetProcAddress(GetModuleHandleA("kernel32.dll"), "LoadLibraryA");
  HANDLE hRemote = CreateRemoteThread(
      p->hProcess, NULL, 0, (LPTHREAD_START_ROUTINE)pLoadLib, pRemote, 0, NULL);
  if (hRemote) {
    WaitForSingleObject(hRemote, 5000);
    CloseHandle(hRemote);
    HookLog("[ChildInject] Injected into child process.");
  } else {
    HookLog("[ChildInject] CreateRemoteThread failed: %d", GetLastError());
  }

  VirtualFreeEx(p->hProcess, pRemote, 0, MEM_RELEASE);
  ResumeThread(p->hThread);
  CloseHandle(p->hProcess);
  CloseHandle(p->hThread);
  return 0;
}

void InjectIntoChild(HANDLE hProcess, HANDLE hThread) {
  // Detect child process bitness. Cross-bitness injection (64→32 or 32→64)
  // cannot work via CreateRemoteThread+LoadLibraryA because the LoadLibraryA
  // address from our kernel32.dll is the wrong bitness. The captureengine host
  // process handles injecting the correct arch DLL independently, so we skip
  // cross-arch children here to avoid crashing them.
  BOOL childIsWow64 = FALSE;
  BOOL selfIsWow64 = FALSE;
  IsWow64Process(hProcess, &childIsWow64);
  IsWow64Process(GetCurrentProcess(), &selfIsWow64);
  if (childIsWow64 != selfIsWow64) {
    HookLog("[ChildInject] Skipping cross-bitness child (self wow64=%d, child "
            "wow64=%d) — let captureengine handle it",
            (int)selfIsWow64, (int)childIsWow64);
    ResumeThread(hThread);
    return;
  }

  auto p = std::make_unique<ChildInjectParams>();
  GetModuleFileNameA(g_hModule, p->dllPath, MAX_PATH);

  // Duplicate handles so the worker thread owns them
  HANDLE hCurrent = GetCurrentProcess();
  if (!DuplicateHandle(hCurrent, hProcess, hCurrent, &p->hProcess, 0, FALSE,
                       DUPLICATE_SAME_ACCESS) ||
      !DuplicateHandle(hCurrent, hThread, hCurrent, &p->hThread, 0, FALSE,
                       DUPLICATE_SAME_ACCESS)) {
    HookLog("[ChildInject] DuplicateHandle failed: %d", GetLastError());
    if (p->hProcess) CloseHandle(p->hProcess);
    ResumeThread(hThread);
    return;
  }

  HANDLE hWorker = CreateThread(NULL, 0, ChildInjectWorker, p.get(), 0, NULL);
  if (hWorker) {
    CloseHandle(hWorker); // Detach — worker cleans up
    // NOLINTNEXTLINE(bugprone-unused-return-value) - ownership intentionally transferred to the detached worker
    p.release();
  } else {
    HookLog("[ChildInject] CreateThread failed: %d", GetLastError());
    CloseHandle(p->hProcess);
    CloseHandle(p->hThread);
    ResumeThread(hThread); // Fallback: resume inline so child isn't stuck
  }
}

// Helper: Check if executable should be injected into.
// Only injects if the process name is on the discovery-memory whitelist.
// The skip list provides a safety backstop for common non-game processes.
bool ShouldInjectChild(const char *exePath) {
  if (!exePath)
    return false;

  // Extract filename from path
  std::string path(exePath);
  size_t lastSlash = path.find_last_of("\\/");
  std::string filename =
      (lastSlash != std::string::npos) ? path.substr(lastSlash + 1) : path;

  // Convert to lowercase
  std::string lowerName;
  for (char c : filename)
    lowerName += (char)tolower(c);

  // Only inject into .exe files
  if (lowerName.length() < 4 ||
      lowerName.substr(lowerName.length() - 4) != ".exe") {
    return false;
  }

   // Skip common system and launcher processes (safety backstop)
  static const char *skipList[] = {"cmd.exe",
                                   "powershell.exe",
                                   "pwsh.exe",
                                   "powershell_ise.exe",
                                   "conhost.exe",
                                   "explorer.exe",
                                   "wscript.exe",
                                   "cscript.exe",
                                   "mshta.exe",
                                   "reg.exe",
                                   "rundll32.exe",
                                   "sdiagnhost.exe",
                                   "regsvr32.exe",
                                   "msiexec.exe",
                                   "taskkill.exe",
                                   "tasklist.exe",
                                   "schtasks.exe",
                                   "wmic.exe",
                                   "mmc.exe",
                                   "steam.exe",
                                   "steamwebhelper.exe",
                                   "gameoverlayui.exe",
                                   "crashpad_handler.exe",
                                   "vc_redist",
                                   "setup",
                                   "install",
                                   "launcher.exe",
                                   "bootstrapper.exe",
                                   "updater.exe",
                                   "epicwebhelper.exe",
                                   "eadesktop.exe",
                                   "origin.exe",
                                   "upc.exe",
                                   "uplay.exe",
                                   "galaxyclient.exe",
                                   nullptr};

  for (int i = 0; skipList[i] != nullptr; i++) {
    std::string_view entry(skipList[i]);
    // Exact-match .exe filenames; substring-match generic terms (vc_redist, setup, install)
    if (entry.ends_with(".exe")) {
      if (lowerName == entry) {
        return false;
      }
    } else {
      if (lowerName.find(skipList[i]) != std::string::npos) {
        return false;
      }
    }
  }

  // Primary check: only inject if the process is on the discovery whitelist.
  // This prevents injection into arbitrary child processes not explicitly
  // approved by CaptureEngine.
  HANDLE hDisc = OpenFileMappingW(FILE_MAP_READ, FALSE, SHARED_MEM_DISCOVERY);
  if (!hDisc) {
    // No discovery memory — CaptureEngine not running or not ready. Don't inject.
    return false;
  }
  DiscoveryInfo *pDisc = (DiscoveryInfo *)MapViewOfFile(
      hDisc, FILE_MAP_READ, 0, 0, sizeof(DiscoveryInfo));
  bool whitelisted = false;
  if (pDisc) {
    if (ValidateDiscoveryInfo(pDisc)) {
      const char *p = pDisc->processWhitelist;
      const char *end = pDisc->processWhitelist + sizeof(pDisc->processWhitelist);
      while (p < end && *p != '\0') {
        if (_stricmp(filename.c_str(), p) == 0) {
          whitelisted = true;
          break;
        }
        p += strlen(p) + 1;
      }
    }
    UnmapViewOfFile(pDisc);
  }
  CloseHandle(hDisc);
  return whitelisted;
}

// Hooked CreateProcessA - Inject into whitelisted child processes only
BOOL WINAPI HookedCreateProcessA(LPCSTR lpApp, LPSTR lpCmd,
                                 LPSECURITY_ATTRIBUTES lpPA,
                                 LPSECURITY_ATTRIBUTES lpTA, BOOL bInherit,
                                 DWORD dwFlags, LPVOID lpEnv, LPCSTR lpDir,
                                 LPSTARTUPINFOA lpSI,
                                 LPPROCESS_INFORMATION lpPI) {
  CreateProcessA_t original = GetOriginalCreateProcessA();
  if (!original) {
    return FALSE;
  }

  const char *exePath = lpApp ? lpApp : lpCmd;
  bool shouldInject = ShouldInjectChild(exePath);

  DWORD modifiedFlags = shouldInject ? (dwFlags | CREATE_SUSPENDED) : dwFlags;
  BOOL result = original(lpApp, lpCmd, lpPA, lpTA, bInherit, modifiedFlags,
                         lpEnv, lpDir, lpSI, lpPI);

  if (result && lpPI && shouldInject) {
    HookLog("[ChildInject] CreateProcessA: Whitelisted child: %s", exePath);
    InjectIntoChild(lpPI->hProcess, lpPI->hThread);
  }
  return result;
}

// Hooked CreateProcessW - Inject into whitelisted child processes only
BOOL WINAPI HookedCreateProcessW(LPCWSTR lpApp, LPWSTR lpCmd,
                                 LPSECURITY_ATTRIBUTES lpPA,
                                 LPSECURITY_ATTRIBUTES lpTA, BOOL bInherit,
                                 DWORD dwFlags, LPVOID lpEnv, LPCWSTR lpDir,
                                 LPSTARTUPINFOW lpSI,
                                 LPPROCESS_INFORMATION lpPI) {
  CreateProcessW_t original = GetOriginalCreateProcessW();
  if (!original) {
    return FALSE;
  }

  // Convert wide to narrow for whitelist check
  char exePath[MAX_PATH] = {0};
  if (lpApp)
    WideCharToMultiByte(CP_UTF8, 0, lpApp, -1, exePath, MAX_PATH, NULL, NULL);
  else if (lpCmd)
    WideCharToMultiByte(CP_UTF8, 0, lpCmd, -1, exePath, MAX_PATH, NULL, NULL);

  bool shouldInject = ShouldInjectChild(exePath);

  DWORD modifiedFlags = shouldInject ? (dwFlags | CREATE_SUSPENDED) : dwFlags;
  BOOL result = original(lpApp, lpCmd, lpPA, lpTA, bInherit, modifiedFlags,
                         lpEnv, lpDir, lpSI, lpPI);

  if (result && lpPI && shouldInject) {
    HookLog("[ChildInject] CreateProcessW: Whitelisted child: %s", exePath);
    InjectIntoChild(lpPI->hProcess, lpPI->hThread);
  }
  return result;
}
