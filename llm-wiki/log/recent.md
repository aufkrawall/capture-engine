# llm-wiki Log

### 2026-08-18 - Screenshots on the present thread: a hard freeze, then the overlay in an overlay-free shot

Two defects on the same path, both in Gothic 1 Remake under DLSS MSFG 4x. Fixed in 0.1.6150.

- **The freeze** (session `20260818_172155`, `screenshot_include_overlay=false`). `SaveDX12TextureAsScreenshotRaw`
  ended in `WaitForSingleObject(fenceEvent, INFINITE)` after submitting the backbuffer copy. That runs on the
  present path, and under DLSS FG the copy goes on the game's own swapchain queue - which the runtime only drains
  once its presenter thread makes progress, and that thread cannot progress while the present call it drives sits
  blocked in our hook. Render thread wedged at 17:23:34.4, watchdog fired 30 s later. The same wait had already
  been visible as a **1.8 s stall** on the earlier successful shot at 17:22:58; it just had not closed the ring yet.
  Exactly the boundary the FFX work recorded in 2026-07 ("never do blocking work on the presenter thread"), DLSS-G
  edition.
- **Fix:** record, submit, `Signal`, hand the resources to the screenshot worker, return. The worker waits in
  1 s slices, checks `GetDeviceRemovedReason()` between them so a dead device cannot park it, and **strands rather
  than releases** resources a live GPU may still be reading. The producer reserves the worker's single slot
  *before* recording any GPU work, so a submitted copy can never be stranded by a busy queue - the only other exits
  would be the blocking wait we just removed or a use-after-free. Worker queue and task moved to
  `hook/common/screenshot_worker.{h,cpp}`; the Vulkan layer links an explicit source list and needed it added.
- **Then the overlay showed up anyway** (session `20260818_205615`, build 0.1.6148: `Saved (hook)`, no freeze, no
  GDI fallback - but the overlay was in the picture). The overlay-free capture sat at the top of
  `DX12_ProcessFrameExternal`, which is early enough only when `ProcessFrame` draws the overlay. Under FG it does
  not: `postSLCallback=1 postSLActive=1 skip=1`, and `DetourPresent` runs `ExecuteStartupRouting` - **which invokes
  the PostSL overlay draw** - before `ExecutePresentCore` reaches `ProcessFrame`. So the "pre-overlay" capture was
  post-overlay.
- **Fix:** whoever draws the overlay owns the ordering for both screenshot variants. `PostSLOwnsThisFramesOverlayDraw()`
  (renamed from `ShouldUseConfirmedPostSLForOverlayIncludedWork` - it was never only about the included case) now
  routes *both* into the PostSL submit chunk: overlay-free immediately ahead of the overlay list on the same queue,
  overlay-included after it. `ProcessFrame` yields both when that predicate holds.
- The chunk has **sixteen mutually exclusive submit branches** across four possible queues, so the capture is
  inserted before each rather than hoisted (hoisting would either duplicate the branch selection or guess the
  queue). `ScreenshotPresentThreadPolicyTest.EveryPostSLOverlaySubmitCopiesTheOverlayFreeFrameFirst` parses the
  file and fails if any submit is not immediately preceded by the capture call *for that same queue*, which is what
  keeps the arrangement from drifting.
- Unrelated diagnostics gap found on the way: `UpdateSharedMemoryFromConfig`'s summary hash omitted every overlay
  boolean, so flipping `screenshot_include_overlay` live published a new config and logged **nothing**. The
  publication line is the only record that a setting reached the hook; the four booleans are in the hash now.

### 2026-08-18 - One overlay-toggle press wiped the active profile: UE5, DLSS and graphics overrides all died

Session `20260818_164520` (Gothic 1 Remake, `G1R-Win64-Shipping.exe`, DLSS MFG 4x, `[Profile.gothicremake]`).
Fixed in 0.1.6144.

- **Symptom.** `16:46:22.062 [Controller] Overlay toggle hotkey handled`, then 84 ms later the hook restored
  all 35 installed CVar shadows with reason `configuration disabled`. The same instant, Streamline stopped
  overriding DLSS-G: `generatedFrames=1->3` every present up to `16:46:22.057`, then
  `slDLSSGSetOptions ... generated=1->1 ... override=0`. Not two bugs — one.
