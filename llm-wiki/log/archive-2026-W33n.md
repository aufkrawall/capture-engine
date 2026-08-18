# llm-wiki Log Archive 2026-W33n

Covers 2026-08-16. Newest-first.

### 2026-08-16 - Steam's overlay stopped working next to CE's: entry ownership was decided by a race CE now always wins

- Crashes gone (0.1.6122 confirmed on hardware), but the Steam overlay no longer rendered in Cyberpunk
  (`20260816_154722`, four launches, identical evidence).
- CE owned the `dxgi!CDXGISwapChain::Present` entry: `[OVERLAY LAYER] CE composites ABOVE the foreign Present chain
  (site=present-entry-patch … foreignOverlays=1)`, and `g_externalOverlayHook=0000000000000000` in every frame's
  diagnostics — CE never observed *any* foreign patch on that entry, i.e. **Steam never hooked Present at all**.
  Steam does patch `CreateSwapChainForHwnd` (CE bypasses that E9 for its temp swapchain), so its hooking engine is
  alive and simply declined an entry that already carried a foreign `E9`.
- **Why now:** `ShouldLeavePresentEntryToForeignOverlayChain` required, with exactly one overlay, that the foreign
  patch be *visible right now* — "an unpatched entry has no chain to go below". But Steam hooks Present only when
  the game's first swapchain appears (`15:47:41.8`), while CE's guarded temp swapchain installs Present hooks as
  soon as the hook thread runs (`15:47:36.8`). The previous fix, which made that guarded route work at all without
  a dxgi proxy, therefore turned a latent coin flip into a systematic CE win — and CE winning it is exactly the
  documented way to break the other overlay.
- **Fix (0.1.6123):** the decision takes the loaded overlay MODULE count and nothing else —
  `ShouldLeavePresentEntryToForeignOverlayChain(loadedOverlayCount)`, one argument, `>= 1`. A loaded overlay owns
  that entry whether or not it has patched it yet. CE takes its deep body hook below the chain, which is a
  full-strength Present view (it also covers swapchains that pre-date injection) and makes CE topmost — the
  project rule anyway. `InstallDeepHook` already handles a pristine prolog: with no jump visible it uses the
  caller's observed span, and the install site passes the widest form CE recognizes (14 bytes) precisely so a later
  foreign `E9` or `FF25` cannot land inside CE's patch.
- Nothing else changed: the prepend survives as the refusal fallback against a single overlay
  (`MayPrependPresentEntryWhenBelowChainViewUnavailable`), and with zero overlay modules CE still takes the entry
  outright.
- Residual, deliberately not addressed: an overlay that loads *after* CE's Present install still meets CE's entry
  patch (`main_overlay_detect.cpp` logs that join). Steam and RTSS inject at process start, so the install-time
  decision covers the real cases; un-prepending live from the loader callback would need thread quiescing under
  the loader lock.
- **Needs a hardware check:** Shift+Tab in Cyberpunk with the CE overlay active, plus one DLSS-FG and one FSR-FG
  run to confirm the below-the-chain topology holds across FG transitions.

### 2026-08-16 - Cyberpunk still crashed on start: CE's Streamline override was applied to HALF the plugin set

- `20260816_153027`, build 0.1.6121 (with the duplicate-instance fix from the entry below). Same crash site as the
  earlier `20260816_032716` session: `Cyberpunk2077 -> sl_reflex -> sl.common`, `call [rax+0x50]` with `rax=0`.
- **The dump names the bug outright — one Streamline runtime, three versions:**
  | module | version | path |
  | --- | --- | --- |
  | `sl.interposer` | 2.7.1 | game dir (`H:\SteamLibrary\…\bin\x64`) |
  | `1B0_E658703.dll` = **sl.common** | **2.11.0** | `C:\ProgramData\NVIDIA\NGX\models\sl_common_0\…` |
  | `sl.reflex` (and dlss_g / dlss_d / pcl / nis) | **2.12.0** | the configured `streamline_dll_path` override dir |
  sl.reflex 2.12 asked that 2.11 sl.common for an interface it does not provide and called through the null result.
- **Why the mix:** NGX loaded its OTA sl.common at `15:30:37.416`; CE's `LdrLoadDll` redirect was armed at
  `15:30:37.879` — **463 ms too late**. Streamline had already resolved its core, so that load could not be
  redirected. Every *later* plugin load (39.2x) did reach the redirect and became the npi 2.12 copy. CE's preload
  also added a third, unused sl.common 2.12 at 37.584, because Streamline had probed and unloaded the game's own
  `sl.common.dll` (37.435 → 37.477) and `GetModuleHandleA` therefore found nothing.
