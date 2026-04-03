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
#include "../common/crash_handler.h"
#include "apis/ffx_hook.h" // FSR Frame Generation hook
#include "apis/nvngx_hook.h"
#include "capture/shared_capture.h"
#include "common/dxgi_shared.h"
#include "common/fg_detection.h"
#include "common/hook_common.h"
#include "common/hook_context.h"
#include "common/input_manager.h"
#include "common/ipc_client.h"
#include "common/perf_logger.h"
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
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

HMODULE g_hModule = NULL;
// Note: g_ShuttingDown is declared in hook/common/hook_common.h

static std::atomic<bool> g_ProcessTerminating{false};

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
                                   "conhost.exe",
                                   "explorer.exe",
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
    if (lowerName.find(skipList[i]) != std::string::npos) {
      return false;
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
    if (pDisc->GetMagic() == DISCOVERY_MAGIC) {
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
                   filenameLower.begin(), ::tolower);

    std::string overridePath;

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
                       cfgFilenameLower.begin(), ::tolower);

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

      HookLog("Redirecting %s to: %s", filename.c_str(), finalPath.c_str());
      return finalPath;
    }

  } catch (...) {
    // Fallback
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

// Hooked Functions - Signal Event & Redirect
static std::string g_SpoofedCmdLineA;
static std::wstring g_SpoofedCmdLineW;

void NotifyHookModuleLoaded(HMODULE module, const char *moduleNameOrPath) {
  if (!module)
    return;

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
  }

  if (g_hCheckHooksEvent)
    SetEvent(g_hCheckHooksEvent);
}

LPSTR WINAPI HookedGetCommandLineA() {
  LPSTR original = OriginalGetCommandLineA.load(std::memory_order_acquire)();

  // Only spoof if config is loaded and feature forced
  if (g_pLocalConfig && g_pLocalConfig->graphics.forceRayReconstruction) {
    static bool s_Logged = false;
    if (!s_Logged) {
      HookLog("HookedGetCommandLineA called. Original: %s",
              original ? original : "<null>");
      s_Logged = true;
    }

    if (g_SpoofedCmdLineA.empty()) {
      if (original)
        g_SpoofedCmdLineA = original;

      // Check if argument already exists to avoid duplication
      if (g_SpoofedCmdLineA.find("r.NGX.DLSS.denoisermode") ==
          std::string::npos) {
        g_SpoofedCmdLineA += " -r.NGX.DLSS.denoisermode=1";
        // Also force the RR feature cvar just in case (some plugins use this)
        g_SpoofedCmdLineA += " -r.NGX.DLSS.RayReconstruction=1";
        HookLog("HookedGetCommandLineA: Appended CVar flags.");
      }
    }
    return (LPSTR)g_SpoofedCmdLineA.c_str();
  }
  return original;
}

LPWSTR WINAPI HookedGetCommandLineW() {
  LPWSTR original = OriginalGetCommandLineW.load(std::memory_order_acquire)();

  if (g_pLocalConfig && g_pLocalConfig->graphics.forceRayReconstruction) {
    static bool s_Logged = false;
    if (!s_Logged) {
      // wchar conversion for logging
      char buf[2048];
      WideCharToMultiByte(CP_UTF8, 0, original, -1, buf, 2048, NULL, NULL);
      HookLog("HookedGetCommandLineW called. Original: %s", buf);
      s_Logged = true;
    }

    if (g_SpoofedCmdLineW.empty()) {
      if (original)
        g_SpoofedCmdLineW = original;

      if (g_SpoofedCmdLineW.find(L"r.NGX.DLSS.denoisermode") ==
          std::wstring::npos) {
        g_SpoofedCmdLineW += L" -r.NGX.DLSS.denoisermode=1";
        g_SpoofedCmdLineW += L" -r.NGX.DLSS.RayReconstruction=1";
        HookLog("HookedGetCommandLineW: Appended CVar flags.");
      }
    }
    return (LPWSTR)g_SpoofedCmdLineW.c_str();
  }
  return original;
}