- **Root cause.** `inject_main.cpp`'s `ProcessCommand::ToggleOverlay` handler flipped
  `currentConfig.overlay.showOverlay` and republished `currentConfig` — the **base** config, with no
  `[Profile.*]` applied. `inject.log` shows it exactly: the publication at 16:45:27.981 carried
  `vsync=fifo cpuPrerender=1.00 srPreset=13 forceRR=1 ue5InternalAF=16 ue5InternalTextureMipBias=-2.00`; the one
  at 16:46:22.062 carried `vsync=default cpuPrerender=-1.00 srPreset=0 forceRR=0 ue5InternalAF=0`. `dlss_fg_factor=4x`
  went with it, which is why MFG dropped to 1x. The `ReloadConfig` handler already resolved the target
  (`ResolveActiveTargetConfig`); the toggle handler simply never did.
- **Fix.** The toggle is now a runtime `OverlayVisibilityOverride` carried *beside* the config instead of an edit
  of it, and **every** publication site (startup, injection callback, hook-source detection, reload, toggle) goes
  through one `PublishConfigLocked`, so `UpdateSharedMemoryFromConfig` is called from exactly one place — a
  structural invariant `ProcessIPCTest.OverlayToggleHotkeyIsWiredEndToEnd` now asserts by counting call sites.
- Two defects fell out of the same handler and are fixed with it. The press flips the **resolved** visibility, so
  a profile overriding `[Overlay] enabled` can no longer make the first press a no-op; and the override now
  survives republication, so an injection or hook-source event cannot silently snap the overlay back on. A config
  reload still clears it — the file is the declared state again.
- **A real race, not a theoretical one.** `InjectionManager::Inject` runs the `onInject` callback on a delayed
  injection worker thread, and that callback published too. Two threads entering `BeginWriteOverlayConfig` leaves
  the sequence even while one is still writing, so the hook could accept a torn `overlayConfig`; the shared
  `configSummaryHash` static was racy as well. One publication mutex makes the seqlock genuinely single-writer.
- Target identity for resolution: the last target the injector/hook-source path identified wins, with the live
  `GetSourcePid()` name as fallback and `ClearStaleHookSourceState` clearing it when the process dies. That
  closes the window between injection and the hook publishing its source PID, where a toggle used to fall back
  to the base config even with a target running.
- Publication state lives in a function-local-static `PublicationState`, not namespace-scope globals:
  `AppConfig`'s defaults allocate, and `bugprone-throwing-static-initialization` (correctly) fails lint on that.

### 2026-08-17 - Wukong exit crash: three separate bugs behind one "crashes on close"

Session `20260817_052857` (Black Myth: Wukong, DLSS FG, Steam overlay loaded). Fixed in 0.1.6143.

- **The crash: a use-after-free of `g_pLocalConfig` during `LdrShutdownProcess`.** `crash.log` names the address;
  it resolves to `GetRedirectedPath+0x282`, `movzx eax,[r12+4B0h]` where `r12 = g_pLocalConfig` — and
  `AppConfig::graphics` (+0x180) `+ GraphicsConfig::streamlineDllPath` (+0x330) is exactly 0x4B0, i.e. the
  `.empty()` on line 208 of `main_redirect.cpp`. So the pointer, not the string, was dead.
- Why it was dead: the exit path took `DllMain(DETACH, lpReserved != NULL)`, which set only `g_ProcessTerminating`
  and returned. `RequestHookShutdown()` was called **only** on the dynamic-unload branch, so `HookIsShuttingDown()`
  stayed false for the rest of teardown even though the five loader hooks already had the guard. Our own static
  destructors then ran (`PerfLogger: Shutdown` in `hook_debug.log` is the timestamp for that), and ~1 s later
  something still loading DLLs from its own detach path came through `HookedLoadLibraryEx*`.