- **Fix (0.1.6122): the Streamline override is all-or-nothing, anchored on `sl.common`.** See the invariant in
  `graphics-overrides-and-frame-pacing.md`. Once an image providing sl.common is seen outside the override
  location, a latch refuses every `sl.*` redirect and the whole `sl.*` preload set; the game/driver keeps one
  coherent distribution. Answering "which plugin does this image provide" needs the resolved full path
  (`ResolveStreamlineProvidedDllName`), because the NGX cache names every plugin `1B0_E658703.dll`. A startup
  module scan covers cores that predate CE. `nvngx_dlss*` / `nvngx_deepdvc` / `nvlowlatencyvk` are outside the gate.
- **Why not "arm the redirect earlier" instead:** the hook thread reaches `PreloadConfiguredGraphicsRuntimeDlls`
  after the crash-handler and IAT passes, and the `LdrLoadDll` inline hook quiesces peer threads — moving either
  into `DllMain` (loader lock) is not acceptable. Winning the race is timing-dependent by nature; refusing a
  partial override is not. When CE *does* win (session `20260816_045933`: CE's sl.common preloaded first, the later
  NGX `sl_common_0` load redirected onto it), the latch never sets and the override applies in full.
- Confirmed working in this session's log: the duplicate guard from the entry below fired correctly —
  `Loader redirect refused for …\npi\sl\sl.dlss.dll: sl.dlss.dll is already loaded from H:\SteamLibrary\…`.
- **Still not hardware-validated.** Unit/source-policy tests only; a real Cyberpunk launch has to confirm it.

### 2026-08-16 - Cyberpunk crashed on start inside Streamline: CE's own DLL redirect turned a module *pin* into a duplicate `sl.interposer`

- Two crashes, both inside NVIDIA Streamline, both `0xC0000005` on a null this-pointer, neither in CE code
  (build 0.1.6120): `20260816_045933` six seconds after inject — `Cyberpunk2077 -> sl_interposer -> sl_dlss_g+0x4df9f`,
  `cmp [rax+8],r12` with `rax=0`; and `20260816_032716` after a long session — `Cyberpunk2077 -> sl_reflex ->
  sl_common(1B0_E658703.dll)`, `call [rax+0x50]` with `rax=0`.
- **Root cause chain (all CE):**
  1. `ScopedStreamlineFeatureQueryGuard` pins every loaded `sl.*` module for a feature query.
     `PinLoadedStreamlineModule` did that with `GetModuleFileNameA` + `LoadLibraryA(<that exact path>)`.
  2. CE hooks `ntdll!LdrLoadDll` process-globally when a runtime override is configured, so **CE's own pin was
     redirected**: `LoadLibraryA("H:\…\Cyberpunk 2077\bin\x64\sl.interposer.dll")` became a load of
     `C:\…\npi\sl\sl.interposer.dll` — a different file, therefore a **second live sl.interposer instance**
     (`Loader: runtime module loaded: sl.interposer.dll -> …\npi\sl\… (base=00007FFB7DAF0000)`, later
     `base=0000030092670000`).
  3. `NotifyHookModuleLoaded` ran the full hook pipeline on the duplicate. CE forwards each Streamline export
     through ONE process-global `original` pointer, so `InstallInlineHookOnce` **overwrote** the live interposer's
     forward pointers with trampolines into the duplicate.
  4. `pinned != module` → `FreeLibrary` → the duplicate unmapped → the unload invalidation nulled those slots.
- **Result:** the live `sl.interposer` was still entry-patched to CE, but `Hooked_slSetTag` /
  `slSetTagForFrame` / `slSetD3DDevice` / `slEvaluateFeature` had no original to call and returned
  `kSlResultErrorInvalidState` **without ever reaching Streamline**. The game ignored the errors and ran on state
  that was never established; sl.dlss_g / sl.common then dereferenced null. Log signature: `Invalidated <symbol>
  hook slot for unloaded sl.interposer.dll` with no later `Reconciled rediscovered …` line.
