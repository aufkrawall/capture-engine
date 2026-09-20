# llm-wiki Log Archive 2026-W38o

### 2026-09-20 - a sharpen-only config save respawned inject and then deadlocked the RHI thread

Session `20260920_192913`, build 0.1.6741. Saving `config.ini` with only sharpen keys changed made CE
tear its whole runtime down and rebuild it, flashed the desktop "NOT RECORDING" warning over a game
configured for the inject overlay, and ended with Talos frozen and self-terminating. Two independent
bugs, one visible story.

**1. The reload ack overran its window, so the controller killed a healthy child.**

`SetPublicationBaseConfig` warmed the resolved-config cache for *every* whitelist entry, and each warm
is a full `ReadLiteralIniValue` pass over `config.ini`. The log shows 28 of them at ~178 ms each -
`19:32:25.074` to `19:32:29.942`, ~4.9 s. That function is also what the inject child's `ReloadConfig`
handler calls, and `ProcessIPCClient::SendCommand` gives a child 1000 ms to reply. So:

- `19:32:20.901 [IPC] inject command channel broke; a fresh child is required` - exactly 1000 ms after
  the send. The pipe framing is genuinely desynchronized after a timeout, so the client closes it and
  `recoverProcess` respawns. Not a wrong reaction; a wrong premise.
- `19:32:23.918 inject has not exited after its IPC channel broke` proves the child was **alive and
  busy**, not dead.
- In the game: `19:32:24.888 [InjectLifecycle] Runtime dormant (host requested shutdown)` ->
  `19:32:32.133 Reactivated resident hook for host PID 21288`. 7.2 s with graphics overrides off, FFX
  dormant, UE5 overrides shut down, the FPS limiter stopped and the inject-overlay flags cleared - which
  is precisely why the controller's pseudo overlay un-suppressed and drew `NOT RECORDING` at
  `19:32:25.120`. The desktop warning was not a profile-resolution bug; it was CE correctly observing
  that its own inject overlay had gone away.
- Reactivation then re-ran the UE5 console-registry sweep: 645 MB over 4 passes, `19:32:30` to
  `19:34:09`, ~100 s, to re-resolve 46 CVars that had never changed.

Fix: the prewarm is kept - it exists so the injector's pre-`LoadLibrary` publish (which carries
`ngx_ota` mode) is a cache hit - but it now runs on its own worker thread, queued by generation, and
never holds the publication mutex across a resolve. `ResolveActiveConfigLocked` still resolves on demand,
so the sweep is an optimization and never a correctness input. The handler is now one `LoadConfig` plus
one active-target resolve, and logs its own duration with a warning past 500 ms.

**2. `SharpenDX12PresentedFrame` re-entered a non-recursive mutex and parked the present thread.**

Introduced by 180a4f93. The function takes `g_SharpenMutex` with `try_to_lock` for the whole call; its
`sharpen=off` branch then called `ReleaseDX12SharpenResources()`, which takes the same plain
`std::mutex` again. libc++ maps `std::mutex` onto an SRWLOCK, so the second acquire never returns. The
dump is unambiguous:

```
RHIThread  ntdll!RtlAcquireSRWLockExclusive
           capture_hook_x64!ReleaseDX12SharpenResources
           capture_hook_x64!SharpenDX12PresentedFrame+0x1c9
           capture_hook_x64!PostSLOverlayRender ... DXGIShared::DetourPresent
```

and the fatal message on the game thread was
`RenderingThread.cpp:1415 GameThread timed out waiting for RenderThread after 120.00 secs`, followed by
Talos calling `TerminateProcess` on itself with code 3. `[HookThreadStages] ipc(max=7245244u)` is the
same 7.2 s dormancy from bug 1 in the hook's own accounting.

The two bugs meet here: the hook reads its sharpen mode from the published shared config, and while the
host was gone that config fell back to defaults - `sharpen=off` - with `g_SharpenEverRendered` still
true from before. So bug 1 *triggered* bug 2. Setting `sharpen=off` by hand reaches it just as
deterministically.

Fix: an unlocked `ReleaseSharpenResourcesLocked` body for callers that already hold the mutex; the
public entry point is now only a lock plus a delegation. This is the shape `layer_sharpen.cpp` already
had (`DestroySharpenState` directly), which is why Vulkan never hit it. DX11 has no mutex on this path
at all and was likewise unaffected.

