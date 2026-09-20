# llm-wiki Log Archive (2026-09-19)

### 2026-09-19 - Streamline Reflex/PCL retry quiescence thrashing and 2.5s frametime spike fix

Diagnosed regular 65-110 ms frame time spikes occurring every 2.50 seconds (~348 frames at ~138 FPS) in Talos
Principle 2 (session `20260919_204420`).
- **Root cause:** `RetryResolveReflexFeatureHooksForRuntimeActivity` fired every 2500 ms on the game render
  thread (`slDLSSGGetState`/`slDLSSGSetOptions`). Because Reflex and PCL inline hooks failed to patch in Talos,
  `AreReflexFeatureHooksComplete()` was permanently false. On every tick, 13 consecutive `InlineHook::Install`
  attempts were executed; each invoked `ThreadQuiescence` (`SuspendThread` on ~60 threads in UE5), stalling the game
  render thread for 65–110 ms and leaking executable trampoline memory pools (~48 MB across 752 attempts).
- **Generic Solution:**
  1. **Policy module (`streamline_feature_retry_policy.h`):**
     - `ShouldAttemptInlineHookOnTarget`: bails out before invoking thread quiescence if a target already failed
       2 attempts (`kMaxInlineHookAttemptsPerTarget`).
     - `IsReflexFeatureResolutionComplete` & `IsPclFeatureResolutionComplete`: treats hooks as complete when
       either successfully hooked, failed max hook attempts, or exceeded query unavailability limit (`kReflexFeatureQueryUnavailableLimit = 3`).
     - `ShouldRetryRuntimeReflexResolution`: hard-caps late runtime retries at 6 attempts (`kMaxRuntimeReflexRetryAttempts`),
       enforces the 2500 ms cooldown, and stops permanently once complete or attempt ceiling is reached.
  2. **Thread Quiescence & Diagnostic Logging (`inline_hook.cpp`):**
     - Added diagnostics logging `quiescence.FailureReason()` and OS error codes when `VirtualProtect` fails.
     - Fixed memory leak on failed hook installations by deallocating unused trampolines instead of abandoning executable pools.
  3. **Scan Optimization (`streamline_hook_resolve.cpp` & `streamline_hook_install.cpp`):**
     - When `sl.reflex.dll` is already loaded, skips expensive `CreateToolhelp32Snapshot` module scans.
     - Records inspected modules in `streamline_hook_g_InstalledModuleMask` even when no core exports are present,
       preventing redundant export/IAT re-inspection on every scan.
     - Re-arms retry attempts on fresh module load/unload events for safe dynamic module handling.


### 2026-09-19 - NGX updater supervisor watchdog and target prewarming

When a title like Alan Wake 2 imports `sl.interposer` statically, `_nvngx.dll` can spawn `nvngx_update.exe`
during initial process loader initialization before CE's hook DLL is mapped. While `--launch` avoids this by
spawning the target suspended, regular hooking (CE background supervisor running) needed a generic,
robust solution:

1. **Multi-layer Supervisor Watchdog (`injection_ota_watchdog.cpp`):**
   - In `injection_manager.cpp` (`HandlePolledProcessStart`) and `injection_wmi_events.cpp` (`Indicate`), newly
     started processes are checked via `ce::ngx_ota::IsNgxUpdaterImage`. If `ngx_ota=off`, the spawned updater is
     terminated immediately via `OpenProcess(PROCESS_TERMINATE)` + `TerminateProcess(hProcess, 0)`.
   - In `injection_security.cpp` (`ScanExistingProcesses`), any existing updater running before CE started is
     terminated during the initial scan.
   - When a whitelisted target is discovered, `SweepRunningNgxUpdatersIfDisabled("TargetLaunchSweep")` runs
     immediately in `LaunchDelayedInjectionThread`, catching updaters launched during injection worker setup.
   - Injected `CreateProcessW` hook continues refusing subsequent spawns (`refusal #1..#15`).
2. **Target Profile Prewarming (`inject_config_publication.cpp`):**
   - Synchronous disk I/O and INI re-parsing previously took ~195 ms during `onInjectCallback`.
   - `SetPublicationBaseConfig` now prewarms `resolvedTargetConfigs` for all entries in `gameWhitelist` and
     `overlayWhitelist` during startup, dropping target config publication latency to ~0 ms.
3. **Reduced Discovery Polling Interval (`process_start_poll.h`):**
   - Dropped `kDefaultPollIntervalMs` from 250 ms to 50 ms (`kMinPollIntervalMs = 10 ms`). Because
     `NtQuerySystemInformation` takes <2 ms per sweep, this cuts discovery latency from 250 ms to ~25-50 ms.