- Fix, three layers: latch `RequestHookShutdown()` in the process-exit detach branch **and** in the attach-time
  `atexit` handler (LIFO puts it ahead of this module's globals, so ordering between the two is irrelevant), and
  give the config module storage that is never destroyed. The general rule: **anything CE owns for the process
  lifetime must outlive static destruction**, because a pinned image's hooks stay callable after the CRT is done.
- **The 2-minute hang: a benign exception classified as a crash.** CEF's exit-time
  `WTSUnRegisterSessionNotification` cancels an async RPC wait and rpcrt4 raises `RPC_S_CALL_CANCELLED`
  (`0x0000071A`). The VEH's "dump everything not known-benign" policy dumped it — twice, ~62 s each — and then
  had no dump budget left for the real AV. The filter now classifies by NTSTATUS severity: below error severity
  only the explicitly listed codes (breakpoint, UE5 `ensure`, the COM/DXGI set with their existing thresholds)
  are dump-worthy. Nothing is lost — an unhandled exception still re-enters via the top-level filter with
  `forceDump`.
- **Why a `MiniDumpNormal` took 61.6 s at all** (twice, to the millisecond — a fixed cost, not a lock): dbghelp
  reads every module's version resource, and with the Steam overlay loaded each query round-trips through its
  loader hooks while every other thread is suspended. This is the same hazard `crash_dump_policy.h` already
  documented for the fatal-exit path (`20260813_222058`) — it just had never been applied to the VEH worker. The
  worker now prefers the external `captureengine.exe --dump-helper` process and refuses the in-process fallback
  when a foreign overlay is loaded; the hook publishes both via `RegisterCrashDumpEnvironmentHooks`.
- Worth remembering for future dump reading: `GetExitCodeProcess` stops returning `STILL_ACTIVE` as soon as
  `RtlExitUserProcess` starts, so `[Inject] Tracked injected process exited ... exit=0x00000000` can be logged
  while the main thread is still running DLL detach — and an AV timestamped *after* it is not a contradiction.
- **Validated** on `20260817_055930` (user-confirmed clean exit): no dumps, no `0x0000071A`, no teardown freeze.
- Follow-up that turned out **not** to be a CE bug: Steam's overlay never drew in that session. It is missing
  without CE injected too — Steam ships the Wukong *Benchmark Tool* as a Tool, and the overlay is disabled for
  those. `gameoverlayrenderer64.dll` still loads (so `steam_overlay_loaded=1` and the leave-the-entry mode engages
  exactly as designed), which makes the state look identical to the Cyberpunk `20260816_154722` regression. The
  distinguishing evidence is the same in both, so it cannot separate them: `foreignJumpVisibleNow=0`,
  `g_externalOverlayHook=0`, per-frame `cmdLists=2` (game + CE, no third submitter). CE's own topology was
  healthy — entry left pristine, deep body hooks on `Present`/`Present1` at +14,
  `[OVERLAY LAYER] ... BELOW the foreign Present chain`, and the pre-fix session `20260817_052857` logged byte-for-byte
  the same decisions. **Before treating "Steam's overlay is missing" as a coexistence regression, check the app
  without CE injected first.**

### 2026-08-16 - Display gamma override, and a general "only safe in this engine state" guard

- `[UE5] display_gamma=default|srgb|1.0..3.0` (0.1.6139, shared ABI 42). Motivated by Talos shipping a
  piecewise-sRGB vs power-2.2 option that visibly does nothing - the kind of bug CE can route around, because it
  writes the CVar directly instead of going through the game's settings code.
- Carried as the `r.TonemapperGamma` value itself: negative untouched, 0 is UE's documented "default behavior"
  (piecewise sRGB/Rec709), positive is a pure power curve. One field, one predicate, no separate mode enum.
  Only the sRGB direction writes `r.HDR.Display.OutputDevice` - UE raises the device to explicit-gamma mapping on
  its own once the exponent is positive.
- **New: `ApplyGuard`.** `r.HDR.Display.OutputDevice` doubles as the HDR output selector (3-6 are ST-2084/ScRGB),
  so writing an SDR device over it would silently drop an HDR game out of HDR and ruin the capture. A spec can now
  carry a guard judged from *the value the game currently holds*, checked at every install site before any write.
  General mechanism, not a special case - use it whenever the engine's current state decides whether a write is
  safe. Under HDR the gamma option does nothing rather than degrading anything.
- Types read from the binary again (GPR = int32 for OutputDevice, `xmm2` = float for TonemapperGamma). That check
  has now caught two mistypes and prevented a third; it should be routine for every new spec.
- The ABI-name lockstep noted last time paid off immediately: bumping 41 -> 42 needed
  `SHARED_MEM_BASE_NAME`/`SHARED_MEM_DISCOVERY` moved in the same commit.

### 2026-08-16 - UE5 texture mip bias override, and two traps it walked into

- `[UE5] internal_texture_mip_bias=default|-15.0..15.0` drives `r.MipMapLODBias` (0.1.6138, shared ABI 41).
  Distinct from the `[Graphics]` sampler mip overrides: this changes what the engine asks for, so it also moves
  texture streaming.
- **The CVar is a float, verified not assumed.** The registration passes its default in `xmm2` (`0f57d2`,
  `xorps xmm2,xmm2`) - the float overload; an int default goes in a GPR. Same check should be used for any future
  spec: it is a two-minute scan and it is exactly the bug that made
  `r.Lumen.ScreenProbeGather.Temporal.MaxFramesAccumulated` silently unreachable for its whole life.
- **0 is a real value here**, not "leave alone", unlike every other numeric UE5 knob. The untouched state is a
  sentinel outside UE's -15..15 range; `IsTextureMipBiasRequested` is the one predicate that decides it.
- Two process notes the gate taught: new specs must be **appended** to `kSpecs` (`kTonemapperSharpenIndex` is
  positional), and an ABI bump must move `SHARED_MEM_BASE_NAME`/`SHARED_MEM_DISCOVERY` too - those literals are
  hardcoded while the event names are derived, so a partial bump compiles fine and only the name test catches it.

### 2026-08-16 - Show flag force bits: one bit proven safe on two engine versions, effect still unproven

- **Handoff note.** The `ShowFlag.*` force bits are the only open part of the UE5 override work. The full state,
  measured bit maps for 5.4.4 and 5.6.1, what is proven, and the ordered next steps now live in one block in
  `graphics-overrides-and-frame-pacing.md` ("`ShowFlag.*` force bits - state of knowledge"). Read that before
  touching it rather than reconstructing it from these entries.
- 0.1.6134 drives exactly **one** bit, `ShowFlag.Vignette`, recorded and restored, compare-exchange on the shared
  word. Validated on Talos 5.4.4 (`20260816_205827`, 32/32 verified) and Industria 2 Demo 5.6.1
  (`20260816_210946`, 34/34 verified after the game's own load-time writes were re-asserted). **No visual
  regression on either.** So the four-bit write that killed lighting in 0.1.6128 was not a uniform off-by-one -
  bit 13 alone is harmless, and the culprit is one of Grain/MotionBlur/SceneColorFringe.
- Still unproven: whether the write *disables* vignette. Neither title visibly uses vignette or grain, so
  "inert" and "correct but invisible" look identical. Needs a title that shows the effect.
- Two findings that close off the alternatives: UE ships **no vignette CVar at all** (whole-binary literal
  enumeration - only the show flag name and its localization key), and `r.Tonemapper.Quality` does remove vignette
  but only as part of a cumulative ladder that takes grain and the rest with it. **User decision: tonemapper
  quality must not be reduced for this.** A test already asserts the spec table never contains it; that assertion
  is load-bearing, not incidental.
- Also corrected: the masks are read through the console object's own pointer, never RIP-relative, which is why a
  full `.text` scan finds no apply site. An earlier reading of that absence as "compiled out of Shipping" was wrong.
- Cross-version data worth keeping: mask geometry and bit indices both differ by engine version (48 B/384 flags in
  5.4.4 vs 64 B/512 in 5.6.1; GI 12 vs 14, Vignette 13 vs 15). CE reads both per object, so nothing assumes them.

### 2026-08-16 - The bit map measurement lands, and the last unreachable CVar turns out to be mistyped

- `20260816_201740` (0.1.6131, Talos): lighting is back, 31/40 installed, **verified 31/31, re-asserted 0,
  retired 0**. The four show flags are recognised as force bits and reported, not written.
- `ProbeShowFlagBitNumbers` resolved 7 of 8: **Bloom 1, Tonemapper 3, AntiAliasing 4, TemporalAA 5,
  GlobalIllumination 12, Vignette 13, Grain 14, DirectLighting 28, MotionBlur 37, SceneColorFringe 46,
  Lighting 109** (`ShowFlag.DiffuseIndirect` is not registered in Talos). GI at 12 is exactly what the binary's
  name table predicted, one below Vignette's 13.
- That settles the direction of the 0.1.6128 defect: **the console-object side is completely self-consistent** -
  distinct indices, matching the engine's own declaration order. CE set bits 13/14/37/46 and the renderer dropped
  global illumination (12), so the discrepancy is entirely on the renderer's side of the mask, not in what CE
  read. Still not enough to write by; what is missing now is how the renderer indexes the mask, not what the
  objects say.
- The rate-limited dump also cracked `r.Lumen.ScreenProbeGather.Temporal.MaxFramesAccumulated`, unreachable in
  Talos across every session: its shadow at `object+0x58` is `0x41C80000` = **25.0f**. The variable is a *float*
  and the spec table declared it `Int32`, so the Int32 plausibility check refused it every time - correctly, since
  installing it would have written 10 (a denormal float, effectively zero) into a float global and turned temporal
  accumulation off. Fixed in 0.1.6132 by typing the spec `Float`; the checks stay fail-closed the other way too,
  because an int-shaped 10 or 25 reinterprets as a denormal and is refused rather than written.
- Lesson worth keeping: a spec-table type is a claim about the engine's registration, and the plausibility check
  is the only thing standing between a wrong claim and a corrupted variable. A CVar that is "found but never
  validated" in one title and fine in another is a type mismatch until proven otherwise.

### 2026-08-16 - Driving the show flag force bits removed all lighting from Talos; CE stops writing them

- 0.1.6128 shipped the bit-ref write. Talos then rendered with no lighting at all (`20260816_165501`), and the
  install lines say why it is not a discovery failure: `force0=00007FF73B3A16C0 force1=00007FF73B3A16F0`, exactly
  0x30 apart - two adjacent 48-byte (384-flag) bit arrays, identical across all four flags. The bit numbers are
  self-consistent too: Vignette=13, Grain=14, MotionBlur=37, SceneColorFringe=46, all distinct.
- The engine's own show flag name table in the Talos binary (`0x810d0e0` `GlobalIllumination`, `0x810d108`
  `Vignette`, `0x810d120` `Grain`) lists them in that exact order, so **`GlobalIllumination` is bit 12 - one below
  the first bit CE set**. Any off-by-one between the index a console object carries and the index the renderer
  reads the mask by lands precisely on global illumination, which is the observed symptom.
- So the masks *are* live in a Shipping build (that standing open question is now answered), the discovery is
  right, and the **mapping** is what is unproven - plausibly `SHOWFLAG_FIXED_IN_SHIPPING` flags compiled out of
  one side and not the other. Writing a bit means guessing which flag gets turned off, and the guess cost a
  broken frame.
- **0.1.6131:** bit references are classified, confirmed against a second flag, and *reported only*. No force bit
  is written, so the verify/restore/update branches for that mode are gone rather than left dead. Classification
  stays - it is what stops the old redirect from replacing the engine's mask pointer, the defect that made these
  overrides inert in the first place.
- Added `ProbeShowFlagBitNumbers`: eight extra show flag names resolved through the already-anchored map purely to
  log `name -> bit` once per session. If `GlobalIllumination` reports 12 next to `Vignette`'s 13, the
  console-object side is exactly the table order and the discrepancy is entirely renderer-side.
- Net effect on the user-visible bundle: unchanged from before 0.1.6128. Grain, motion blur and chromatic
  aberration ride on `r.FilmGrain` / `r.MotionBlurQuality` / `r.SceneColorFringeQuality` (all verified live);
  vignette remains the one post-processing effect with no working lever.

### 2026-08-16 - `ShowFlag.*` was never a value variable: four UE5 overrides had been writing the wrong structure

- Follow-up to the 7-minute Talos run (`20260816_014003`, 35/40 installed, verified 35/35, re-asserted 0). The
  four `ShowFlag.*` entries counted as installed were the ones with no evidence behind them, and the `prevLocal`
  diagnostic added for exactly that question answered it: all four objects reported the **identical**
  `object+0x58` qword `0x00007FF73B3A16F0` (`20260816_161158`), an address inside the exe's writable data.
- A per-variable `{game, render}` shadow pair cannot be identical across four different variables. These are
  `FConsoleVariableBitRef` - UE registers show flags as one bit in two process-wide force masks, so the first two
  qwords are `{Force0MaskPtr, Force1MaskPtr}`. CE's fixed `ref+0x50` model read `*Force0MaskPtr` (an all-clear
  mask, which is where the long-standing `prevValue=0` came from), decided it was a plausible value, and
  CAS-replaced the mask pointer with CE's 8-byte shadow. The engine then read CE's storage as the mask: the write
  reached nothing, and the object lost its route to the real mask. Inert in every title and engine version so far.
