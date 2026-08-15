# llm-wiki Log

### 2026-08-16 - Console-registry sweep is resumable; "never registered" is no longer assumed

- Reviewing Industria 2 session `20260815_214219` (UE 5.6.1, build 0.1.6109) surfaced two defects behind one log
  line: `console registry anchored on r.VT.MaxAnisotropy (31/34 anchor element(s) across 2 region(s)) after
  218 MB in 406ms`. 406 ms against a 400 ms budget is a **deadline exit**, not a finished walk (contrast Talos
  `20260815_210850`: 30/31 in 93 ms over 175 MB, which *did* reach the end of the heap).
- Defect 1: the partial result was frozen. `FindRegistryMap` only ran while `!g_map.valid`, so once the first
  element was confirmed the search never ran again - all 90 retry passes re-read the same two regions and the
  heap beyond the park point stayed unexamined for the whole session. Fixed: the sweep carries a cursor, parks on
  the chunk it did **not** read (the 64-byte chunk overlap then guarantees nothing falls through the pause), and
  resumes there each pass until the enumeration is exhausted; bounds are now per-pass (400 ms / 768 MB) plus
  cumulative (32 passes / 8 GB). The all-anchors-placed early exit was removed - placing every anchor does not
  prove every region of the map was seen.
- Defect 2: the closing summary reported the leftovers as "never registered by the engine", computed as
  `missing - refused`, i.e. "not found in the regions we happened to prove". With an incomplete sweep that is an
  inference, not a measurement. It now branches on `SweepProvesAbsence`: "absent from the fully swept registry"
  vs "not found in the N MB swept before the sweep stopped at X - unproven, not known-absent".
- For the two names Industria 2 reported (`r.MegaLights.DownsampleMode`, `r.Tonemapper.GrainQuantization`) the
  verdict happens to hold anyway on independent evidence: neither has a UTF-16 literal in any of the 119 loaded
  modules, in *either* title or engine version, and a statically registered CVar always carries its name literal.
- Sweep progress lives in `hook/common/ue5_console_registry.h` as pure predicates so the resume arithmetic is
  unit-testable (`UE5RegistrySweepTest`, 5 tests): a paused-and-resumed sweep visits exactly the chunk offsets a
  single pass would, the cursor parks on the unread chunk, region skipping stays in bounds and never wraps, and
  only a finished sweep may support an absence claim.
- Resolve passes are time-boxed (100 ms) and rotate their starting region, since the region set can now grow.
- Open, not addressed here: all four `ShowFlag.*` installs read `prevValue=0` in both titles, so writing 0 may be
  a no-op - see the stale-risk note in `graphics-overrides-and-frame-pacing.md`. Reported symptom: Industria 2's
  main menu still shows chromatic aberration / lens distortion despite `r.SceneColorFringeQuality=0` and
  `ShowFlag.SceneColorFringe=0` both installed and verified.

### 2026-08-15 - VALIDATED: no crash, ShowFlag overrides land, 35/40; one self-inflicted log-spam regression fixed

- Session `20260815_210850` (build 0.1.6108) is the validation run for both fixes. **No crash, no dump.**
- DX12 fix behaved exactly as designed: `Deferring eager temp-swapchain ...` then the new
  `postponing the temp swapchain instead of recursing through its startup hook chain`, so nothing was created
  during the dangerous window. 6.8 s later the game's own swapchain arrived, Present hooks installed from it,
  and `Postponed Present hook install resolved by the game's own swapchain` disarmed the retry. Multi-overlay
  behaviour intact: `CE intercepts BELOW the foreign Present chain via a deep body hook` plus the Present1
  deep body hook, with Steam still owning the Present entry.
- Console registry proved out on a real UE 5.4.4 process: anchored on `r.SceneColorFringeQuality`,
  `valueOffset=16`, **30/31 anchor elements across 3 regions** - confirming the earlier single-region walk was
  the reason it resolved nothing. `ShowFlag.Vignette`, `ShowFlag.Grain`, `ShowFlag.MotionBlur` and
  `ShowFlag.SceneColorFringe` now install with `writeThrough=1`, so vignette is no longer the standing gap.
  Coverage 31/40 -> **35/40**, `verified 35/35` with `re-asserted=0 retired=0`, RR still rendering.