**Diagnosis notes worth keeping.** The UE5 `ensure` at `19:34:30` reached CE only through
`HookedRaiseException`; every other frame is the game's. Reading the message out of the stack
(`dpu @rsp` -> "Fatal error: [File:...RenderingThread.cpp] [Line: 1415]") is what turned "Talos crashed"
into "the render thread has been blocked for two minutes", and `~*k` then named the blocker in one step.
`[HookThreadStages]` was the pre-existing instrumentation that made the 7-8 s IPC stalls visible in
plain text before the dump was ever opened.

Regression coverage: `tests/test_config_reload_reinit_policy.cpp` (8 cases) locks both the DX12/Vulkan
sharpen lock discipline and the queued-prewarm contract, including that the worker never holds the mutex
across a resolve, that a stale generation is dropped, and that inject joins the worker before unmapping.
`test_ngx_ota_policy.cpp`'s prewarm assertion was updated to the new mechanism rather than dropped.
Full suite 3492 green on 0.1.6743. **No hardware run yet.**


### 2026-09-20 - a cached COM interface outlived the apartment Windows tore down under it

`ScreenGrabPrivacyTest.TaskViewAndDesktopClassesAreRejectedEvenWithFullscreenGeometry` faulted
with 0xC0000005 on every run and took the whole unit-test process down mid-suite. Root cause in
`common/screen_grab_privacy.cpp::IsWindowOnCurrentVirtualDesktop`, fixed by creating the
`IVirtualDesktopManager` per call instead of caching it in a `thread_local`.

- **The hypothesis that an *earlier test* called `CoUninitialize` was wrong.** The test crashes
  alone, as the only test in the process, so nothing else had run. Verifying that first saved
  chasing test-ordering ghosts.
- **What the debugger showed.** The fault is in CE's own frame, not inside COM:
  `mov rax,[rcx]` reads the object's vtable pointer successfully, then `mov rax,[rax+18h]` faults.
  `!address` on that vtable reports `MEM_FREE` / `PAGE_NOACCESS` - the vtable is in **unmapped**
  memory. That is the signature of an in-process COM server that was unloaded while CE still held
  an interface pointer into it.
- **Who unloaded it.** A breakpoint on `CoUninitialize` caught
  `USER32!CtfHookProcWorker -> combase!CoUninitialize`: USER32's text-services hook balances its
  own `CoInitialize`/`CoUninitialize` while ordinary window messages are processed. The balancing
  call reached zero, so `combase!ProcessUninitialize -> CClassCache::CleanUpDllsForProcess`
  called `FreeLibrary` on every in-process server. The test's `DestroyWindow`/`UnregisterClass`
  between the two queries is what pumped the messages.
- **The invariant:** a COM interface pointer is valid only while the apartment that created it
  lives, and **CE does not own the apartments of the threads it runs on**. Windows itself
  initializes and uninitializes COM on a UI thread. Caching an interface across calls is only
  sound if CE holds its own apartment reference, which it must not do on a thread it borrows.
- **Not a test artifact.** The media process queries this once per captured frame in the
  screen-grab privacy gate, so the same teardown would fault a live recording.
- Per-call creation keeps the server loaded for exactly as long as the reference is held. The
  cost is a warm class-cache lookup, below the DWM round trip `IsWindowCloaked` already makes on
  the same path. Removing the "already tried" latch also means a thread that initializes COM
  late recovers on its own, which is what cac63467 had been reaching for.
- Regression test `VirtualDesktopQuerySurvivesApartmentTeardownBetweenCalls` does explicitly what
  Windows was doing incidentally - query, `CoUninitialize`, re-init, query - and faults with
  0xC0000005 against the old code. Full suite now 3473 green with no gtest filter.


### 2026-09-20 - post-processing sharpen: FidelityFX CAS and RCAS on D3D11, D3D12 and Vulkan

New feature, `[Graphics] sharpen = off | cas | rcas` plus `sharpen_strength`,
`sharpen_intensity` and `sharpen_color_space`. Full topic page:
`post-processing-sharpen.md`.

