                                       LPSECURITY_ATTRIBUTES, BOOL, DWORD,
                                       LPVOID, LPCSTR, LPSTARTUPINFOA,
                                       LPPROCESS_INFORMATION);
typedef BOOL(WINAPI *CreateProcessW_t)(LPCWSTR, LPWSTR, LPSECURITY_ATTRIBUTES,
                                       LPSECURITY_ATTRIBUTES, BOOL, DWORD,
                                       LPVOID, LPCWSTR, LPSTARTUPINFOW,
                                       LPPROCESS_INFORMATION);
std::atomic<CreateProcessA_t> OriginalCreateProcessA{nullptr};
std::atomic<CreateProcessW_t> OriginalCreateProcessW{nullptr};

namespace {
CreateProcessA_t GetOriginalCreateProcessA() {
  return ResolveOriginalProc(OriginalCreateProcessA, "kernel32.dll",
                             "CreateProcessA");
}

CreateProcessW_t GetOriginalCreateProcessW() {
  return ResolveOriginalProc(OriginalCreateProcessW, "kernel32.dll",
                             "CreateProcessW");
}
} // namespace

// Registry Hook Typedefs (for DLSS Debug Overlay)
typedef LSTATUS(WINAPI *RegQueryValueExW_t)(HKEY hKey, LPCWSTR lpValueName,
                                            LPDWORD lpReserved, LPDWORD lpType,
                                            LPBYTE lpData, LPDWORD lpcbData);
RegQueryValueExW_t OriginalRegQueryValueExW = nullptr;

// Helper: Inject our DLL into a suspended child process.
// Runs on a dedicated worker thread so the calling thread (possibly render
// thread) is not blocked by the 5-second WaitForSingleObject.
struct ChildInjectParams {
  HANDLE hProcess;
  HANDLE hThread;
  char dllPath[MAX_PATH];
};

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
} // namespace

// Forward declaration
void CheckAndInstallHooks();
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

// Hooked Functions - Signal Event & Redirect
static std::string g_SpoofedCmdLineA;
static std::wstring g_SpoofedCmdLineW;

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