- Regression introduced by that same change and fixed in cdb9f5c0: the per-CVar registry failure for
  `r.Lumen.ScreenProbeGather.Temporal.MaxFramesAccumulated` was logged on every retry - 91 copies in one
  session, exactly the log-spam class 3ec11124 exists to prevent. A located-but-undrivable CVar is now
  recorded and skipped, retrying stops once every remaining name has been refused, and the closing summary
  distinguishes "located but undrivable" from "never registered" instead of claiming the latter for a name
  the registry had just found.
- Remaining 5 of 40: four version-conditional names absent from UE 5.4 (`DownsampleCheckerboard`,
  `MegaLights.DownsampleMode`, `MegaLights.NumSamplesPerPixel`, `Tonemapper.GrainQuantization`) and
  `MaxFramesAccumulated`, whose object the registry finds but whose value storage matches neither supported
  layout. Refusing it stays correct - its `ref+0x50` points into `.pdata`.

### 2026-08-15 - FIXED locally: the startup stack overflow was CE's temp swapchain, not Steam's bug

- Correction to the entry below: the recursion is Steam's, but **CE starts it**. No CE frames appear on the
  stack, which is why the first read was wrong - the stack shows the loop, not who entered it.
- `DX12Hook::Init` refuses the eager temp swapchain when a third-party overlay is hooked before the game's
  first real D3D12 device, because entering that overlay's handler during hook install is a crash CE's own
  source documents (`dx12_hook_hook_install.cpp:25`: Steam dispatches through callback slots that stay NULL
  until it has rendered on a real game swapchain). **That deferral protected nothing** - `Init` called
  `FindAndWrapPreExistingSwapchains` on the next line and it created the temp swapchain anyway, justified by
  "the overlay's startup hook chain should be settled". Session `20260815_203836` logged both decisions in
  the same millisecond, and the crashing thread's last CE line is `CreateSwapChainForHwnd INLINE: Temp
  swapchain — passthrough`, 440 ms before it died.
- Fix (cc7ed7cc): the deferred install re-evaluates the same startup window through
  `ShouldPostponeDeferredTempSwapchainPresentHookInstall` and waits for the game's own D3D12 device - the
  evidence the eager decision already trusts. `DX12Hook::ServicePendingPresentHooks` retries from the hook
  thread's service pass, because nothing else calls back once that window closes. No third-party overlay, or
  no deferral, means byte-for-byte the historical path. A unit test pins the invariant that the postponement
  can never outlast the deferral condition, so a pre-existing swapchain cannot be stranded without hooks.
- The other crash - `0xC0000005` at `capture_hook_x64.dll+0x850A0`, session `20260815_174213`, build 6087 -
  needs no fix. Resolved with that session's archived binary + PDB: the faulting instruction is
  `cmp dword ptr [rbp+r15*4], 0x680053`, i.e. UTF-16 `"Sh"` - a raw heap scan for `ShowFlag…` reading ~18 MB
  past its region. That was the uncommitted ShowFlag discovery experiment (findings in 8a558623).
  `git log -S "MEM_PRIVATE" -- hook/` shows the only committed heap scanner is the console registry from
  28faeae2, which reads exclusively via `ReadProcessMemory` and fails cleanly instead of faulting.
- Validation still owed: several launches with the Steam overlay enabled, confirming the inject overlay still
  appears and `Postponed Present hook install resolved by the game's own swapchain` is logged.

### 2026-08-15 - Intermittent startup crash characterised: Steam overlay self-recursion, not CE code

- Session `20260815_203836` crashed on startup with `0xC00000FD` (stack overflow). The stack is 750+
  identical recursive frames of `gameoverlayrenderer64!OverlayHookD3D3+0x14bc4` calling itself, then
  `VulkanSteamOverlayProcessCapturedFrame+0x23346 -> +0x2f9ce -> KERNELBASE!WriteFile` at the innermost end.
  **No CE frames anywhere on the stack**; the WriteFile is Steam writing its own log and is only what finally
  exhausts the stack (CE calls WriteFile for logging but does not hook it).