- **Two independent controls, matching ReShade's CAS port.** `sharpen_strength`
  is AMD's own contrast-adaptation parameter and is *not* off at 0;
  `sharpen_intensity` mixes the filtered result back over the original pixels
  and *is* off at 0. The mix happens in the frame's stored space after the
  working-space round trip, not inside AMD's kernel, which is what keeps the two
  orthogonal. Zero intensity refuses the pass outright rather than writing the
  frame back unchanged. That second float moved SHARED_MEMORY_VERSION to 62.

- **The ordering rule is the design.** The filter runs on the frame the Present will put on
  screen, *before* inject capture copies it and *before* the overlay draws. That is what
  keeps CE's own overlay unsharpened, keeps the recording and the screen in agreement even
  with `capture_include_overlay=false`, and makes it work with the overlay disabled. Each
  backend resolves its own target for that reason rather than borrowing the overlay's.
- **Every displayed frame is filtered under FG, generated ones included.** Filtering a subset
  would show up as a sharpness pulse at the generation cadence. The cost therefore scales with
  the displayed rate; at 4x MFG that is four passes per rendered frame, and it is **unmeasured**.
  Default is `off` until `overlay_gpu_timing.cpp` has produced numbers.
- **Filtering pre-FG was rejected**, though it would cost one pass per rendered frame: with
  FSR FG, CE has no view of the application's Present through AMD's proxy, and with DLSS-G it
  would mean writing into a buffer Streamline owns as its interpolation input.
- **`_SRGB` views put linear light in front of the kernel**, exactly like scRGB FP16 does, and
  sharpening linear light rings around highlights. The `auto` working space keys on that, not
  on the presentation encoding alone.
- **Neither effect is off at strength 0**; 0 is each one's mildest setting and `sharpen=off` is
  the only switch. The parser keeps an explicit 0 rather than treating it as absent.
- **Vulkan needed `VK_IMAGE_USAGE_TRANSFER_SRC_BIT` negotiated at swapchain creation**
  (`vulkan_swapchain_usage_policy.h`, fail-closed against `supportedUsageFlags`). Inject capture
  had been copying from swapchain images without that bit ever being requested.
- `spirv-opt -O --strip-debug` takes the CAS fragment module from 61 KB to about 6 KB - a tenth
  of the blob and a tenth of the driver's pipeline-creation work, not a size cosmetic.
- ABI: the resolved sharpen settings grew `SharedGraphicsConfig` past its tail padding, so
  `SHARED_MEMORY_VERSION` moved to 61. `SharedGraphicsConfig` and its layout assertions moved
  into `common/shared_defs_detail/graphics_config.h` to keep the ABI header under the size
  ceiling; it is included from inside that file's pack region and is deliberately not standalone.
- Headers vendored from the MIT FidelityFX SDK 1.1.4 archive the build already downloads. The
  newer 2.x SDK drop must not be used as the source: its `docs/license.md` is
  binary-redistribution-only and contradicts the per-file MIT banner in the same headers.
- **Unrelated pre-existing failure found and fixed the same day** - see the entry above.

### 2026-09-20 - decoupled Vulkan layer registration via runtime staging to eliminate external file locks

Non-whitelisted third-party processes (Explorer, Chrome, Discord, `DataExchangeHost.exe`, etc.) frequently
loaded `VK_LAYER_CE_overlay.dll` via the Vulkan implicit layer registry entries when enumerating Vulkan
instances/adapters. This held shared read-execute locks on the binaries in `installed/captureengine/`,
preventing developers and build scripts from replacing, rebuilding, or removing them even when CaptureEngine
itself was closed.

- **Root Cause:** The Vulkan loader enumerates implicit layers from `HKCU` / `HKLM` `Software\Khronos\Vulkan\ImplicitLayers`
  for every process initializing Vulkan. When those registry entries pointed directly to manifests in
  `installed/captureengine/`, arbitrary third-party apps kept the original DLLs open in memory.
