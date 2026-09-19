#include "main_internal.h"

#include "common/ngx_ota_runtime.h"

// Installs (or re-installs) CE's kernel32 loader and process-creation hooks
// across every module currently mapped.
//
// This runs twice on purpose. The first pass is in DllMain, before the graphics
// IAT work, because the loader hook is both the cheapest hook CE installs and
// the one whose value decays fastest: anything that maps before it exists can
// never be redirected, and a Streamline/NGX title resolves a large part of its
// runtime within the first few hundred milliseconds. Measured on session
// 20260918_162809, CE's DLL was live at 19:53:39.190 but this ran at 19:53:39.520
// - 330 ms of DXGI/D3D10/D3D11/D3D12 patching stood in between, and every module
// mapped in that window was unreachable.
//
// The second pass is on the hook thread, and it is not redundant: IAT patching
// only reaches import tables that exist when it runs, so modules mapped since
// DllMain need the pass repeated. PatchIATAllModules is idempotent per slot, so
// the overlap costs a re-scan and nothing else.
//
// DllMain safety is the same argument the graphics IAT hooks already rely on:
// this resolves addresses in an already-loaded kernel32 and writes import
// slots. It loads nothing, so it cannot re-enter the loader.
void InstallKernel32LoaderHooks(const char *phase) {
  HookLog("Installing LoadLibrary/CreateProcess hooks via IAT patching (%s)...",
          phase ? phase : "unspecified");

  // Temporary plain pointers for IAT hook init, then stored atomically so other
  // threads never observe a half-written set.
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

  // A repeat pass resolves the same kernel32 exports, so storing them again is
  // harmless; a failed resolution must not overwrite a good pointer with null.
  if (tmpLoadLibraryA)
    OriginalLoadLibraryA.store(tmpLoadLibraryA, std::memory_order_release);
  if (tmpLoadLibraryW)
    OriginalLoadLibraryW.store(tmpLoadLibraryW, std::memory_order_release);
  if (tmpLoadLibraryExA)
    OriginalLoadLibraryExA.store(tmpLoadLibraryExA, std::memory_order_release);
  if (tmpLoadLibraryExW)
    OriginalLoadLibraryExW.store(tmpLoadLibraryExW, std::memory_order_release);
  if (tmpCreateProcessA)
    OriginalCreateProcessA.store(tmpCreateProcessA, std::memory_order_release);
  if (tmpCreateProcessW)
    OriginalCreateProcessW.store(tmpCreateProcessW, std::memory_order_release);
}

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
  if (HookIsShuttingDown()) {
    return original(lpApp, lpCmd, lpPA, lpTA, bInherit, dwFlags, lpEnv, lpDir, lpSI, lpPI);
  }

  const char *exePath = lpApp ? lpApp : lpCmd;

  // ngx_ota=off: refuse the NGX updater before it is created. `_nvngx.dll`
  // treats a failed launch as "use the cache", so this is a supported outcome
  // rather than a broken call. Reported as ACCESS_DENIED because that is what
  // it is - CE denied it.
  if (ce::ngx_ota::ShouldRefuseProcessLaunch(exePath)) {
    ce::ngx_ota::NoteUpdaterLaunchRefused(exePath);
    if (lpPI) {
      *lpPI = PROCESS_INFORMATION{};
    }
    SetLastError(ERROR_ACCESS_DENIED);
    return FALSE;
  }

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
  if (HookIsShuttingDown()) {
    return original(lpApp, lpCmd, lpPA, lpTA, bInherit, dwFlags, lpEnv, lpDir, lpSI, lpPI);
  }

  // Decide the NGX question on the caller's own wide string, BEFORE the narrow
  // conversion below. WideCharToMultiByte writes nothing when the destination
  // is too small, so a command line longer than the buffer used to leave
  // exePath empty and let the updater through unrecognized - the failure mode
  // was silence, not an error. See HookedCreateProcessA for why refusing is a
  // supported outcome rather than a broken call.
  const wchar_t *ngxTarget = lpApp ? lpApp : lpCmd;
  if (ce::ngx_ota::ShouldRefuseProcessLaunch(ngxTarget)) {
    ce::ngx_ota::NoteUpdaterLaunchRefused(ngxTarget);
    if (lpPI) {
      *lpPI = PROCESS_INFORMATION{};
    }
    SetLastError(ERROR_ACCESS_DENIED);
    return FALSE;
  }

  // Convert wide to narrow for the whitelist check. The buffer is deliberately
  // larger than MAX_PATH: when lpApplicationName is null the argument is a full
  // command line, which routinely exceeds 260 characters, and a conversion that
  // does not fit silently yields an empty name - which reads as "not
  // whitelisted" rather than as a failure.
  char exePath[2048] = {0};
  if (lpApp)
    WideCharToMultiByte(CP_UTF8, 0, lpApp, -1, exePath, sizeof(exePath), NULL, NULL);
  else if (lpCmd)
    WideCharToMultiByte(CP_UTF8, 0, lpCmd, -1, exePath, sizeof(exePath), NULL, NULL);

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