- Not a regression from the UE5 override rework: the identical signature appears in `20260815_171603`
  (build 0.1.6081) and `20260815_174131` (0.1.6087), hours earlier and in Industria 2 rather than Talos, and in
  the crashing session the UE5 override code never executed at all (no `UE5 overrides enabled` line - the hook
  thread was still in its initial install). Rate: 4 crashed sessions out of 21 on 2026-08-15.
- Lead worth chasing: every crash lands ~0.5-1 s after `DX12Hook: Deferring eager temp-swapchain Present hook
  install because third-party overlay gameoverlayrenderer64.dll is already loaded`, and before
  `InstallPresentInlineHooks` is reached; good runs pass that point ~0.6 s after the deferral. That window -
  CE's deferred Present hooking against Steam's overlay init - is where to look. Unproven; no CE frames.
  Decisive test: several launches with the Steam overlay disabled.
- Separate and also pre-existing: `20260815_174213` (build 0.1.6087) crashed `0xC0000005` inside
  `capture_hook_x64.dll+0x850A0`. A genuine CE fault, distinct from the recursion, uninvestigated.

### 2026-08-15 - Talos 20260815_202743 validates the UE5 override rework; registry region scope was too narrow

- First run with the reworked overrides. Confirmed working: `writeThrough=1` on all 20 data-pointer installs,
  `verified 31/31 installed CVar(s) ... (re-asserted=0, retired=0)` and then silence for the rest of the session
  (the summary only logs on change), `prevValue`/`neighborDword` labelling, and **no**
  `module notification queue overflowed` at all - the 32 -> 128 slot bump removed both full rescans, though the
  scan itself now covers 122 modules in 1141 ms. RR still reaches `Feature 13 evaluation succeeded`.
- The console registry located UE's map in 47 ms after 21 MB, anchoring on `r.SceneColorFringeQuality` with
  `valueOffset=16` - the layout derivation works on a real UE 5.4.4 process.
- But it resolved nothing, including `r.Lumen.ScreenProbeGather.Temporal.MaxFramesAccumulated`, which is definitely
  a registered console variable. That is the tell: the search stopped at the first confirmed element and the second
  pass only re-read that one 64 KB region. Fixed in 23fd7b5f - walk until every anchor is placed, record every
  region holding confirmed elements, resolve across all of them, and report confirmed/total in the log.
- Consequence for the `ShowFlag.*` question: this session does **not** show they are unregistered, only that the
  search was incomplete. A run with a full confirmed/total ratio and still no ShowFlag hits would be the evidence.
- Still unproven: whether a data-pointer-mode override changes rendering. `verified 31/31` proves CE's value is
  what the storage holds, not that the engine reads that storage. Needs an A/B against the game's own console on
  something visible, e.g. `r.Lumen.Reflections.DownsampleFactor` 2 -> 1.

### 2026-08-15 - UE5 overrides: restore-null fix, write-through, read-back verification, console registry

- Reviewing Talos session `20260815_191332` (31/40 installed, exactly as documented) turned up a **crash-on-restore
  bug**: the data-pointer redirect stored its undo pointer as `nullptr`, so `RestoreOverride` compare-exchanged null
  into the live console object on config disable, single-CVar disable, or hook shutdown. 20 of Talos's 31 overrides
  used that path. The undo contract is now a pure, unit-tested plan (`hook/common/ue5_redirect_plan.h`): no redirect
  is installed unless the pointer it replaces has been recorded.
- Data-pointer installs now also mirror CE's value into the storage the original pointer addressed (`writeThrough=1`)
  and restore it on undo. Engine code generated for `FAutoConsoleVariableRef` CVars reads that global directly rather
  than through the console object, so the pointer redirect alone was potentially invisible to it. Note the one
  end-to-end proof in the session - `r.NGX.DLSS.DenoiserMode` driving `Feature 13 ... Ray Reconstruction is
  rendering` - came through the **Ref** redirect; no data-pointer install has equivalent proof yet.
