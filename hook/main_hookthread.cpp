#include "main_internal.h"

namespace {

void PublishLdrLoadDllTrampoline(void* trampoline, void*) {
  OriginalLdrLoadDll.store(reinterpret_cast<LdrLoadDll_t>(trampoline), std::memory_order_release);
}

}  // namespace

DWORD WINAPI HookThread(LPVOID lpParam) {
  g_HookThreadRunning = true;
  const bool inheritedRenderer =
      g_InheritedRendererProcess.load(std::memory_order_acquire);
  InitializeHookLifecycleControl();

  // Fast D3D-app coverage: install the DXGI factory + CreateSwapChainForHwnd hooks before any other
  // hook-thread work (module scans, IPC waits, periodic passes). A game that initializes D3D12
  // within the first second (e.g. dx12_fg_switch_test via Steam + RTSS, session 20260812_044326)
  // otherwise creates its swapchain before these hooks exist; in the leave-the-entry mode (two
  // or more foreign overlays) an unwrapped pre-existing swapchain means CE never sees a Present
  // and the overlay never appears. DX12Hook::Init retries this when dxgi.dll was not loaded yet.
  // A resident CE Vulkan layer is the one exception: it already owns final
  // presentation, so speculative DXGI patching would interfere with its ICD.
  const bool vulkanLayerModuleLoaded =
      GetModuleHandleW(L"VK_LAYER_CE_overlay.dll") != nullptr ||
      GetModuleHandleW(L"VK_LAYER_CE_overlay_x86.dll") != nullptr;
  if (ce::vulkan_renderer_policy::ShouldInstallEarlyD3DDXGIHooks(
          vulkanLayerModuleLoaded)) {
    InstallGlobalVTableHooks();
  } else {
    EarlyLog("HookThread: CaptureEngine Vulkan layer already loaded - skipping "
             "early DXGI factory/swapchain hooks");
  }

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
    // NVIDIA's Vulkan WSI can end at an internal DXGI flip swapchain. For the
    // explicit FIFO mode, register a narrow real-factory path that changes only
    // final Present synchronization arguments. This must happen after config is
    // loaded and before the process-wide GetProcAddress router is armed.
    ce::vulkan_dxgi_fifo::RegisterDynamicFactoryHooks(vulkanLayerModuleLoaded);
    // The RTX Remix bridge resolves its public interface initializer as soon as
    // the inherited renderer resumes. Register before any other helper can arm
    // the process-wide GetProcAddress router, so that first lookup is covered.
    RemixHook::RegisterDynamicHooks();
    ArmManualReflexQueryHookIfConfigured("config.ini");
    ArmNgxFgPresetOverrideIfConfigured("config.ini");

    // A late 1.x runtime may still be inside slInit while CE is arriving. Give
    // both generations the configured NGX SR/FG images before taking its imports
    // over; any legacy image that already won is retired after 1.x shutdown.
    if (!inheritedRenderer)
      PreloadConfiguredStreamlineBridgeNgxDlls();

    // With streamline_upgrade=on, run the configured Streamline 2.x runtime as a
    // second, CE-owned runtime beside a 1.x game's own and repoint the game's
    // sl.interposer imports at CE.
    //
    // This is the first import takeover the hook thread does once it has a config,
    // and the position is the point. A 1.x game reaches its own Streamline within a few
    // hundred milliseconds of CE's DllMain - session 20260821_151738 has it there
    // by 15:18:12.3, against a DllMain at 15:18:11.99 - and the previous position,
    // after the pre-termination dump hooks, was roughly 550 ms further on, most of
    // it spent quiescing peer threads to install inline hooks. Every stage between
    // here and there is work the bridge does not need and the game does not wait
    // for. It also has to stay ahead of the runtime preload below: both mechanisms
    // want the same configured folder for opposite purposes, and an active bridge
    // stands the sl.* substitution down entirely.
    //
    // The takeover itself is only import-table writes; loading and initialising
    // the 2.x runtime happens behind it, so arriving here early costs nothing and
    // a game call that lands mid-bring-up waits rather than races.
    if (!inheritedRenderer)
      ce::streamline_bridge::TryActivate();

    // User-configured third-party tools (Special K / ReShade / OptiScaler) must
    // be present before the game creates its first graphics device, so load
    // them as early as the hook can, ahead of CE's wrapper and runtime
    // preloads. Each tool is loaded through the original loader entry and
    // tracked by the overlay-compatibility registry.
    if (!inheritedRenderer)
      PreloadConfiguredThirdPartyDlls();

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
  if (main_g_ProcessCategory == ProcessCategory::Blacklisted) {
    CompleteInheritedRendererBootstrap(false);
    CloseCheckHooksEvent();
    return 0;
  }

  // --- LAUNCHERS ---
  if (main_g_ProcessCategory == ProcessCategory::Launcher) {
    // Launchers only need CreateProcess hooks. They still participate in the
    // host-generation lifecycle so closing CE makes those hooks exact
    // pass-through and a replacement CE can reactivate them without unloading
    // code that an IAT or foreign hook chain may retain.
    OriginalCreateProcessA.store((CreateProcessA_t)GetProcAddress(
        GetModuleHandleA("kernel32.dll"), "CreateProcessA"), std::memory_order_release);
    OriginalCreateProcessW.store((CreateProcessW_t)GetProcAddress(
        GetModuleHandleA("kernel32.dll"), "CreateProcessW"), std::memory_order_release);

    void *dummy;
    IATHook::PatchIATAllModules("kernel32.dll", "CreateProcessA",
                                (void *)&HookedCreateProcessA, &dummy);
    IATHook::PatchIATAllModules("kernel32.dll", "CreateProcessW",
                                (void *)&HookedCreateProcessW, &dummy);

    RunLauncherHookLifecycle();
    CompleteInheritedRendererBootstrap(false);
    CloseCheckHooksEvent();
    g_HookThreadRunning = false;
    return 0;
  }

  // POTENTIAL GAMES
  EarlyLog("HookThread: Potential game detected. Watchdog started.");

  // Init IPC loop
  g_IPC = new IPCClient();

  if (main_g_isSkippedProcess) {
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
        main_g_isSkippedProcess = false;
        break;
      }

      if (g_IPC->Connect()) {
        Sleep(1000); // 1s is aggressive enough without being a CPU hog/bomb
      } else {
        if (!DeactivateHookRuntimeAndWaitForHost("initial host unavailable", true)) {
          CompleteInheritedRendererBootstrap(false);
          CloseCheckHooksEvent();
          return 0;
        }
      }
      Sleep(1000);
    }
  }

  EarlyLog("HookThread: [%s] IPCClient created, attempting connect...",
           g_ProcessName);
  bool ipcConnected = g_IPC->Connect();
  if (!ipcConnected) {
    EarlyLog("HookThread: Initial IPC connection unavailable; entering dormant wait");
    ipcConnected = DeactivateHookRuntimeAndWaitForHost("initial IPC connection unavailable", true);
  }
  if (ipcConnected) {
    EarlyLog("HookThread: IPC Connected successfully!");
    HookLog("IPC Connected successfully!");

    if (g_IPC->GetSharedMem()) {
      g_pSharedMem = g_IPC->GetSharedMem();
      SyncInheritedRendererRuntimeConfig(g_pSharedMem);
      if (ce::vulkan_renderer_policy::ShouldPublishHookAsSource(
              g_InheritedRendererProcess.load(std::memory_order_acquire))) {
        g_pSharedMem->SetSourcePid(GetCurrentProcessId());
      } else {
        HookLogImportant(
            "Inherited renderer: preserving profiled parent source PID %u",
            g_pSharedMem->GetSourcePid());
      }
      ArmManualReflexQueryHookIfConfigured("shared memory");
      ArmNgxFgPresetOverrideIfConfigured("shared memory");
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
    CompleteInheritedRendererBootstrap(false);
    CloseCheckHooksEvent();
    g_HookThreadRunning = false;
    return 0;
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
  RemixHook::RegisterDynamicHooks();
  RemixHook::Install();
  IATHook::InitializeGetProcAddressHook();
  TryInstallFatalTerminationDumpHooks();

  // When the profile configures runtime override paths (dlss_sr_dll_path,
  // dlss_fg_dll_path, dlss_rr_dll_path, streamline_dll_path), load the override
  // copies NOW so name-based loads later resolve to them instead of the game's
  // own (often older) runtime. The LoadLibrary redirect alone cannot cover
  // Streamline-internal loads, which run through the IAT of modules that load
  // after this snapshot pass.
  PreloadConfiguredGraphicsRuntimeDlls();

  // Answer the NGX ShowDlssIndicator registry probe for dlss_debug_overlay.
  // This must be an inline hook on the shared advapi32 exports, not an IAT
  // patch: nvngx_dlss.dll / nvngx_dlssg.dll are the modules that read the value
  // and they load minutes into a session, long after any IAT snapshot taken
  // here would have been applied.
  if (CurrentProcessOwnsProcessLocalRuntimeOverrides()) {
    ce::dlss_indicator::Install(ce::dlss_indicator::ParseMode(
        g_pLocalConfig ? g_pLocalConfig->graphics.dlssDebugOverlay : std::string()));
  } else {
    HookLogImportant(
        "DLSS indicator: skipped because an inherited child renderer owns the "
        "process-local registry probe");
  }

  // Retain inject-side coverage for OpenGL and already-loaded ICDs. Vulkan's
  // ordinary path is patched earlier by the Vulkan layer, before vkCreateDevice.
  ce::nv_lod_spread::Install(GetActiveGraphicsConfig().nvLodSpreadFix
                                 ? ce::nv_lod_spread::Mode::kOn
                                 : ce::nv_lod_spread::Mode::kOff);

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
          if (InlineHook::InstallPublished(pLdrLoadDll, (void *)&HookedLdrLoadDll,
                                           &trampoline, PublishLdrLoadDllTrampoline, nullptr)) {
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

  // Install the graphics hooks before any optional engine-memory discovery.
  // UE5 CVar discovery can take several seconds in large shipping modules;
  // running it first lets the game create its initial swapchain before the
  // DXGI queue-capture hooks exist and strands the PostSL overlay without an
  // authoritative queue.
  CheckAndInstallHooks();
  MarkHookLifecycleBootstrapComplete();
  CompleteInheritedRendererBootstrap(true);

  const GraphicsConfig initialGraphicsConfig = GetActiveGraphicsConfig();
  UE5::RefreshOverrides(initialGraphicsConfig);

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
    const GraphicsConfig activeGraphicsConfig = GetActiveGraphicsConfig();

    // Process deferred releases (D3D11) on background thread
    // This prevents render thread stalls when destroying capture resources
    if (g_DX11Hook)
      g_DX11Hook->ProcessDeferredReleases();

    bool periodicHookCheckDue = (now - lastPeriodicHookCheck) >= 1000;
    if (waitResult == WAIT_OBJECT_0 || periodicHookCheckDue) {
      if (periodicHookCheckDue) {
        lastPeriodicHookCheck = now;
      }
      // Event signaled or periodic tick - run detection
      RefreshThirdPartyOverlayIdentityCache();
      CheckAndInstallHooks();
    }

    // A Present-hook install postponed because a third-party overlay owned the
    // swapchain creation path during startup has to be retried from here; the
    // startup window it is waiting out ends without any further hook callback.
    if (g_DX12Hook)
      g_DX12Hook->ServicePendingPresentHooks();

    // Keep graphics/module hook installation ahead of the optional UE5 module
    // scan on every service pass as well as during initial startup.
    UE5::RefreshOverrides(activeGraphicsConfig);

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
      if (!DeactivateHookRuntimeAndWaitForHost("host requested shutdown", false)) {
        break;
      }
      s_WasRecording = false;
      lastPeriodicHookCheck = GetTickCount();
      continue;
    }

    if (hostPID != 0) {
      HANDLE hHost = OpenProcess(SYNCHRONIZE, FALSE, hostPID);
      if (hHost) {
        DWORD waitResultHost = WaitForSingleObject(hHost, 0); // Immediate check
        CloseHandle(hHost);
        if (waitResultHost == WAIT_OBJECT_0) {
          EarlyLog("HookThread: Host process died");
          HookLog("Host process died. Cleaning up...");
          if (!DeactivateHookRuntimeAndWaitForHost("host process exited", true)) {
            break;
          }
          s_WasRecording = false;
          lastPeriodicHookCheck = GetTickCount();
          continue;
        }
      } else {
        if (g_IPC->GetSharedMem()) {
          EarlyLog("HookThread: Can't open host process, assuming dead");
          HookLog("Host process inaccessible. Exiting...");
          if (!DeactivateHookRuntimeAndWaitForHost("host process inaccessible", true)) {
            break;
          }
          s_WasRecording = false;
          lastPeriodicHookCheck = GetTickCount();
          continue;
        }
      }
    } else {
      if (!DeactivateHookRuntimeAndWaitForHost("host identity unavailable", true)) {
        break;
      }
      s_WasRecording = false;
      lastPeriodicHookCheck = GetTickCount();
      continue;
    }
  }

  UE5::ShutdownOverrides();

  // Cleanup Event
  CloseCheckHooksEvent();

  // Only lifecycle-event failure reaches here. Normal host shutdown leaves the
  // thread resident and dormant so game-held wrappers and foreign saved hook
  // targets remain callable.
  g_HookThreadRunning = false;
  return 0;
}