// Hooked Functions - Signal Event & Redirect
HMODULE WINAPI HookedLoadLibraryA(LPCSTR lpLibFileName) {
  LoadLibraryA_t original = GetOriginalLoadLibraryA();
  if (!original) {
    return nullptr;
  }

  if (lpLibFileName) {
    std::string redirect = GetRedirectedPath(lpLibFileName);
    if (!redirect.empty()) {
      HMODULE hMod = original(redirect.c_str());
      NotifyHookModuleLoaded(hMod, redirect.c_str());
      return hMod;
    }
  }
  HMODULE hMod = original(lpLibFileName);
  NotifyHookModuleLoaded(hMod, lpLibFileName);
  return hMod;
}

HMODULE WINAPI HookedLoadLibraryW(LPCWSTR lpLibFileName) {
  LoadLibraryW_t original = GetOriginalLoadLibraryW();
  if (!original) {
    return nullptr;
  }

  char pathUtf8[MAX_PATH] = {};
  if (lpLibFileName) {
    // Convert to UTF-8 for check
    WideCharToMultiByte(CP_UTF8, 0, lpLibFileName, -1, pathUtf8, MAX_PATH, NULL,
                        NULL);
    std::string redirect = GetRedirectedPath(pathUtf8);

    if (!redirect.empty()) {
      // Convert back to Wide for LoadLibraryW if needed, or just use A?
      // Safer to use W with W
      std::wstring redirectW;
      int len = MultiByteToWideChar(CP_UTF8, 0, redirect.c_str(), -1, NULL, 0);
      if (len > 0) {
        redirectW.resize(len);
        MultiByteToWideChar(CP_UTF8, 0, redirect.c_str(), -1, &redirectW[0],
                            len);
        // Remove null terminator added by resize if strictly needed, but
        // usually LoadLibraryW handles it
        if (redirectW.back() == L'\0')
          redirectW.pop_back();

        HMODULE hMod = original(redirectW.c_str());
        NotifyHookModuleLoaded(hMod, redirect.c_str());
        return hMod;
      }
    }
  }
  HMODULE hMod = original(lpLibFileName);
  NotifyHookModuleLoaded(hMod, pathUtf8);
  return hMod;
}

HMODULE WINAPI HookedLoadLibraryExA(LPCSTR lpLibFileName, HANDLE hFile,
                                    DWORD dwFlags) {
  LoadLibraryExA_t original = GetOriginalLoadLibraryExA();
  if (!original) {
    return nullptr;
  }

  if (lpLibFileName) {
    std::string redirect = GetRedirectedPath(lpLibFileName);
    if (!redirect.empty()) {
      // Use OriginalLoadLibraryA for the redirect to simplify (Ex flags might
      // conflict with absolute path? usually ok) But let's stick to ExA to
      // respect flags if possible, filtering flags that shouldn't apply to
      // absolute path? Actually, usually users just want to load the DLL.
      // LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR might be an issue. Let's try to trust
      // the user path is absolute.
      HMODULE hMod = original(redirect.c_str(), hFile, dwFlags);
      NotifyHookModuleLoaded(hMod, redirect.c_str());
      return hMod;
    }
  }
  HMODULE hMod = original(lpLibFileName, hFile, dwFlags);
  NotifyHookModuleLoaded(hMod, lpLibFileName);
  return hMod;
}

HMODULE WINAPI HookedLoadLibraryExW(LPCWSTR lpLibFileName, HANDLE hFile,
                                    DWORD dwFlags) {
  LoadLibraryExW_t original = GetOriginalLoadLibraryExW();
  if (!original) {
    return nullptr;
  }

  char pathUtf8[MAX_PATH] = {};
  if (lpLibFileName) {
    WideCharToMultiByte(CP_UTF8, 0, lpLibFileName, -1, pathUtf8, MAX_PATH, NULL,
                        NULL);
    std::string redirect = GetRedirectedPath(pathUtf8);
    if (!redirect.empty()) {
      std::wstring redirectW;
      int len = MultiByteToWideChar(CP_UTF8, 0, redirect.c_str(), -1, NULL, 0);
      if (len > 0) {
        redirectW.resize(len);
        MultiByteToWideChar(CP_UTF8, 0, redirect.c_str(), -1, &redirectW[0],
                            len);
        if (redirectW.back() == L'\0')
          redirectW.pop_back();

        HMODULE hMod = original(redirectW.c_str(), hFile, dwFlags);
        NotifyHookModuleLoaded(hMod, redirect.c_str());
        return hMod;
      }
    }
  }
  HMODULE hMod = original(lpLibFileName, hFile, dwFlags);
  NotifyHookModuleLoaded(hMod, pathUtf8);
  return hMod;
}