- `VerifyOverrides` (once a second) re-reads every override through the storage the engine reads: a redirect the game
  took back retires the record and requests a rescan, a drifted value is re-asserted. Reporting is capped per
  override and only lifts after 60 clean passes.
- New **console-registry resolution**: anchors on CVars the module scan already installed, finds a map element that
  pairs a known name with a known object, derives the key-to-value distance from it, then resolves the missing names
  from the same allocation. This is the path to `ShowFlag.Vignette` and friends, whose names UE composes at runtime.
  All reads use `ReadProcessMemory`; the search is bounded (768 MB / 400 ms / 4 attempts) and cheap re-reads cover
  variables registered later during world init. Unvalidated on hardware yet - needs a Talos/Industria run.
- `main_ue5_scan.cpp` had reached 869 lines, past the 800-line ceiling and absent from
  `tools/file_size_baseline.json`, so lint would have failed. Split into `main_ue5_scan.cpp` (discovery),
  `main_ue5_install.cpp`, `main_ue5_memory.cpp`, and `main_ue5_registry.cpp`; all units are now 185-489 lines.
- Pending-module queue grew 32 -> 128 slots. Talos overflowed it 8 and 11 times per launch, and each overflow costs a
  full ~700 ms rescan of all 112 modules.
- Install logs no longer print the data-pointer mode's unvalidated neighbouring dword as `oldRender`: it is
  `prevValue=` plus `neighborDword=` now, because the old label read as a second shadow value and invited the
  conclusion that correct installs had garbage in them.

### 2026-08-15 - Rate-limit repeated hook diagnostics; ShowFlag discovery findings

- `hook_debug.log` was dominated by repeated loader/Streamline/FFX module notifications ("Fresh module load
  inspected", "IAT: ... already patched", "Redirecting ... to:", "Hooks installed successfully"), producing 26k+
  lines / 5 MB per session. The first occurrences plus periodic counters are now logged; a 20 s Industria 2 loop
  drops the hook log to ~2.9k lines / 475 KB (~10%) with identical UE5 override coverage (32/38). The UE5 scanner's
  raw-candidate memory dumps are additionally bounded to four and its unresolved-CVar detail budget halved.
- ShowFlag.* investigation: the composed names ("ShowFlag.Vignette", ...) DO exist in the game's writable memory as a
  contiguous UTF-16 string table (created at world/graphics init, present once DLSS features exist). They are not
  embedded in the CVar objects (the S-0x60 heuristic hits unrelated heap objects like thread pools), and no FString
  referencing them was found in the owning region. Reaching the CVar objects therefore needs a registration-capture
  hook (e.g., on IConsoleManager::RegisterConsoleVariable) rather than a memory scan; vignette remains the last
  functional gap for `disable_post_processing_effects` on UE 5.4/5.6.

### 2026-08-15 - UE5 scanner: data-pointer redirect mode for UE 5.4/5.6 Lumen CVars

- UE 5.4/5.6 `FConsoleVariable` keeps its value behind a data pointer at `ref+0x50` with a local fallback pair at
  `ref+0x58` (verified by disassembling `GetValueOnGameThread`). The UE5 scanner now has a data-pointer redirect mode
  (CAS `ref+0x50` -> CE shadow + mirror into `+0x58`), a lone-validated-candidate acceptance rule, dedup of duplicate
  registration sites, and preference for the proven pointer-model target. Per-CVar failure diagnostics now separate
  "literal not found" (version-conditional/absent CVar) from "found but no validated object".
- Validation: Industria 2 Demo (UE 5.6.1) installs 32/38 requested CVars; Talos Reawakened (UE 5.4.4) installs 31/40.
  `ShowFlag.*` CVars are composed at runtime (`ShowFlag.%s`) in both engines, so vignette disabling needs a
  runtime-composed-name discovery (open). `r.MegaLights.DownsampleMode`/`r.Tonemapper.GrainQuantization` are absent in
  both; `DownsampleCheckerboard`/`MegaLights.NumSamplesPerPixel` are 5.6-only; Talos's
  `ScreenProbeGather.Temporal.MaxFramesAccumulated` has a per-title layout variance (ref+0x50 points into `.pdata`).

### 2026-08-15 - UE5 internal fps limiter and internal anisotropic filtering overrides

- `[UE5] internal_fps_limit=default|off|1..1000` overrides UE5's own `t.MaxFPS` engine frame rate limiter (float
  CVar, read on the game thread); `off`/`0` disables it, fractional values like `59.94` are accepted. It is
  independent of CaptureEngine's own fps limiter and both can pace at once.