- **Fix (0.1.6128):** classify before writing. `hook/common/ue5_console_layout.h` decides between reference
  pointer / inline pair / bit reference from what the caller read, `hook/main_ue5_layout.cpp` does the reading and
  owns the three install modes, and an object matching nothing (or one shape twice) is left untouched with its
  first 0x80 bytes dumped. The reference shape is now accepted only when the shadow pair actually mirrors the
  global the pointer addresses - the check the ShowFlag objects fail.
- Bit writes are compare-exchange on the containing word so neighbouring show flags survive, restore puts back only
  CE's bit, and verification reads both force bits straight out of the engine's masks. Two gates keep the bit path
  off everything else: `ShowFlag.` names only, and commit only once a second flag confirms the same mask pair with
  a different bit index.
- Also fixed: a mid-session configuration change that newly requested CVars could never resolve them, because the
  registry resolver stays closed once it has finished with the previous request set (`ReopenConsoleRegistry`).
- **Open:** whether a Shipping build consults the force masks at all (these flags are `SHOWFLAG_FIXED_IN_SHIPPING`
  in UE). Practically that only exposes vignette. Also open: `r.Lumen.ScreenProbeGather.Temporal.MaxFramesAccumulated`
  in Talos, which now refuses with a byte dump instead of a bare "layout does not match" line - the next Talos run
  should identify it outright.