NTSTATUS NTAPI HookedLdrLoadDll(PWSTR SearchPath, PULONG DllCharacteristics,
                                PUNICODE_STRING DllName, PVOID *BaseAddress) {
  LdrLoadDll_t original = GetOriginalLdrLoadDll();
  std::string requestedPath;
  if (DllName && DllName->Buffer && DllName->Length > 0 && original) {
    std::wstring requestedW(DllName->Buffer, DllName->Length / sizeof(wchar_t));

    if (!requestedW.empty()) {
      int utf8Len =
          WideCharToMultiByte(CP_UTF8, 0, requestedW.c_str(), -1, nullptr, 0, nullptr, nullptr);
      if (utf8Len > 0) {
        requestedPath.resize(static_cast<size_t>(utf8Len));
        WideCharToMultiByte(CP_UTF8, 0, requestedW.c_str(), -1, requestedPath.data(), utf8Len, nullptr, nullptr);
        if (!requestedPath.empty() && requestedPath.back() == '\0') {
          requestedPath.pop_back();
        }

        std::string redirect = GetRedirectedPath(requestedPath);
        if (!redirect.empty()) {
          std::wstring redirectW;
          int wLen = MultiByteToWideChar(CP_UTF8, 0, redirect.c_str(), -1, NULL, 0);
          if (wLen > 0) {
            redirectW.resize(static_cast<size_t>(wLen));
            MultiByteToWideChar(CP_UTF8, 0, redirect.c_str(), -1, redirectW.data(), wLen);
            if (!redirectW.empty() && redirectW.back() == L'\0') {
              redirectW.pop_back();
            }

            UNICODE_STRING redirectName{};
            redirectName.Buffer = const_cast<PWSTR>(redirectW.c_str());
            redirectName.Length = static_cast<USHORT>(redirectW.size() * sizeof(wchar_t));
            redirectName.MaximumLength = redirectName.Length + sizeof(wchar_t);

            NTSTATUS status = original(SearchPath, DllCharacteristics,
                                       &redirectName, BaseAddress);
            if (NT_SUCCESS(status)) {
              NotifyHookModuleLoaded(BaseAddress ? (HMODULE)*BaseAddress : nullptr,
                                     redirect.c_str());
              return status;
            }
          }
        }
      }
    }
  }

  if (!original)
    return STATUS_DLL_NOT_FOUND;

  NTSTATUS status = original(SearchPath, DllCharacteristics, DllName,
                             BaseAddress);
  if (NT_SUCCESS(status))
    NotifyHookModuleLoaded(BaseAddress ? (HMODULE)*BaseAddress : nullptr,
                           requestedPath.c_str());
  return status;
}