- `[UE5] internal_anisotropic_filtering=default|off|1x|2x|4x|8x|16x` sets `r.MaxAnisotropy` and `r.VT.MaxAnisotropy`
  (int32 render-thread CVars) to one shared level, separate from the general `[Graphics] anisotropic_filtering`
  sampler override.
- Both reuse the typed persistent CVar scanner; shared-memory ABI 40 appends `internalFpsLimit` and
  `internalAnisotropicFiltering`. The literal scanner's first-character filter now admits `t` (`t.MaxFPS`).
  Config/template, profile, exact-policy, and mapping-name regression coverage was extended.

### 2026-08-15 - Runtime overlay toggle hotkey

- The previously dead `toggle_fps` hotkey became `toggle_overlay` (default `CTRL+8`). The controller registers
  `HOTKEY_ID_TOGGLE_OVERLAY`, dispatches it in the `WM_HOTKEY` loop, and forwards `ProcessCommand::ToggleOverlay`
  over the authenticated channel; the inject process flips `overlayConfig.showOverlay` in shared memory under the
  single-writer seqlock and acknowledges. The toggle is a runtime-only override: a config hot-reload or a newly
  injected target restores the `[Overlay] enabled` baseline. The old `toggle_fps` key no longer parses.

### 2026-08-15 - UE5 persistent RR-quality and post-processing override bundles

- `[UE5]` is now the canonical home for `force_ray_reconstruction`; `[DLSS]` and `[Graphics]` remain compatibility
  inputs, including legacy section-qualified profile overrides. New live options are
  `ray_reconstruction_optimal_settings`, `disable_post_processing_effects`, and `tonemapper_sharpen=default|0..10`;
  the explicit sharpen value wins over the disable bundle's sharpen=0.
- The existing validated DenoiserMode pointer redirection became a typed multi-CVar scanner. One read-section pass
  finds every requested exact UTF-16 name, one executable pass scores all surviving static CVar objects, and each
  accepted `Ref` points at process-lifetime game/render shadow storage. Config disable, module unload/reload,
  resident-hook dormancy, and shutdown restore safely; no Engine.ini or other game file is written.
- The RR bundle contains the requested 29 DenoiserMode/Lumen/VSM/MegaLights values. The post bundle uses dedicated
  sharpen, film-grain, motion-blur, scene-fringe, and `ShowFlag.*` CVars, including vignette, without lowering
  `r.Tonemapper.Quality`. Arbitrary post-process materials remain enabled because UE exposes no generic semantic
  marker that distinguishes custom sharpening from damage/underwater/accessibility rendering.
- Shared-memory ABI 39 appends the two policy flags and sharpen value. Config/template, profile, exact-policy,
  persistence-source, and mapping-name regression coverage is included.

### 2026-08-15 - Fresh config template: per-profile ThirdParty overrides + wgc_same_device_capture canonical home

- `captureengine/config.ini.template`'s `[Profile.My Game]` example now lists the three per-application
  `ThirdParty.*_dll_path` override keys (file/folder rules as in `[ThirdParty]`; an empty profile value
  inherits the global path and cannot disable a globally configured tool per app). `tests/test_config.cpp`
  template-parity assertions cover the new example lines.
- `wgc_same_device_capture` was sitting under `[Diagnostics]` in the template while the loader reads it from
  `[WGC]` (legacy `[General]`), so the fresh-config key was dead. Moved it to `[WGC]`; parity test now asserts
  it appears inside `[WGC]` and not in the `[Diagnostics]` span.

