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
        if (ValidateDiscoveryInfo(pDisc) && pDisc->logsPath[0]) {
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
      if (HMODULE hDbgHelp = GetModuleHandleA("dbghelp.dll")) {
        TryInstallMiniDumpWriteDumpHookForModule(hDbgHelp, "dbghelp.dll");
      }
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
      EarlyLog("DllMain: Process '%s' is a whitelisted hook target", fileName);
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
      if (hasGraphicsAPI && !IsDXVKD3D11WrapperLoaded()) {
        EarlyLog("DllMain: Graphics API detected - initializing IAT hooks "
                 "immediately...");
        InitializeWrapperHooks();
      } else if (hasGraphicsAPI) {
        EarlyLog("DllMain: DXVK d3d11 detected - skipping immediate DXGI/D3D wrapper init");
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
    ShutdownScreenshotWorker();

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