// ----------------------------------------------------------------------------
// UE5 CVar Enforcer using Pattern Scanning
// ----------------------------------------------------------------------------
namespace UE5 {
// IConsoleManager Interface (Virtual Table Reconstruction)
// We only need FindConsoleVariable.

// Virtual Table Layout (Estimated for UE4/5):
// 0: Destructor
// ...
// FindConsoleVariable is often index 3 (UE4.27 to UE5.3 often consistent)
// but can vary.

// A safer, albeit more complex, approach is to find the CVar directly by name
// string scan.

class IConsoleVariable {
public:
  virtual ~IConsoleVariable() {}
  virtual void Set(const wchar_t *Value, uint32_t SetBy = 0) = 0;
  virtual void Set(const char *Value, uint32_t SetBy = 0) = 0;
  virtual void Set(int32_t Value, uint32_t SetBy = 0) = 0;
  virtual void Set(float Value, uint32_t SetBy = 0) = 0;
  // The above is a GUESS. The actual interface has overloads.
  // Usually Set(const TCHAR* InValue, EConsoleVariableSetBy InSetBy) is the
  // main one. EConsoleVariableSetBy: SetByCommandline = 0x00000002.
};

// We will use a "manual vtable call" helper to avoid interface mismatches.
template <typename T> T GetVFunc(void *instance, int index) {
  uintptr_t *vtable = *((uintptr_t **)instance);
  return (T)vtable[index];
}

// IConsoleManager::FindConsoleVariable is usually index 3 or 4.
// IConsoleVariable::Set is usually index 0, 1, or 2 (Set has overloads).
// Let's assume standard UE4/5 layout:
// IConsoleManager:
// 0: ~
// 1: RegisterConsoleObject
// 2: UnregisterConsoleObject
// 3: FindConsoleObject(name)
// 4: FindConsoleVariable(name) <--- Target

// IConsoleVariable:
// 0: ~
// 1: Set(const TCHAR* InValue, uint32 SetBy)
// 2: ...

typedef void *(*FindConsoleVariable_t)(void *mgr, const wchar_t *name);
typedef void (*Set_t)(void *cvar, const wchar_t *value, uint32_t setBy);

void EnforceRR() {
  if (!g_pLocalConfig || !g_pLocalConfig->graphics.forceRayReconstruction)
    return;

  static uintptr_t s_ConsoleManagerPtr = 0;
  static bool s_AttemptedScan = false;

  HMODULE hMain = GetModuleHandleA(NULL);
  if (!hMain)
    return;

  if (!s_ConsoleManagerPtr && !s_AttemptedScan) {
    s_AttemptedScan = true;

    // Strategy 1: Scan for "r.DumpingMovie" (Core CVar) string ref
    // This is a very safe anchor.
    uintptr_t refStr = Scanner::ScanForStringRef(hMain, "r.DumpingMovie");
    if (!refStr) {
      // Try another one "r.AmbientOcclusionLevels"
      refStr = Scanner::ScanForStringRef(hMain, "r.AmbientOcclusionLevels");
    }

    if (refStr) {
      // refStr points to the LEA/MOV instruction loading the string.
      // We look backwards for the call to IConsoleManager::Get()
      // usually within 50 bytes.
      // Pattern: CALL Get; ...; LEA RDX, String

      uint8_t *p = (uint8_t *)refStr;
      for (int i = 0; i < 100; i++) {
        // Check for CALL (E8)
        if (*(p - i) == 0xE8) {
          // This MIGHT be IConsoleManager::Get()
          // Let's check where it goes.
          int32_t offset = *(int32_t *)(p - i + 1);
          uintptr_t funcAddr = (uintptr_t)(p - i + 5 + offset);

          // Check if specific function pattern: MOV RAX, [Global]; RET
          // 48 8B 05 ?? ?? ?? ?? C3
          if (*(uint8_t *)funcAddr == 0x48 &&
              *(uint8_t *)(funcAddr + 1) == 0x8B &&
              *(uint8_t *)(funcAddr + 7) == 0xC3) {
            // Found it!
            int32_t gOffset = *(int32_t *)(funcAddr + 3);
            s_ConsoleManagerPtr =
                funcAddr + 7 + gOffset; // The Global Variable Address
            HookLog("UE5: Found ConsoleManager singleton at %p (via "
                    "r.DumpingMovie)",
                    (void *)s_ConsoleManagerPtr);
            break;
          }
        }
      }
    }

    // Strategy 2: If finding Get() failed, try finding GConsoleManager global
    // directly via AOB
    if (!s_ConsoleManagerPtr) {
      // Generic pattern for "MOV RCX, [GConsoleManager]"
      // 48 8B 0D ?? ?? ?? ?? 48 85 C9 75 ?? E8
      uintptr_t aob =
          Scanner::Scan(hMain, "48 8B 0D ?? ?? ?? ?? 48 85 C9 75 ?? E8");
      if (aob) {
        int32_t offset = *(int32_t *)(aob + 3);
        s_ConsoleManagerPtr = aob + 7 + offset;
        HookLog("UE5: Found ConsoleManager singleton at %p (via AOB)",
                (void *)s_ConsoleManagerPtr);
      }
    }
  }

  if (s_ConsoleManagerPtr) {
    void *mgr = *(void **)s_ConsoleManagerPtr;
    HookLog("UE5: GConsoleManager Value at %p is %p",
            (void *)s_ConsoleManagerPtr, mgr);

    bool safeToUseMgr = false;
    if (mgr) {
      // ... verify vtable ...
      MEMORY_BASIC_INFORMATION mbi;
      if (VirtualQuery((void *)mgr, &mbi, sizeof(mbi)) &&
          (mbi.Protect &
           (PAGE_READONLY | PAGE_READWRITE | PAGE_EXECUTE_READWRITE))) {
        safeToUseMgr = true;
      } else {
        HookLog("UE5: GConsoleManager points to invalid memory!");
      }
    }

    if (safeToUseMgr) {
      // Probe VTable for FindConsoleVariable
      // We test indices 3, 4, 5
      static int s_ValidFindIndex = -1;

      if (s_ValidFindIndex == -1) {
        for (int idx : {4, 3, 5}) {
          FindConsoleVariable_t fn = GetVFunc<FindConsoleVariable_t>(mgr, idx);
          if (!fn)
            continue;

          // Check if points to executable memory
          MEMORY_BASIC_INFORMATION mbi;
          if (VirtualQuery((void *)fn, &mbi, sizeof(mbi))) {
            if (!(mbi.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ |
                                 PAGE_EXECUTE_READWRITE))) {
              continue;
            }
          }

          // Try "r.vsync"
          // Wrap in try/except if possible (not standard C++) but we don't have
          // it. We rely on memory check.
          void *check = fn(mgr, L"r.vsync");
          // If it returns null, it might just be not found?
          // But r.vsync is standard.
          // Try "r.DumpingMovie" which we scanned for?
          if (check) {
            s_ValidFindIndex = idx;
            HookLog("UE5: Confirmed FindConsoleVariable at VTable Index %d",
                    idx);
            break;
          }
        }
        if (s_ValidFindIndex == -1) {
          HookLog("UE5: Failed to find FindConsoleVariable (Probed 3, 4, 5)");
          // Prevent retry spam
          s_ValidFindIndex = -2;
          // DO NOT RETURN! FALLTHROUGH TO FALLBACK
        }
      }

      if (s_ValidFindIndex >= 0) {
        FindConsoleVariable_t fnFind =
            GetVFunc<FindConsoleVariable_t>(mgr, s_ValidFindIndex);

        // 1. Denoiser Mode
        void *cvarMode = fnFind(mgr, L"r.NGX.DLSS.denoisermode");
        if (cvarMode) {
          static Set_t fnSet = nullptr;
          if (!fnSet)
            fnSet = GetVFunc<Set_t>(cvarMode, 1);

          if (fnSet) {
            fnSet(cvarMode, L"1", 0x02);
            HookLog("UE5: Force Set r.NGX.DLSS.denoisermode=1");
          }
        } else {
          static bool s_LogOnce = false;
          if (!s_LogOnce) {
            HookLog(
                "UE5: CVar 'r.NGX.DLSS.denoisermode' NOT FOUND via Manager.");
            s_LogOnce = true;
          }
        }

        // 2. Ray Reconstruction
        void *cvarRR = fnFind(mgr, L"r.NGX.DLSS.RayReconstruction");
        if (cvarRR) {
          static Set_t fnSet = nullptr;
          if (!fnSet)
            fnSet = GetVFunc<Set_t>(cvarRR, 1);

          if (fnSet) {
            fnSet(cvarRR, L"1", 0x02);
            HookLog("UE5: Force Set r.NGX.DLSS.RayReconstruction=1");
          }
        }
        // If we succeeded here, we can return.
        // But if CVars were not found, Fallback might find them if Manager
        // lookup is broken? Unlikely. If Manager is valid, lookup should work.
      }
    }
  }
}
} // namespace UE5