bool isProcessWhitelistedFast(const char *name) {
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
          const size_t remaining = static_cast<size_t>(end - p);
          const size_t length = strnlen(p, remaining);
          if (length == remaining)
            break;
          if (_stricmp(name, p) == 0) {
            found = true;
            break;
          }
          p += length + 1;
        }

        // A Vulkan layer may have proven that this exact process is the direct
        // child renderer of the published profile target. Accept only that PID;
        // the executable name itself is not added to the whitelist.
        if (!found) {
          wchar_t sharedMemName[64] = {};
          GenerateSharedMemName(sharedMemName, _countof(sharedMemName),
                                pDisc->GetInjectPid());
          HANDLE sharedMapping =
              OpenFileMappingW(FILE_MAP_READ, FALSE, sharedMemName);
          if (sharedMapping) {
            auto *sharedMemory = static_cast<SharedMemoryLayout *>(MapViewOfFile(
                sharedMapping, FILE_MAP_READ, 0, 0, sizeof(SharedMemoryLayout)));
            if (ValidateSharedMemory(sharedMemory) &&
                ce::vulkan_renderer_policy::IsPublishedInheritedRenderer(
                    GetCurrentProcessId(),
                    sharedMemory->runtimeState.inheritedRendererProcessPid.load(
                        std::memory_order_acquire))) {
              found = true;
              g_InheritedRendererProcess.store(true, std::memory_order_release);
            }
            if (sharedMemory)
              UnmapViewOfFile(sharedMemory);
            CloseHandle(sharedMapping);
          }
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
bool IsServiceProcess(const char *name) {
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
