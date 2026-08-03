  //      app)
  bool d3d11Or10DllPresent =
      (!dxvkD3D11WrapperLoaded &&
       (GetModuleHandleA("d3d11.dll") || GetModuleHandleA("d3d10.dll") ||
        GetModuleHandleA("d3d10_1.dll")));
  bool legacyD3DLoaded = (GetModuleHandleA("d3d9.dll") != nullptr) ||
                         (GetModuleHandleA("d3d8.dll") != nullptr) ||
                         (GetModuleHandleA("ddraw.dll") != nullptr);
  bool d3d11Or10DeviceCreated = !dxvkD3D11WrapperLoaded && WasD3D11Or10DeviceCreated();
  bool d3d12DeviceCreated = WasD3D12DeviceCreated();

  // Log third-party overlay presence for diagnostics
  {
    HMODULE hGameoverlay = GetModuleHandleA("gameoverlayrenderer.dll");
    if (hGameoverlay) {
      char overlayPath[MAX_PATH] = {};
      GetModuleFileNameA(hGameoverlay, overlayPath, MAX_PATH);
      HookLog("Third-party overlay detected: gameoverlayrenderer.dll (%s)", overlayPath);
    }
  }

  // NOTE: Skip D3D11 hooks for Vulkan games to prevent DXGI interference
  // Also avoid DX11 hook install in legacy D3D processes unless actual
  // D3D11/D3D10 device creation was observed (prevents DX9 interop false
  // positives when recording starts).
  //
  // Many DX11 games load d3d9.dll as a transitive dependency (for audio
  // codecs, Windows version checks, etc.) without using it for rendering.
  // The old (!d3d12DeviceCreated && !legacyD3DLoaded) fallback was too
  // conservative — it prevented DX11 hook installation in DX11 games that
  // happened to have d3d9.dll loaded.  Now we install DX11 hooks whenever
  // d3d11/d3d10 is present and D3D12 was NOT actually used (legacyD3DLoaded
  // is no longer a blocker: a true DX9-only game never hits DX11 hook paths
  // because it never calls D3D11 functions).
  {
    static int s_dx11CheckCount = 0;
    ++s_dx11CheckCount;
    bool dx11CondVulkanOk = !s_vulkanActive;
    bool dx11CondNoHookYet  = !g_DX11Hook;
    bool dx11CondDllPresent = d3d11Or10DllPresent;
    bool dx11CondDeviceOk   = (d3d11Or10DeviceCreated || !d3d12DeviceCreated);
    bool dx11CondAll        = dx11CondVulkanOk && dx11CondNoHookYet && dx11CondDllPresent && dx11CondDeviceOk;
    if (s_dx11CheckCount <= 5 || dx11CondAll) {
      HookLogImportant(
          "DX11 check #%d: vulkan=%d noHook=%d dllPresent=%d device=%d legacy=%d "
          "d3d12Created=%d => %s",
          s_dx11CheckCount, s_vulkanActive ? 1 : 0, g_DX11Hook ? 1 : 0,
          d3d11Or10DllPresent ? 1 : 0, d3d11Or10DeviceCreated ? 1 : 0,
          legacyD3DLoaded ? 1 : 0, d3d12DeviceCreated ? 1 : 0,
          dx11CondAll ? "INSTALL" : "skip");
    }
    if (dx11CondAll) {
      HookLog("Detected D3D10/11. Installing hooks... (D3D11/10 API called: %d, "
              "D3D12 API called: %d, LegacyD3D loaded: %d)",
              d3d11Or10DeviceCreated ? 1 : 0, d3d12DeviceCreated ? 1 : 0,
              legacyD3DLoaded ? 1 : 0);
      g_DX11Hook = new DX11Hook();
      LARGE_INTEGER _t1, _t2, _freq;
      QueryPerformanceFrequency(&_freq);
      QueryPerformanceCounter(&_t1);
      g_DX11Hook->Init();
      QueryPerformanceCounter(&_t2);
      // NOLINTNEXTLINE(bugprone-narrowing-conversions) - intentional narrowing; value is range-bounded by the surrounding API/geometry contract
      double _initMs = (double)(_t2.QuadPart - _t1.QuadPart) * 1000.0 / _freq.QuadPart;
      HookLog("D3D10/11 hooks installed (init=%.1f ms)", _initMs);
    }
  }

  // For other APIs, skip if D3D12 was actually used (not just loaded).
  // d3d12.dll can be loaded by D3D11On12 even in non-DX12 apps.
  // We use the actual device creation flag instead of just DLL presence.
  bool dx12ActuallyUsed = WasD3D12DeviceCreated();

  // NOTE: Skip D3D9 hooks for Vulkan games, except DXVK D3D9. That path still
  // needs the DX9 hook for game-thread Present pacing and overlay integration
  // while Vulkan remains the primary capture path.
  // Also skip D3D9 hooks for DX11 games — d3d9.dll is often a transitive system
  // dependency (audio codecs, Windows version checks) in DX11 titles.
  const bool dx11DllLoaded = GetModuleHandleA("d3d11.dll") != nullptr;
  if ((!s_vulkanActive || dxvkD3D9WrapperLoaded) && !g_DX9Hook && !dx12ActuallyUsed && !dx11DllLoaded &&
      GetModuleHandleA("d3d9.dll")) {
    EarlyLog(
        "DX9 Hook Check: Installing DX9 hooks (d3d9.dll loaded, vulkanActive=%d, dx12Used=%d, dxvkD3D9=%d)",
        s_vulkanActive ? 1 : 0, dx12ActuallyUsed ? 1 : 0, dxvkD3D9WrapperLoaded ? 1 : 0);
    HookLog("Detected d3d9.dll. Installing DX9 hooks...");
    g_DX9Hook = new DX9Hook();
    LARGE_INTEGER _t1, _t2, _freq;
    QueryPerformanceFrequency(&_freq);
    QueryPerformanceCounter(&_t1);
    g_DX9Hook->Init();
    QueryPerformanceCounter(&_t2);
    // NOLINTNEXTLINE(bugprone-narrowing-conversions) - intentional narrowing; value is range-bounded by the surrounding API/geometry contract
    double _initMs = (double)(_t2.QuadPart - _t1.QuadPart) * 1000.0 / _freq.QuadPart;
    HookLog("DX9 hooks installed (hook ptr=%p, init=%.1f ms)", (void*)g_DX9Hook, _initMs);
  } else if (!g_DX9Hook && GetModuleHandleA("d3d9.dll")) {
    EarlyLog("DX9 Hook Check: Skipping DX9 hooks (vulkanActive=%d, dx12Used=%d, dx11Loaded=%d, dxvkD3D9=%d)",
             s_vulkanActive ? 1 : 0, dx12ActuallyUsed ? 1 : 0, dx11DllLoaded ? 1 : 0, dxvkD3D9WrapperLoaded ? 1 : 0);
  }

  // DirectDraw titles can still load or probe D3D12 through DXGI/driver helper
  // components. That must not suppress the actual DirectDraw hook path.
  // Skip DirectDraw hooks when the Vulkan layer owns presentation. In the DXVK
  // D3D9 case the DX9 hook stays active, and synthesizing DirectDraw objects on
  // our worker thread can recurse into external overlays and crash.
  // Also skip DDraw hooks when a higher-level D3D API (d3d9, d3d8) is present,
  // because ddraw.dll is often a transitive system dependency and bootstrapping
  // DDraw (which internally creates a D3D9 device) can crash third-party overlays
  // that have already hooked Direct3DCreate9 (see DDrawHook::Init for details).
  if (!s_vulkanActive && !g_DDrawHook && GetModuleHandleA("ddraw.dll") &&
      !GetModuleHandleA("d3d9.dll") && !GetModuleHandleA("d3d8.dll")) {
    HookLog("Detected ddraw.dll. Installing DirectDraw hooks... (dx12Used=%d)",
            dx12ActuallyUsed ? 1 : 0);
    g_DDrawHook = new DDrawHook();
    LARGE_INTEGER _t1, _t2, _freq;
    QueryPerformanceFrequency(&_freq);
    QueryPerformanceCounter(&_t1);
    g_DDrawHook->Init();
    // NOLINTNEXTLINE(bugprone-narrowing-conversions) - intentional narrowing; value is range-bounded by the surrounding API/geometry contract
    QueryPerformanceCounter(&_t2);
    // NOLINTNEXTLINE(bugprone-narrowing-conversions) - intentional narrowing; value is range-bounded by the surrounding API/geometry contract
    double _initMs = (double)(_t2.QuadPart - _t1.QuadPart) * 1000.0 / _freq.QuadPart;
    HookLog("DDraw hooks installed (init=%.1f ms)", _initMs);
  } else if (!s_vulkanActive && !g_DDrawHook && GetModuleHandleA("ddraw.dll")) {
    HookLog("DDraw hooks skipped (higher-level D3D API present: d3d9=%d d3d8=%d)",
            GetModuleHandleA("d3d9.dll") ? 1 : 0,
            GetModuleHandleA("d3d8.dll") ? 1 : 0);
  }

  if (!g_DX8Hook && !dx12ActuallyUsed && GetModuleHandleA("d3d8.dll")) {
    HookLog("Detected d3d8.dll. Installing DX8 hooks...");
    g_DX8Hook = new DX8Hook();
    LARGE_INTEGER _t1, _t2, _freq;
    QueryPerformanceFrequency(&_freq);
    QueryPerformanceCounter(&_t1);
    // NOLINTNEXTLINE(bugprone-narrowing-conversions) - intentional narrowing; value is range-bounded by the surrounding API/geometry contract
    g_DX8Hook->Init();
    // NOLINTNEXTLINE(bugprone-narrowing-conversions) - intentional narrowing; value is range-bounded by the surrounding API/geometry contract
    QueryPerformanceCounter(&_t2);
    double _initMs = (double)(_t2.QuadPart - _t1.QuadPart) * 1000.0 / _freq.QuadPart;  // NOLINT(bugprone-narrowing-conversions)
    HookLog("DX8 hooks installed (init=%.1f ms)", _initMs);
  }

  if (!g_OpenGLHook && !dx12ActuallyUsed && GetModuleHandleA("opengl32.dll")) {
    HookLog("Detected opengl32.dll. Installing OpenGL hooks...");
    g_OpenGLHook = new OpenGLHook();
    LARGE_INTEGER _t1, _t2, _freq;
    QueryPerformanceFrequency(&_freq);
    // NOLINTNEXTLINE(bugprone-narrowing-conversions) - intentional narrowing; value is range-bounded by the surrounding API/geometry contract
    QueryPerformanceCounter(&_t1);
    // NOLINTNEXTLINE(bugprone-narrowing-conversions) - intentional narrowing; value is range-bounded by the surrounding API/geometry contract
    g_OpenGLHook->Init();
    QueryPerformanceCounter(&_t2);
    double _initMs = (double)(_t2.QuadPart - _t1.QuadPart) * 1000.0 / _freq.QuadPart;  // NOLINT(bugprone-narrowing-conversions)
    HookLog("OpenGL hooks installed (init=%.1f ms)", _initMs);
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
    // independently of DXGI (a common VRAM-reporting override technique)
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
    ArmManualReflexQueryHookIfConfigured("config.ini");

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

    // Seed third-party-overlay detection and register the loader-safe DLL load/unload
    // notification. Done here (HookThread, after DllMain) so the seed's one-time loader walk
    // and the registration are off both the DllMain loader lock and the Present hot path.
    InitializeThirdPartyOverlayDetection();

    // perf_metrics_logging now folds into debug_logging so a single switch
    // controls all hook-side diagnostics.
    if (g_pLocalConfig && IsTraceLoggingEnabled(g_pLocalConfig->logLevel)) {
      // Read session-specific logs path from DiscoveryInfo (set by inject process)
      std::string sessionLogsDir;
      HANDLE hDisc = OpenFileMappingW(FILE_MAP_READ, FALSE, SHARED_MEM_DISCOVERY);
      if (hDisc) {
        DiscoveryInfo *pDisc = (DiscoveryInfo *)MapViewOfFile(
            hDisc, FILE_MAP_READ, 0, 0, sizeof(DiscoveryInfo));
        if (ValidateDiscoveryInfo(pDisc) && pDisc->logsPath[0]) {
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
      ArmManualReflexQueryHookIfConfigured("shared memory");
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

  // GTA and some middleware can terminate with fail-fast style status codes
  // before VEH/UEF crash filters get control. Keep this narrow and passive:
  // one CE-owned dump for current-process fatal exits, then forward.
  FFXHook::RegisterDynamicHooks();
  IATHook::InitializeGetProcAddressHook();
  TryInstallFatalTerminationDumpHooks();

  // Install RegQueryValueExW for DLSS Debug Overlay
  if (GetModuleHandleA("advapi32.dll")) {
    HookLog("Installing RegQueryValueExW hook via IAT patching...");
    IATHook::InitializeAdvapi32Hooks((void *)&HookedRegQueryValueExW,
                                     (void **)&OriginalRegQueryValueExW);
  } else {
    HookLog("advapi32.dll not loaded yet - skipping RegQueryValueExW hook");
  }

  // Install the low-level loader hook for DLL redirection and early module-load
  // observation. GTA Enhanced can bring up the official FFX runtime through a
  // path that reaches CE's periodic scan only after ffxConfigure has already
  // been cached, which prevents the native FSR present-callback bridge from
  // arming.
  if (NeedsLoaderRedirectionHook() || NeedsLowLevelModuleLoadObservationHook()) {
    if (!OriginalLdrLoadDll.load(std::memory_order_acquire)) {
      if (HMODULE hNtdll = GetModuleHandleA("ntdll.dll")) {
        if (void *pLdrLoadDll = (void *)GetProcAddress(hNtdll, "LdrLoadDll")) {
          void *trampoline = nullptr;
          if (InlineHook::Install(pLdrLoadDll, (void *)&HookedLdrLoadDll,
                                  &trampoline)) {
            OriginalLdrLoadDll.store((LdrLoadDll_t)trampoline, std::memory_order_release);
            HookLogImportant("Installed LdrLoadDll hook for module-load observation and optional DLL redirection");
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
      if (ValidateDiscoveryInfo(pDisc)) {
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
