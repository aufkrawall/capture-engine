# llm-wiki Log Archive 2026-W38n

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
