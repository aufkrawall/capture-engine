
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