- **Generic Solution (Runtime Staging / Shadow Copying):**
  - Pristine build binaries stay in `installed/captureengine/` and are never directly registered in the Vulkan
    implicit layer registry.
  - `BuildRegistrationPlan` resolves a staging folder under `%LOCALAPPDATA%\CaptureEngine\vulkan_layers\b<build_number>\`
    (or `%PROGRAMDATA%` when running elevated).
  - Manifests written to the staging directory reference the staged DLL paths.
  - `ApplyRegistrationPlan` copies/stages the manifest and DLL artifacts to the staging folder before writing
    the registry keys. If an existing staged file is unchanged in size and timestamp, staging is skipped. If a staged
    file is locked by another process during an in-place reinstall of the same build, it logs a warning and reuses the
    existing image.
  - External non-whitelisted processes map only the AppData shadow copy, leaving `installed/captureengine/` completely
    unlocked and freely replaceable.
  - `CleanupStaleStagingDirectories` iterates over the parent staging directory on startup/registration and prunes
    older build folders (`b*`), catching and ignoring `remove_all` errors if an external process still holds a lock on
    an older build image until that process exits.
  - Late injection is 100% preserved because the layer remains resident and registered in the Vulkan loader chain.
- **Source anchors:** `common/vulkan_layer_registration.{h,cpp}`, `tests/test_vulkan_layer_registration.cpp`,
  `tools/build/build_bootstrap.py`, and `llm-wiki/{dx12-injection-bootstrap,log/recent}.md`.


### 2026-09-20 - the FPS limiter tests measured the host scheduler, not the limiter

Seven `FpsLimiterTest` cases failed intermittently with a DIFFERENT set each run, on clean HEAD as well
(1 of 6 runs on an idle machine). None of them had a bug under them; they asserted on wall-clock.

- **Mode-resolution tests** (`AutoMode_FallsBackToBasic`, `AutoMode_UsesFGFallbackWhenFGActive`,
  `FGFallback_CaptureSync_DoublesInterval`, `FGFallback_UsesExplicitDLSSMultiplier`) called `Apply()` twice
  and timed the second. `RunLocalCadence` waits `localTargetTime_ - now`, so **every microsecond the host
  spends between arming the deadline and reaching it is subtracted from the measured wait**. Under load the
  residual collapses toward zero - and past it, at which point the deadline is re-based and nothing is
  waited for at all - so the LOWER bounds failed, not just the upper ones. Both bounds were load-sensitive
  for the same reason. They now assert `FpsLimiter::GetResolvedCadence()`, which publishes the target rate,
  the cadence scale and the group interval that `RunLocalCadence` resolved. Exact equality, no margins,
  0.1 s instead of seconds of real sleeping.
- **`GpuWorkRunningPastTheDeadlineGrowsTheReservation`** failed as `grown.budgetUs == engaged.budgetUs`
  (4166 vs 4166 - the whole 240 fps interval). `NoteFrameWorkForFrontLoadedRelease` derives CPU frame work
  from the span since the last release, which in a test is just however the host scheduled the loop, so a
  loaded machine reports the game working for a full interval and the budget **saturates at the back edge
  during the phase that is supposed to leave room to grow**. New `SetObservedFrameWorkOverrideUs` states it
  instead, the way `ObservePresentToDisplay` already states the GPU half; 0 (always, outside tests) keeps
  the measured path. The test now also ASSERTs the first phase left headroom, so a future saturation
  cannot make the growth assertion vacuous again.
- **`SmartWait_Accuracy` / `SubTickWaitsLandWithoutTheKernelTimer`** measure the wait primitive, so they
  cannot be made deterministic by moving the assertion - but their real claim is **structural**: a wait
  shorter than the scheduler tick must not arm the kernel timer (it cannot land inside a tick, so it sleeps
  past the deadline). New `GetSmartWaitCount()` / `GetKernelTimerWaitCount()` make the path observable, and
  the tests assert the path plus never-early per sample. The overshoot is now a `RecordProperty` diagnostic.
  Added `SupraTickWaitsStillUseTheKernelTimer` so the sub-tick assertion cannot be satisfied by SmartWait
  abandoning the timer altogether.

Result: 8 of 8 passes under 16-way CPU saturation, where the previous form failed on an idle host.
Coverage went UP, not down - the mode tests now pin exact rates rather than a duration band, never-early is
asserted per sample rather than on the minimum of seven, and the wait path is pinned in both directions.

Lesson: **if an assertion's value is produced by the scheduler, the test measures the scheduler.** Ask what
the code under test actually decides, and expose that instead. AGENTS.md already says not to put timing
assumptions in tests; a duration bound on a real sleep is one, no matter how wide the margin - widening it
only makes the test slower to fail, and the previous round of widening (medians over 7 and 15 samples) is
visible in the history of exactly these cases.
