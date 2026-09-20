# llm-wiki Log

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

New feature, `[Graphics] sharpen = off | cas | rcas` plus `sharpen_strength` and
`sharpen_color_space`. Full topic page: `post-processing-sharpen.md`.

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


### 2026-09-19 - pre-release review: three things worth changing, none of them broken

A review of the ~2 weeks since `v0.1.6261` against a near-term release. The tree was green
(`--incremental --skip-package --run-tests`, build 0.1.6713). Findings were about cost and honesty,
not correctness, and all three fixes are one-file.

- **The process-start poll cadence was increased 5x in a commit titled "poll cadence reduction"**
  (`6ce0adfb`: 250 -> 50 ms default, 50 -> 10 ms floor). That reverted a decision `ce26dafa` had made hours
  earlier with a written rationale, and it contradicts `process_start_poll.h`'s own header, which still says
  detection speed has seconds of margin before a real game's first swapchain. The motive was letting
  `TerminateNgxUpdaterIfDisabled` race the NGX updater's start - but `ngx_ota=off` works by **refusal**
  (`__NGX_DISABLE_UPDATER` in DllMain plus the slInit route), validated at 15/15 refused with no updater
  process created at all, so the sweep is a kill-on-sight backstop and not a race. Restored to 250/50.
- **The poller named every process on every sweep.** `SnapshotProcesses` ran `Utf8FromUnicodeString` plus a
  `std::string` allocation for all ~300 processes, while only pids absent from the previous sweep can be a
  start. It now takes the known-pid set and names only the misses; the baseline sweep passes `nullptr` and
  names nothing. Lesson: **a cheap syscall does not make the loop around it cheap** - the sweep's cost was
  never the `NtQuerySystemInformation` call the header sized it by.
- **`WerSetFlags(0x3)` was commented `NO_UI | QUEUE` but is `NOHEAP | QUEUE`; `NO_UI` is `0x20`.** This only
  became load-bearing in `52e3adf1`, which dropped `SEM_NOGPFAULTERRORBOX` from `SetErrorMode` (correctly -
  it makes the default unhandled filter terminate without invoking WER, losing the `__fastfail` dump) and
  justified that by saying the UI was suppressed via WerSetFlags instead. It was not. `crash_handler.cpp`
  runs in the injected hook DLL, so the gap was a fault dialog on the player's screen. Now `0x23` with named
  constants mirrored from `werapi.h`. Lesson: **when a removal is justified by a flag elsewhere, read the
  flag's value, not its comment.**
- **CHANGELOG was stale by ~10k lines of production code** - `v0.1.6652` (tagged, shipped) was the newest
  section while everything from 09-18/19 had landed since. Added an `Unreleased` section. `config.ini.template`
  was already current, so this was changelog-only. Note `dlss_fg_preset` (`0c2a299e`, 2026-08-08) predates
  `v0.1.6652` and is documented in no release section at all - a pre-existing gap, not this release's.

Reviewed and found sound, recorded so the next review does not re-derive it: the DRS override channel
(`017c040a`) wraps `nvapi_QueryInterface` rather than patching NvAPI prologues (required - DLSS FG validates
them during Reflex setup), mirrors `NVDRS_SETTING` with static asserts, and short-circuits on
`HasAnyOverride` so an unconfigured build changes no call path. The DX11 transient-RTV route and its scope
guard in `52e3adf1` are correct, and the batch quiescence fallback in `80bbf802` correctly leaves unclaimed
entries to the independent retry.