// Centralized Hook Detection Logic (Executed by HookThread)
void CheckAndInstallHooks() {
  std::lock_guard<std::mutex> lock(g_HookMutex);

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
    // Also treat d3d12.dll/d3d11.dll presence as D3D evidence — UE5 loads
    // vulkan-1.dll even for DX12 games, and our D3D12CreateDevice wrapper may
    // not be installed yet if d3d12.dll loaded after our initial IAT scan.
    bool d3dDllPresent = (GetModuleHandleA("d3d12.dll") != nullptr) ||
                         (GetModuleHandleA("d3d11.dll") != nullptr);
    bool d3dDeviceCreated = WasD3D12DeviceCreated() || d3dDllPresent ||
                            WasD3D11Or10DeviceCreated() || legacyD3DLoaded;
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
  if (!s_vulkanActive) {
    InitializeWrapperHooks();
  }

  // DX12: Only initialize the hook instance for state tracking and
  // ExecuteCommandLists hooking. We do NOT install DXGI vtable hooks anymore -
  // wrappers handle Present/ResizeBuffers. NOTE: Skip for Vulkan games to
  // prevent DXGI interference
  {
    HMODULE hD3D12 = GetModuleHandleA("d3d12.dll");
    if (!s_vulkanActive && !g_DX12Hook && hD3D12 && WasD3D12DeviceCreated()) {
      HookLogImportant("Detected active D3D12 device usage. Initializing DX12 hook instance...");

      // STATIC DESTRUCTOR FIX: Dynamically allocate the hook instance
      if (!g_dx12HookInstance) {
        g_dx12HookInstance = new DX12Hook();
      }
      g_DX12Hook = g_dx12HookInstance;

      // Note: DX12Hook::Init() now only hooks ExecuteCommandLists for frame
      // detection. DXGI Present/Resize is handled by CWrapDXGISwapChain.
      g_DX12Hook->Init();
      HookLogImportant("DX12 hook instance ready (wrappers active)");
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
      (GetModuleHandleA("d3d11.dll") || GetModuleHandleA("d3d10.dll") ||
       GetModuleHandleA("d3d10_1.dll"));
  bool legacyD3DLoaded = (GetModuleHandleA("d3d9.dll") != nullptr) ||
                         (GetModuleHandleA("d3d8.dll") != nullptr) ||
                         (GetModuleHandleA("ddraw.dll") != nullptr);
  bool d3d11Or10DeviceCreated = WasD3D11Or10DeviceCreated();
  bool d3d12DeviceCreated = WasD3D12DeviceCreated();

  // NOTE: Skip D3D11 hooks for Vulkan games to prevent DXGI interference
  // Also avoid DX11 hook install in legacy D3D processes unless actual
  // D3D11/D3D10 device creation was observed (prevents DX9 interop false
  // positives when recording starts).
  if (!s_vulkanActive && !g_DX11Hook && d3d11Or10DllPresent &&
      (d3d11Or10DeviceCreated ||
       (!d3d12DeviceCreated && !legacyD3DLoaded))) {
    HookLog("Detected D3D10/11. Installing hooks... (D3D11/10 API called: %d, "
            "D3D12 API called: %d, LegacyD3D loaded: %d)",
            d3d11Or10DeviceCreated ? 1 : 0, d3d12DeviceCreated ? 1 : 0,
            legacyD3DLoaded ? 1 : 0);
    g_DX11Hook = new DX11Hook();
    g_DX11Hook->Init();
    HookLog("D3D10/11 hooks installed");
  }

  // For other APIs, skip if D3D12 was actually used (not just loaded).
  // d3d12.dll can be loaded by D3D11On12 even in non-DX12 apps.
  // We use the actual device creation flag instead of just DLL presence.
  bool dx12ActuallyUsed = WasD3D12DeviceCreated();

  // NOTE: Skip D3D9 hooks for Vulkan games
  if (!s_vulkanActive && !g_DX9Hook && !dx12ActuallyUsed &&
      GetModuleHandleA("d3d9.dll")) {
    EarlyLog("DX9 Hook Check: Installing DX9 hooks (d3d9.dll loaded, vulkanActive=%d, dx12Used=%d)",
             s_vulkanActive ? 1 : 0, dx12ActuallyUsed ? 1 : 0);
    HookLog("Detected d3d9.dll. Installing DX9 hooks...");
    g_DX9Hook = new DX9Hook();
    g_DX9Hook->Init();
    HookLog("DX9 hooks installed (hook ptr=%p)", (void*)g_DX9Hook);
  } else if (!g_DX9Hook && GetModuleHandleA("d3d9.dll")) {
    EarlyLog("DX9 Hook Check: Skipping DX9 hooks (vulkanActive=%d, dx12Used=%d)",
             s_vulkanActive ? 1 : 0, dx12ActuallyUsed ? 1 : 0);
  }

  // DirectDraw titles can still load or probe D3D12 through DXGI/driver helper
  // components. That must not suppress the actual DirectDraw hook path.
  if (!g_DDrawHook && GetModuleHandleA("ddraw.dll")) {
    HookLog("Detected ddraw.dll. Installing DirectDraw hooks... (dx12Used=%d)",
            dx12ActuallyUsed ? 1 : 0);
    g_DDrawHook = new DDrawHook();
    g_DDrawHook->Init();
    HookLog("DDraw hooks installed");
  }

  if (!g_DX8Hook && !dx12ActuallyUsed && GetModuleHandleA("d3d8.dll")) {
    HookLog("Detected d3d8.dll. Installing DX8 hooks...");
    g_DX8Hook = new DX8Hook();
    g_DX8Hook->Init();
    HookLog("DX8 hooks installed");
  }

  if (!g_OpenGLHook && !dx12ActuallyUsed && GetModuleHandleA("opengl32.dll")) {
    HookLog("Detected opengl32.dll. Installing OpenGL hooks...");
    g_OpenGLHook = new OpenGLHook();
    g_OpenGLHook->Init();
    HookLog("OpenGL hooks installed");
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
    // independently of DXGI (used by SpecialK and RTSS)
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

    // perf_metrics_logging now folds into debug_logging so a single switch
    // controls all hook-side diagnostics.
    if (g_pLocalConfig && g_pLocalConfig->debugLogging) {
      // Read session-specific logs path from DiscoveryInfo (set by inject process)
      std::string sessionLogsDir;
      HANDLE hDisc = OpenFileMappingW(FILE_MAP_READ, FALSE, SHARED_MEM_DISCOVERY);
      if (hDisc) {
        DiscoveryInfo *pDisc = (DiscoveryInfo *)MapViewOfFile(
            hDisc, FILE_MAP_READ, 0, 0, sizeof(DiscoveryInfo));
        if (pDisc && pDisc->GetMagic() == DISCOVERY_MAGIC && pDisc->logsPath[0]) {
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

  // Install RegQueryValueExW for DLSS Debug Overlay
  if (GetModuleHandleA("advapi32.dll")) {
    HookLog("Installing RegQueryValueExW hook via IAT patching...");
    IATHook::InitializeAdvapi32Hooks((void *)&HookedRegQueryValueExW,
                                     (void **)&OriginalRegQueryValueExW);
  } else {
    HookLog("advapi32.dll not loaded yet - skipping RegQueryValueExW hook");
  }

  // Only install the low-level loader hook when DLL redirection is actually
  // configured. Without redirection overrides it adds risk but no value.
  if (NeedsLoaderRedirectionHook()) {
    if (!OriginalLdrLoadDll.load(std::memory_order_acquire)) {
      if (HMODULE hNtdll = GetModuleHandleA("ntdll.dll")) {
        if (void *pLdrLoadDll = (void *)GetProcAddress(hNtdll, "LdrLoadDll")) {
          void *trampoline = nullptr;
          if (InlineHook::Install(pLdrLoadDll, (void *)&HookedLdrLoadDll,
                                  &trampoline)) {
            OriginalLdrLoadDll.store((LdrLoadDll_t)trampoline, std::memory_order_release);
            HookLog("Installed LdrLoadDll hook");
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
      if (pDisc->GetMagic() == DISCOVERY_MAGIC) {
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

// CBTHookProc REMOVED to prevent Steam overlay recursion crashes
// The CBT hook installation in inject_main.cpp is already commented out
// If we export CBTHookProc, Steam's hook can still find and call it even if we
// don't install it Removing the export breaks any lingering hook registrations
// extern "C" __declspec(dllexport) LRESULT CALLBACK CBTHookProc(...) { ... }

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
        if (pDisc && pDisc->GetMagic() == DISCOVERY_MAGIC && pDisc->logsPath[0]) {
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
      EarlyLog("DllMain: Process '%s' is a Whitelisted Game", fileName);
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
      if (hasGraphicsAPI) {
        EarlyLog("DllMain: Graphics API detected - initializing IAT hooks "
                 "immediately...");
        InitializeWrapperHooks();
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