- **Fixes (0.1.6121), three layers, all generic:**
  - `GetRedirectedPath` refuses any redirect whose **target base name** is already loaded from a different file
    (`RedirectWouldDuplicateLoadedModule`, both decision points). See `graphics-overrides-and-frame-pacing.md`.
  - `PinLoadedStreamlineModule` pins by address (`GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS`), never by re-loading a
    path. A path load is a fresh loader resolution, not a pin. Side effect: with an override dir configured the pin
    previously *always* failed, so proactive DLSS-G/Reflex feature resolution never ran for those users.
  - `InstallInlineHookOnce` refuses to retarget a slot while the installed target is still mapped
    (`ShouldRetargetStreamlineHookSlot`). One forward pointer cannot serve two live targets; CE loses visibility
    into a duplicate rather than misrouting the live instance.
- **Second bug in the same session — no overlay, and an unbounded leak.** With Steam loaded before the game's first
  D3D12 device, the deferral falls back to the guarded system-DXGI temp-swapchain route — which was gated on
  `hSystemDXGI != hDXGI`, i.e. on a **dxgi proxy existing**. Cyberpunk has none, so the route never ran: every
  service pass (~120 ms) reported `Failed to create temp swapchain (hr=0x80004005)` from the untouched initial
  `E_FAIL`, built a fresh `CreateDXGIFactory1` bypass trampoline (65 executable pools in one second) and created a
  throwaway D3D12 device. Present hooks never arrived.
  - What makes the route safe next to a foreign overlay is the guarded creation itself (refuses a slot owned by a
    foreign module, steps over a foreign entry patch), not the proxy. With no proxy the factory built from the
    bypassed genuine export already **is** the system factory, so the guarded-only caller now uses it. The
    unguarded historical fallback stays deferred, unchanged.
  - `InlineHook::CreateBypassTrampoline` now caches per `(target, resumeOffset)` — a bypass trampoline is a pure
    function of those plus the disk bytes. The resume offset is part of the key so a later, longer foreign patch
    never reuses a trampoline that would resume inside it.
  - `TryInstallPresentHooksViaGuardedTempSwapchain` bounds the retry (120 attempts); a structurally refused slot
    must not cost a throwaway device forever.
- **Not yet validated on hardware.** Both fixes are proven by unit/source-policy tests only; a real Cyberpunk launch
  (Steam overlay + Streamline + the `npi` override dir) still has to confirm no crash and a visible overlay.

### 2026-08-16 - No overlay at all under Steam: late injection left `WasD3D12DeviceCreated()` false forever

- `dx12_fg_switch_test` launched through Steam with FG off showed **no overlay at all** (`20260816_023850`,
  build 0.1.6117). Present hooks were never installed, so there was nothing to draw with.
- CE injected **after** the app had created its D3D12 device, so the `D3D12CreateDevice` hook never fired:
  `Detected D3D12 runtime presence. Initializing DX12 hook instance... (deviceCreated=0)`. With Steam loaded,
  `DX12Hook::Init` deferred the eager temp-swapchain Present hook install, and the postponed install then waited
  on `WasD3D12DeviceCreated()` — which had already happened and could never be observed again. Postponement never
  released; session ran 7 s with no `Present hooks installed` line.
- `WasD3D12DeviceCreated()` is asked "does the game's D3D12 device exist" but only ever answered "did CE observe
  `D3D12CreateDevice`". Those differ for the entire life of a late-injected process. A command queue handed to
  `CreateSwapChainFor*` cannot exist without its device, so CE already held the proof at
  `CaptureAndHookD3D12QueueFromFactoryDevice` and simply never recorded it. Now marked there, **before** the
  FG-runtime skip and the invisible-window bypass, both of which return early and would swallow it.
  `PublishD3D12UseFromPresentedSwapchain` cannot cover this — it runs from ProcessFrame, which needs the very
  Present hooks that are missing.
- Contributing factor worth knowing: the app's swapchain was created for a not-yet-shown window, so
  `ShouldBypassInvisibleWindowCreateSwapchainSideEffects` skipped the Present refresh that would otherwise have
  installed hooks from the game's real swapchain. Both routes to Present hooks were therefore blocked at once.
  **Stale-risk:** that bypass decides from `IsWindowVisible` at creation time and never re-evaluates, which is a
  transient property for the common create-hidden-then-`ShowWindow` pattern. Not changed here.
- **Process note:** the earlier FSR-heuristic fix (6ce3718d) was chased from the *first* log (`20260816_021557`)
  and is a real defect, but it was **not** this report's cause. The user's scenario was "all FG off"; that first
  log happened to contain an FSR enable/disable cycle and was misread as the reproduction.