Open, not fixed (deliberately out of scope for a release): `wer_dump_adoption::TryAdoptLocalDump` matches
purely on `<image>.<pid>.dmp` with no freshness check, so a dump left by an earlier non-CE crash of the same
exe at the same pid would be moved out of the user's `%LOCALAPPDATA%\CrashDumps`; and
`published_graphics_config.cpp` says "a later call retries" when DllMain is its only call site (harmless -
the hook thread's config takes over - but the comment is wrong).


### 2026-09-19 - the FG health gate looked at the wrong mode and did nothing

Session `20260919_231939` still logged four `[DLSSG HEALTH] ON but NOT interpolating` warnings after the
previous commit was supposed to suppress them, all with `optionsMode=on(1)`.

- **Why the first fix was inert:** `dlss_fg_mode=dynamic` travels the driver settings and is applied inside
  sl.dlss_g, so the `slDLSSGOptions` CE observes never say `eDynamic` - Talos requests `eOn(1)` for the whole
  session. Gating on the application's options could therefore never fire. The gate now also consults CE's
  own configured mode through a new lock-free `ce::ngx_drs::GetConfiguredFrameGenerationMode()` (the
  per-GetState read must not take the overrides lock; the existing dynamic-support diagnostic was switched to
  it as well).
- **Keyed on configured intent, deliberately.** Whether the runtime accepted the request is precisely what CE
  cannot observe on this game (DLSSGState v3), and having *asked* for a runtime-chosen cadence is already
  enough to make "nothing was generated" unusable as a fault signal.
- Lesson, and the third time this shape of mistake appeared this session: **check which layer owns the value
  before gating on it.** An override CE delivers below the API it is reading back will not show up in that API.
- Everything else in that session is clean: all five DRS keys answered, `fps=138.2 stddev=314us
  displayJagUs=271 screenTimeShare=1000permille`, cadence 2x..4x, 14 metered multiplier lines, no dumps.

### 2026-09-19 - `vsync_mode` over DRS validated; the FG health monitor stops accusing dynamic mode

Session `20260919_230915`. All five DLSS driver-settings keys answered including VSYNCMODE
(`0x47814940` force-on to `sl.common`), and the previous session's two reporting bugs are gone:
`stateVersion=3` so `dynMFG` reads -1 rather than a false 0, and the metered multiplier log is 22 lines of
5299 (was 2743 of 10631) while still reporting `change 7168, observed 2x..4x`.

- **No VRR/pacing regression from forcing driver VSync**, which was the risk worth watching:
  `fps=138.5 stddev=415us displayJagUs=316 screenTimeShare=1000permille`, and `output_fps` varies 123-143
  rather than clamping - VRR behaving, not hard vsync.
- **`[DLSSG HEALTH] ON but NOT interpolating` was a false alarm** (5 occurrences) and is now suppressed for
  `eAuto`/`eDynamic`. The monitor's premise - the game requested frame generation, so frames must be
  appearing - only holds for a FIXED request. Under a runtime-chosen cadence, 1x is a legitimate choice when
  the rendered rate already meets the target, so a run of `numFramesActuallyPresented == 1` is not evidence
  of anything. The GTA case it was written for is a fixed request and keeps full monitoring.
- Deliberately *not* fixed by finding a better evidence source: `GetFGMultiplier()` is fed from the NGX/
  Streamline request and `GetBaseFPS()` derives from `outputFps / multiplier` for DLSS FG, so neither is
  independent of the thing being judged. The premise was the bug, not the signal.

### 2026-09-19 - `vsync_mode` now reaches DLSS-G, because only the driver key does

Follow-up to the dynamic MFG run. With `vsync=fifo` live the indicator still read `144 Hz F`; forcing VSync in a
driver profile instead reads `144 Hz VF`.

- **Why.** `sl.dlss_g`'s `vsyncState.cpp::shouldEnableVSync` resolves `DLSSG.VSyncOn` as: internal "unsupported"
  flag wins, then the **driver** VSYNCMODE key (`0x08416747` FORCEOFF / `0x47814940` FORCEON), and only if neither
  force value is present does the **application's** SyncInterval decide. CE's `vsync_mode` rewrite lands in
  `DXGIShared::ProcessPresentVSyncOverride`, on the real dxgi Present - below Streamline's swapchain proxy, per
  the hook-below rule - so the runtime never saw it as an application request either.
- **Fix.** CE answers `0x00A879CF` alongside the DLSS keys: `off` -> FORCEOFF, `fifo`/`adaptive` -> FORCEON,
  `mailbox`/`default` -> not claimed. Scoped by the existing caller filter, so only DLSS-G consumers see it.
- **The runtime's own consistency check is only a log.** `bCplVsyncOn` from NvAPI is compared against the DRS
  value and `RSYNC: Inconsistency detected!` is emitted when they disagree - which they now will. Verified by
  disassembly that the mismatch branch falls through to the same place as the consistent one, so answering the
  key alone is sufficient and no second NvAPI interception is needed.
- **Not cosmetic:** `checkGsyncAndVsync` and the Silk/RSYNC path consult the same value. Watch for a VRR
  regression, given [[fifo-vsync-present-metering-capability]] history. Hardware run pending.
- **Indicator decode corrected** (all named NGX parameters, so this is exact): the `%.0fHz` number is
  `DLSSG.TargetFrameRate`, *not* the panel refresh - `DLSSG.RefreshRate` is separate and is not printed. `Dyn DRV`
  is `DLSSG.Dynamic.ReflexDriven` non-zero (Reflex-**dr**i**v**en), not "driver-requested". `F`/`C` is
  `DLSSG.FlipMetering == 2` (hardware flip metering) or not, and `V` is `DLSSG.VSyncOn`. My earlier reading of the
  last two was wrong.
- The topic outgrew `graphics-overrides-and-frame-pacing.md` (785 lines); split to
  `frame-generation/dlss-driver-settings.md`.

### 2026-09-19 - Dynamic MFG confirmed working on hardware; two CE reporting bugs it exposed

First hardware run of the new `dlss_fg_mode=dynamic` keys (Talos Principle 2, session `20260919_223111`,
sl 2.14.1 + nvngx_dlssg 310.9.1, driver 616.92, 144 Hz panel). **The feature works.**

- **Delivery confirmed end to end.** `NGX DRS: answered NvAPI_DRS_GetSetting` fired for 0x10308298 (mode -> 4),
  0x10CF4125 (target -> 0x01000000 auto) and 0x10562D0F (dynamic max -> 5), from both `nvngx_dlssg.dll` and
  `sl.common.dll`, in exactly the order `readDRSKeys` reads them. The broadened caller filter was necessary and
  sufficient: the sl.common answer is the one the multi-frame keys actually travel through.
- **The cadence really varies.** `DXGIShared::DetourPresent` published multiplier 2/3/4 with base_fps
  46.52/34.89/27.85/20.89 against a constant output_fps of 139.57 - a target-rate hold on a 144 Hz panel, which is
  `dlss_fg_target_fps=max_refresh`. The Streamline options keep saying `optionsMode=on generated=3`, because the
  runtime varies the cadence *below* the API the game drives.
- **Bug 1 (mine, fixed): `bIsDynamicMFGSupported` was read out of a struct that does not have it.** `slDLSSGState`
  is allocated by the *game*; the field arrived in structVersion 4 and Talos publishes 3. CE read the neighbouring
  byte as 0 and logged "dynamic MFG NOT supported, so the request is ignored" while it was plainly working.
  `ResolveDLSSGStateOptionalBool` now returns -1 (unknown) below the field's minimum version, the same gate covers
  `bIsVsyncSupportAvailable`, and every DLSS-G state line carries `stateVer=`. Never read a versioned field of a
  caller-owned struct without checking the caller's version.
- **Bug 2 (pre-existing, fixed): the multiplier transition log was 26% of the session.** 2743 of 10631 lines were
  `FG: DLSS FG multiplier X -> Y`, because dynamic MFG makes that a per-frame event. Metered burst-then-heartbeat
  (8 then every 512) and the heartbeat now carries the observed range.
- **Indicator decode** (from nvngx_dlssg 310.9.1's builder, so we stop guessing): mode token `Auto` / `Dyn` /
  `Dyn DRV`, factor `- 4x` or `- 3x/6x`, then `- %.0fHz %s` where the token is `V` (VSync on) plus `F` (independent
  flip) or `C` (composited). `144 Hz F` is a present-mode report, not an MFG one, and eAuto/eDynamic disable VSync
  anyway, so it cannot become `VC` while dynamic MFG runs.
- **Dead end worth not repeating:** `checkDynamicMFGSupport` gates on `flipMetering.cpp` having negotiated
  SetFlipConfig **V2** - `NvAPI_QueryInterface(0x6194B19D)` reporting feature `0x343DCF` with bit 0, which sets the
  stored version to 0x20018. Chasing that as the failure cost an hour; the driver reports it unconditionally in
  616.92, and the real problem was CE's own out-of-bounds read.

### 2026-09-19 - NVIDIA Profile Inspector dynamic MFG, implemented over the same DRS channel

Added `dlss_fg_mode`, `dlss_fg_fixed_count`, `dlss_fg_dynamic_max` and `dlss_fg_target_fps`: the four DLSS frame
generation keys NVIDIA Profile Inspector writes into a driver profile, answered process-locally instead.

- **Where the ids and encodings came from.** NVIDIA Profile Inspector's `Native/NVAPI/NvApiDriverSettings.h`
  (`NGX_DLSSG_MODE_ID 0x10308298`, `NGX_DLSSG_MULTI_FRAME_COUNT_ID 0x104D6667`,
  `NGX_DLSSG_DYNAMIC_MULTI_FRAME_COUNT_MAX_ID 0x10562D0F`, `NGX_DLSSG_DYNAMIC_TARGET_FRAME_RATE_ID 0x10CF4125`) plus
  its `CustomSettingNames.xml` value lists, cross-checked by disassembling this machine's Streamline 2.14
  `sl.dlss_g.dll` and `sl.common.dll` from the NGX model store.
- **The reader is NOT nvngx_dlssg.** `sl.dlss_g!readDRSKeys` reads all four through `readSingleDRSKey`, which uses the
  DRS context `sl.common` owns; `sl.common` is the module that resolves `NvAPI_DRS_GetSetting` (0x73BF8338) through
  `nvapi_QueryInterface` and performs the call with `NVDRS_SETTING_VER1`, reading `currentValue.u32Value`, app
  profile first and base profile as fallback. The existing `dlss_fg_preset` caller filter only knew `nvngx_dlssg`, so
  it had to be widened - and widened by *export*, not by name, because an OTA-downloaded Streamline plugin is mapped
  under a content-addressed file name (`160_E658703.dll`); `slGetPluginFunction` is what identifies it.
- **Two DRS ids that are read but deliberately not claimed:** `0x00A879CF` is `VSYNCMODE` (sl.dlss_g uses it for its
  RSYNC decisions) and `0x10C7D835` gates the whole DLSS-G DRS override. CE answers only its five keys and forwards
  everything else untouched, including while another key is armed, because both readers pull several keys per loop.
- **Mutual exclusion is a real requirement, not tidiness.** `dlss_fg_factor` travels the NGX/Streamline parameter
  channel and pins `numFramesToGenerate` on every evaluation. Under `dlss_fg_mode=dynamic` that directly contradicts
  the driver-level request, so `ResolveEffectiveDLSSFGFactor` stands the factor down and every consumer (NGX params,
  Streamline options, published multiplier, Remix scheduler) goes through it.
- `hook/common/ngx_fg_preset_override.{h,cpp}` became `ngx_drs_override{,_policy}.{h,cpp}`; the policy half is pure
  and carries the NVDRS_SETTING ABI mirror. Shared ABI 59 -> 60: four fields into the existing tail padding, so
  `sizeof(SharedGraphicsConfig)` is unchanged at 1752 - which is precisely why the version had to move.
- **Unvalidated on hardware.** Built and unit-tested only. This machine's DLSS-G is 310.2.1, which predates both the
  preset key and the multi-frame keys, so nothing here has been observed answering a real read. What to look for:
  `NGX DRS: wrapping NvAPI_DRS_GetSetting for ...` (the resolution reached CE at all), then
  `NGX DRS: answered NvAPI_DRS_GetSetting(...)`, and the runtime's own
  `Read DRS key %d = 0x%x from app profile` / `Dynamic MFG is supported` in the NGX log.
