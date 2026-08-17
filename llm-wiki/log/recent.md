# llm-wiki Log

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