### 2026-08-16 - Heuristic FSR outvoted an authoritative FFX OFF and hid the overlay; FG-toggle "flicker" was a metric bug

- **Overlay lost entirely in `dx12_fg_switch_test` under Steam once FG was switched off** (`20260816_021557`,
  build 0.1.6113). The app disabled frame generation through `ffxConfigure`; CE saw it
  (`Frame Generation configure transition DISABLED` → `FG: FSR FG API DEACTIVATED`) and `SetFSRFGActive(false)`
  cleared the heuristic latch. **5 ms later** `FG: Heuristic FSR FG ACTIVATED` re-latched it from queue/present
  shape alone, and routing stayed `runtime=FSR_FG fsrFG=1 path=scQueue(FSR-FG)` for the whole session - so CE kept
  composing into AMD's UI resource, which nothing consumes with FG off. Overlay never reached the screen again.
- Fix: an observed `ffxConfigure` that changes the enabled state latches an authoritative-off state
  (`FGCompatibility::NotifyAuthoritativeFSRFGApiTransition`), and `UpdateHeuristicFSRFGState` refuses to activate
  while it holds (`ShouldSuppressHeuristicFSRAfterAuthoritativeApiOff`). Released by an authoritative on - a later
  enabled configure, or `ffxCreateContext` via `SetFSRFGActive(true)` - so re-enable and context-recreate are never
  held off. Deliberately **not** inferred from `SetFSRFGActive(false)`: several callers clear that heuristically
  (stale-latch recovery after a real-frame streak, `dx12_hook_process.cpp`), and a heuristic clear must not read as
  the game saying FG is off. A title whose FFX transitions CE never observes is unaffected.
- **The FG-transition "overlay flicker" from `20260816_014003` was not a dropout.** 201 uncovered presents in the
  toggle window matched exactly 201 `PostSL keep-alive render after explicit Streamline OFF` submits, all
  `completed=1` — the overlay was drawn every time. `DX12_TryRenderExactPostSLOffKeepAliveBeforePresent` ran
  `PostSLOverlayRenderGated` without claiming the draw for the enclosing present, so the callback accounted itself
  *and* `DX12_ProcessFrameExternal` accounted the same physical present, the second time with the draw already
  consumed. One present, one covered plus one uncovered entry, alternating. The attribution mechanism and its
  policy comment already described this exact failure; it was only applied to the in-ProcessFrame fallback submit,
  not the pre-routing one. Fixed in 8fa10cee — accounting only, no change to when or where the overlay is drawn.
  **Diagnostic signature worth remembering: alternating single-present uncovered streaks (`missed=1`,
  `longestStreak=1`) with submits logged `completed=1` means double-accounting, not a blank.**
- UE5 diagnostics: the console-registry install line now logs the local fallback pair beside the pointed value
  (the only in-process signal that CE wrote a slot the engine does not read - the open `ShowFlag.*` question), and
  a CVar the game keeps rewriting is now reported on a widening scale instead of falling silent after the
  three-report drift cap.

### 2026-08-16 - Absence is proved from the map's allocation, not the whole heap

- Talos `20260816_014003` (7 min, build 0.1.6111) was the first session long enough to reach both terminal states,
  and it showed the whole-heap completeness criterion was simply wrong. The sweep read **3698 MB across 32 passes,
  never reached the end, and gave up** - then the closing verdict correctly reported `4 not found in the 3698 MB
  swept before the sweep stopped at ...; those 4 are unproven, not known-absent`. Aspect 2 works exactly as
  intended; aspect 1's bar was unreachable.
- The bar was wrong, not the budget. A `TMap`'s elements live in **one allocation**; VirtualQuery reports it in
  pieces that all share `AllocationBase`. Covering that allocation answers "is this name registered" outright -
  Talos's map was three regions inside the first 180 MB. `ExpandToMapAllocations` now adds the remaining regions of
  the owning allocation in a single enumeration, sets `allocationCovered`, and **stops the sweep**, removing the
  32-pass background scan entirely. Fails closed above 256 MB of expansion (a shared pool, not a discrete block),
  falling back to the whole-heap sweep. `SweepProvesAbsence` is now `allocationCovered || (complete &&
  !budgetExhausted)`.
