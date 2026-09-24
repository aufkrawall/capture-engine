#include "main_internal.h"

#include "apis/streamline_ota_preferences.h"
#include "common/child_inject_policy.h"
#include "common/ngx_ota_runtime.h"
#include "common/published_graphics_config.h"

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
    //
    // The same ordering makes this the earliest point at which CE's own state
    // starts disappearing, so latch the runtime dormant here as well: from here
    // on every hook entry point that consults HookIsShuttingDown() forwards
    // straight to its original, and none of them can read a global that this
    // module's (or the CRT's) teardown has already released.
    std::atexit([]() {
      g_ProcessTerminating.store(true, std::memory_order_release);
      RequestHookShutdown();
    });

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

    // Residency barrier: games can retain CE COM wrappers and other injectors
    // can save CE detour addresses in their own trampolines. Unmapping this DLL
    // while either pointer remains callable is inherently unsafe, so deject is
    // an acknowledged pass-through transition and the image remains resident
    // until process exit.

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
    wchar_t moduleDirW[MAX_PATH] = {0};
    if (ce::child_inject_policy::GetHookModuleDirectoryW(hinstDLL, moduleDirW, MAX_PATH)) {
      std::filesystem::path captureEngineDir(moduleDirW);
      // If we're in testapp directory, navigate to captureengine instead
      if (captureEngineDir.filename() == L"testapp") {
        captureEngineDir = captureEngineDir.parent_path() / L"captureengine";
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

      if (crashDir.empty()) {
        const std::filesystem::path logsDir = captureEngineDir / L"logs";
        CreateDirectoryW(logsDir.c_str(), NULL);
        // The crash handler converts its directory from UTF-8 at the WER
        // boundary, so the derived path is handed over in UTF-8 - a narrow
        // conversion here would '?'-mangle non-ACP install paths twice.
        crashDir = ce::child_inject_policy::NarrowFromWideUtf8(logsDir.c_str());
      } else {
        CreateDirectoryA(crashDir.c_str(), NULL);
      }
    } else {
      crashDir = ".\\logs";
      CreateDirectoryA(crashDir.c_str(), NULL);
    }
    // DllMain runs under the loader lock: never stage installed symbols here.
    // The controller archives them into the session directory it owns.
    SetCrashDumpDirectory(crashDir, /*archiveInstalledSymbols=*/false);

    // CRITICAL FIX: Install crash handler IMMEDIATELY for all non-service
    // processes Don't wait for whitelist check or graphics DLL detection -
    // crashes happen during early initialization before those are available
    // Install crash handler for all non-service processes
    // (Injection delay in captureengine prevents D3D12 init crashes)
    if (!IsServiceProcess(fileName)) {
      // Publish the external dump helper before the handler can ever fire: a
      // crash dump written from inside a host process that carries foreign
      // overlay hooks suspends every thread for as long as dbghelp needs to
      // walk the module list through them.
      RegisterCrashDumpEnvironmentHooksForHook();
      InstallCrashHandler();
      if (HMODULE hDbgHelp = GetModuleHandleA("dbghelp.dll")) {
        TryInstallMiniDumpWriteDumpHookForModule(hDbgHelp, "dbghelp.dll");
      }
      OutputDebugStringA("[CaptureHook] Crash handler installed\n");
    }

    // 1. SAFE UNLOAD: Services and non-interactive helpers
    // These processes should unload the DLL immediately and cleanly.
    if (IsServiceProcess(fileName)) {
      main_g_isDormant = true;
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

      main_g_isDormant = true;
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
      InitializeInheritedRendererBootstrapSignal();
    } else {
      // Not whitelisted - assume blacklist
      main_g_ProcessCategory = ProcessCategory::Blacklisted;
      main_g_isDormant = true;

      // DORMANT MODE: We return TRUE to stay loaded but remain completely
      // inert. Returning FALSE (unloading) triggers a "Loader Loop" where
      // Windows continuously re-injects the CBT hook for every window event,
      // causing massive system slowdowns. By staying loaded but doing nothing
      // (no threads, no hooks), we eliminate this overhead. EarlyLog("DllMain:
      // Process '%s' Blacklisted (Dormant Mode)", fileName);
      return TRUE;
    }

    if (main_g_isDormant) {
      // Silent return
      return TRUE;
    }

    if (main_g_ProcessCategory == ProcessCategory::PotentialGame) {
      if (!(GetModuleHandleA("d3d12.dll") || GetModuleHandleA("d3d11.dll") ||
            GetModuleHandleA("d3d9.dll") || GetModuleHandleA("vulkan-1.dll") ||
            GetModuleHandleA("opengl32.dll") || GetModuleHandleA("d3d8.dll"))) {
        main_g_isSkippedProcess = true;
        EarlyLog(
            "DllMain: Process '%s' skipped (No Graphics API modules found)",
            fileName);
      }
    }

    if (main_g_ProcessCategory != ProcessCategory::InternalTool) {
      // Loader and process-creation hooks FIRST, ahead of the graphics IAT work
      // below. Both are the same class of operation (resolve an export in an
      // already-loaded module, write import slots), but they differ in how fast
      // their value decays: a module that maps before the loader hook exists can
      // never be redirected, while the graphics hooks retry and self-heal. The
      // hook thread repeats this pass for modules that map later.
      InstallKernel32LoaderHooks("DllMain");

      // Publish `__NGX_DISABLE_UPDATER` now, from the mode the injector already
      // put in shared memory, rather than waiting for the hook thread's config
      // load. The refusal above is the backstop; this is the mechanism that
      // stops the NGX core from ever attempting a launch, and it only works if
      // the variable is in place before `_nvngx.dll` reads its environment.
      // Session 20260918_224737 published it at 22:47:47.670 against a DllMain
      // at 22:47:47.137, which is why that session refused nine launches
      // instead of seeing none. Shared-memory read plus SetEnvironmentVariable,
      // so it loads nothing.
      ce::ngx_ota::ApplyEarlyPolicyFromPublishedConfig();

      // Arm the loader redirect here too, from the same published config, and
      // for the same reason one layer over: the LoadLibrary hooks above go in
      // now, but `g_pLocalConfig` does not exist until the hook thread has read
      // config.ini ~400 ms later, so every load in between reached CE's hook
      // and was answered "no override" because the policy was missing rather
      // than because it said no. `sl.common` is loaded dynamically by
      // `sl.interposer`, exactly once, and losing it disables the whole sl.*
      // redirect family - so one load in that window costs the feature.
      ce::published_config::ResolveEarlyRuntimeOverridePaths();

      // The ngx_ota=off slInit route belongs here for the same reason, and it
      // was measurably too late anywhere else. Session 20260918_224737: CE's
      // DllMain ran at 22:47:47.137, the route went in from the hook thread's
      // config load at 22:47:47.688, and the verdict read
      // "slInit route installed=1, slInit seen through CE=0" - the game's call
      // landed in that 551 ms gap.
      //
      // Note that `sl.interposer.dll` being a static import of the exe does NOT
      // put slInit out of reach: the static import governs when the MODULE is
      // mapped, which is during process initialisation, while `slInit` is a
      // function the game calls from its own startup code well after the entry
      // point. Those are different events, and conflating them is what made this
      // look unfixable.
      //
      // Nothing here needs the hook thread: the interposer is already mapped
      // (its file version answers the generation) and the OTA mode resolves from
      // the injector's published shared memory. Both are reads; neither loads
      // anything, so this is as loader-lock-safe as the IAT work above.
      ce::streamline_ota::InstallSlInitRouteIfConfigured();

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

      const bool vulkanLayerModuleLoaded =
          GetModuleHandleW(L"VK_LAYER_CE_overlay.dll") != NULL ||
          GetModuleHandleW(L"VK_LAYER_CE_overlay_x86.dll") != NULL;

      // Initialize hooks for D3D/OpenGL processes. A resident CE Vulkan layer
      // already owns graphics interception and must remain the only path.
      if (hasGraphicsAPI &&
          ce::vulkan_renderer_policy::ShouldInstallEarlyD3DDXGIHooks(
              vulkanLayerModuleLoaded) &&
          !IsDXVKD3D11WrapperLoaded()) {
        EarlyLog("DllMain: Graphics API detected - initializing IAT hooks "
                 "immediately...");
        InitializeWrapperHooks();
      } else if (hasGraphicsAPI && vulkanLayerModuleLoaded) {
        EarlyLog("DllMain: CaptureEngine Vulkan layer already owns the process - "
                 "skipping immediate D3D/DXGI wrapper init");
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
        // The service loop is latency-tolerant housekeeping. Keeping the new
        // thread's normal priority prevents it from preempting game/runtime
        // presenter threads according to random per-process core placement.
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
      // Process exit runs the rest of LdrShutdownProcess after this call: this
      // module's globals get destroyed, then the CRT tears its heap down, while
      // CE's hooks stay installed and other modules' detach routines keep
      // calling through them (version.dll resolves resource-only images with
      // LoadLibraryExW, foreign overlays load and probe modules from their own
      // detach paths). Latching the runtime dormant here is a lock-free atomic
      // store and makes every guarded entry point an exact pass-through for the
      // remainder of teardown. Without it, HookedLoadLibraryEx* still resolved
      // redirects and faulted on the already-released config
      // (session 20260817_052857).
      RequestHookShutdown();
      return TRUE;
    }

    // No loader-lock cleanup is safe here. Normal CE shutdown never unloads the
    // module; it uses the cooperative dormant transition on HookThread. If a
    // foreign component nevertheless drives a dynamic detach, make all entry
    // points pass-through and leave process-owned resources for OS teardown.
    g_ProcessTerminating.store(true, std::memory_order_release);
    RequestHookShutdown();
  }
  return TRUE;
}