- **Restore is unexercised, not broken.** Five sessions, zero `restored the game's` lines - because none triggered
  a path that needs it. `ShutdownOverrides` runs from the hook thread's exit (only on lifecycle failure; normal
  host shutdown leaves the thread resident) and from `DeactivateHookRuntimeAndWaitForHost` (host dies while the
  game lives). In `20260816_014003` the game exited at 01:47:11 and the controller followed at 01:47:35 - the
  opposite order, and process teardown makes restore moot anyway. To exercise it: close CaptureEngine while the
  game keeps running, or toggle the profile's `ue5.*` keys off mid-session.
- **201 "uncovered" presents during the FG-transition window** (`01:40:52.714`-`01:40:57.279`), in a strict
  alternation (`missed=1`, `longestStreak=1`, 106 logged streaks), `source=ProcessFrameExternal` on every
  interrupted present and `source=PostSL` on every restoring one. Read as a half-rate overlay at the time.
  **Superseded: this was a coverage double-accounting artifact, not a dropout — see the 2026-08-16 entry above.**
  The suspicion recorded here (pre-SL keep-alive not engaging) was wrong; the keep-alive was submitting on every
  one of those presents.
- Otherwise the session was clean: 54,176 frames, avg 136 fps, `render%=100%`, RR rendering, no device removal,
  no access violations, `verified 35/35 re-asserted=0` (Talos does not rewrite its CVars, unlike Industria).

### 2026-08-16 - Ref redirect never mirrored its shadow pair: 15 of 38 overrides were inert (CA solved)

- Chasing "chromatic aberration is still visible in the Industria 2 main menu despite `r.SceneColorFringeQuality=0`
  installed and `verified`" found a defect far bigger than the symptom. `ApplyCandidate`'s **Ref redirect** branch
  repointed the `TAutoConsoleVariable` wrapper's `Ref` at CE's shadow and stopped there - it never called
  `ApplyRestorePlan`, so `writeThrough=0` for every Ref-mode CVar in every session ever logged.
- Why that matters: repointing only reaches readers that go back through the wrapper. UE's renderer typically
  resolves `IConsoleManager::FindTConsoleVariableDataInt(...)` once into a static and then reads the returned
  `TConsoleVariableData<T>*` - the exact pointer CE swapped out - directly. Those readers kept the game's value.
- Why nobody noticed: `VerifyOverrides` read **CE's own shadow** for this mode and compared it to CE's own
  configured value, so it always agreed. `verified 38/38` was self-confirming for Ref-mode CVars.
- Fix: mirror CE's value into that `{game, render}` pair as well, recording both slots first
  (`MakeReferencePlan`/`CanMirror` in `ue5_redirect_plan.h`, mirroring the existing data-pointer contract), restore
  values before the pointer on undo, and verify by reading the pair back as `through=`.
- First run with the fix (`20260816_012826`, build 0.1.6111): every Ref install now logs `writeThrough=1`, and the
  first verification pass reported **`verified 23/38 ... re-asserted=15`** - 15 overrides had been inert.
  `r.SceneColorFringeQuality through=0x1 expected=0x0`, `r.MotionBlurQuality 0x4 vs 0x0`, `r.MaxAnisotropy` and
  `r.VT.MaxAnisotropy 0x8 vs 0x10`, both `r.Shadow.Virtual.ResolutionLodBiasLocal*`. Second pass: `verified 38/38`.
  User confirmed the chromatic aberration is gone. So `r.SceneColorFringeQuality`, not `ShowFlag.SceneColorFringe`,
  is what gates the effect in UE 5.6 - and forced AF had been ineffective in-engine too.
- Same build: the registry sweep now enumerates from its cursor instead of from address 0
  (`CollectHeapRegions(fromAddress)`). Re-walking the address space with `VirtualQuery` had been eating the pass
  budget as the game's address space grew - `20260816_011313` collapsed from 211 MB on pass 1 to 4-8 MB on passes
  2-4. With the fix, `enumerate=` is 0-109 ms and pass 16 covered 768 MB (the per-pass byte cap) instead of 10 MB;
  1848 MB total by pass 16 versus 903 MB before. Pause lines now report `enumerate=Nms` so the two costs stay
  separable. Neither session ran long enough to reach `sweep complete`.
- Unrelated flake seen once: the full unit suite died at `ScreenshotWorkerTest.ReportsWorkerPublicationFailure...`
  while a live CE session was shutting down; passes standalone and on a clean re-run. Possible test-isolation gap.

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
