# UE5 CVar Overrides

Last cross-checked: 2026-08-22

Process-local, persistent overrides of Unreal Engine console variables in an injected x64 game: how CE finds a CVar's
value storage, which layouts it accepts, what it refuses, and every setting the `[UE5]` config section ships. Split
out of `graphics-overrides-and-frame-pacing.md` on 2026-08-21, which had reached the file-size ceiling.

Primary sources:
- `hook/common/{ue5_cvar_override_policy.h,ue5_console_layout.h,ue5_console_registry.h,ue5_redirect_plan.h,ue5_rr_override_policy.h}`
- `hook/main_ue5.cpp` (policy/lifecycle/service pass), `hook/main_ue5_scan.cpp` (literal + candidate discovery),
  `hook/main_ue5_install.cpp` (install, refresh, read-back verification, restore), `hook/main_ue5_layout.cpp`
  (layout classification and console-object install), `hook/main_ue5_memory.cpp` (process-memory/PE primitives),
  `hook/main_ue5_registry.cpp` (resolution through UE's console-object map)
- `common/config_load_ue5.cpp`, `common/config.h`, `common/shared_defs_detail/abi_constants_and_config.h`
- `captureengine/config.ini.template` (the user-facing contract for every key)
- `tests/{test_ue5_cvar_override_policy,test_ue5_console_layout,test_ue5_console_registry,test_ue5_rr_override_policy,test_config_ue5}.cpp`

## Persistent UE5 CVar override policy

- NVIDIA's UE plugin declares `CVarNGXDLSSDenoiserMode` as a static `TAutoConsoleVariable<int32>` and selects SR
  (`0`) versus RR (`1`) with `GetValueOnRenderThread()`. CE therefore changes the exact value source the plugin reads,
  rather than periodically issuing a console command that a map/device-profile reload can undo.
- The generalized scanner resolves every currently requested exact NUL-terminated UTF-16 CVar literal in one read-
  section pass per module, correlates their nearby x64 RIP-relative constructor/store references, and counts all
  surviving candidate references in one executable pass. It scans the main game module first, then remaining initial
  modules, and after that only newly loaded modules. Raw candidates first have to prove the writable
  24-byte `FAutoConsoleObject`/`Target`/`Ref` layout, callable object and target vtables, readable
  `TConsoleVariableData<int32>`, and plausible `{game, render}` shadow values of `0` or `1`; impossible layouts are
  discarded before one combined executable-code pass counts reference evidence for every survivor. Only one strongly
  distinguished live candidate is accepted, and ambiguous or unfamiliar layouts leave memory untouched. The single
  reference pass is an invariant: Talos exposes 35 raw nearby candidates, so a pass per candidate delayed selection
  by 6.8 seconds and changed SR to RR during live rendering.
- Graphics-hook installation always precedes the optional module scan, both on initial startup and periodic service
  passes. This ordering protects DXGI queue capture even if an unfamiliar large image remains expensive to inspect;
  RR discovery itself must also finish early enough to select the denoiser before the first DLSS feature rather than
  hot-switching an active renderer.
- CE atomically redirects only each validated `Ref` pointer to typed process-lifetime `{game,render}` shadow storage.
  The game's
  real console variable and priority/history state remain intact; later Engine.ini, scalability, level, or game code
  writes update the original shadow but cannot change the plugin's direct read. Disabling the setting or shutting the
  hook down compare/exchanges the original pointer back. Owner-module unload retires the stale address and an eventual
  reload is rescanned. Live policy/value changes restore removed CVars and atomically update retained shadows. There
  is no disk write, render-thread hook, repeated module sweep, or vtable call into unknown UE code.
- UE 5.4/5.6 (validated: Talos Reawakened 5.4.4, Industria 2 Demo 5.6.1) registers many Lumen/rendering CVars through
  a different layout: the static `FAutoConsoleVariable`-style object is a `{vtable, Ref}` pair (16 bytes) and the
  `FConsoleVariable` holds its value behind a data pointer at `ref+0x50`, with a local fallback pair at `ref+0x58`
  (verified against `IConsoleVariable::GetValueOnGameThread`, which returns `*[this+0x50]` when the CVar is
  registered and `[this+0x58]` otherwise). The scanner therefore has a second install mode, **data-pointer redirect**:
  it CAS-replaces the `ref+0x50` pointer with CE's shadow (render-thread reads) and mirrors the value into the
  `+0x58` local pair (game-thread reads). Multiple registration sites can exist per CVar with separate value records;
  duplicates are deduplicated by data pointer, a validated pointer-model candidate is preferred when present, and
  <!-- keep the data-pointer install contract next to the layout it depends on -->
  a redirect is only installed when the pointer it replaces has been recorded (`ce::ue5_redirect::MakePlan` /
  `CanInstall` in `hook/common/ue5_redirect_plan.h`). Until 2026-08-15 the data-pointer path left that record
  default-initialised, so restoring on config disable or hook shutdown compare-exchanged **null** into the live
  console object and the engine dereferenced it on its next read; 20 of Talos's 31 overrides used this path.
  The install also mirrors CE's value into the storage the original pointer addressed (`writeThrough=1` in the
  install log) and restores it on undo, because engine code generated for `FAutoConsoleVariableRef` CVars reads that
  global directly instead of going through the console object - the redirect alone would be invisible to it.
  otherwise the best-correlated data-pointer candidate is applied as a safe best effort.
- **Ref redirect mirrors its shadow pair too, and until 2026-08-16 it did not - which silently disabled most of the
  post-processing bundle.** Repointing the wrapper's `Ref` at CE's shadow only reaches readers that go back through
  the wrapper (`TAutoConsoleVariable::GetValueOnRenderThread()`). UE's renderer routinely resolves
  `IConsoleManager::FindTConsoleVariableDataInt(TEXT("r.X"))` once into a static and reads the returned
  `TConsoleVariableData<T>*` - the very pointer CE swapped out of the wrapper - directly from then on. Those readers
  never consult the wrapper again and kept seeing the game's original value. Every Ref-redirect install logged
  `writeThrough=0`, and **verification could not detect it**: for this mode it read CE's own shadow and compared it
  against CE's own configured value, so it always agreed. `verified 38/38` meant nothing here. The install now also
  mirrors into that `{game, render}` pair, recording both slots first
  (`ce::ue5_redirect::MakeReferencePlan` / `CanMirror`), restores values before the pointer on undo, and verification
  reads the pair back as `through=`. Measured impact on the first run with the fix (Industria 2, 20260816_012826):
  **15 of 38 overrides re-asserted on the first verification pass**, i.e. 15 had been inert - among them
  `r.SceneColorFringeQuality` (`through=0x1` vs `expected=0x0`), `r.MotionBlurQuality` (`0x4` vs `0x0`),
  `r.MaxAnisotropy` and `r.VT.MaxAnisotropy` (`0x8` vs `0x10`) and both `r.Shadow.Virtual.ResolutionLodBiasLocal*`.
  This is the answer to the standing "chromatic aberration is still visible in the Industria 2 main menu despite
  `r.SceneColorFringeQuality=0` verified" report: user-confirmed off after the fix.
- Version-conditional specs: some bundle CVars do not exist in every UE build. `r.MegaLights.DownsampleMode` and
  `r.Tonemapper.GrainQuantization` have no literal in UE 5.4 or 5.6; `r.Lumen.Reflections.DownsampleCheckerboard` and
  `r.MegaLights.NumSamplesPerPixel` exist in 5.6 but not 5.4. The summary log separates these ("literals not found in
  loaded modules") from present-but-unvalidated CVars. `ShowFlag.*` CVars are composed at runtime from a short-name
  table via `FString::Printf(TEXT("ShowFlag.%s"), ...)` in UE 5.4/5.6, so no `ShowFlag.X` literal exists to match and
  the literal scan alone cannot disable vignette. The composed names do exist in the game's
  writable memory as a contiguous UTF-16 table (present once a world/graphics state exists), but they are not embedded
  in the CVar objects and no FString references them inside the owning region, so a literal scan cannot reach the CVar
  objects. Film grain, motion blur, and chromatic aberration are already covered by `r.FilmGrain`,
  `r.MotionBlurQuality`, and `r.SceneColorFringeQuality`; vignette is the one post-processing effect that needs the
  registry path below, which does resolve it (both games, since 20260815_210850).
- **Console-registry resolution** (`hook/main_ue5_registry.cpp`, decoders in `hook/common/ue5_console_registry.h`)
  is the answer to both runtime-composed names and per-title layouts the candidate scoring rejects. `FConsoleManager`
  keeps a `TMap<FString, IConsoleObject*>` of every registered variable, so the composed name is present as an
  ordinary heap FString next to its object. CE does not hard-code that map's layout: it takes CVars the module scan
  already installed as **anchors**, walks committed private RW regions for a qword equal to a known object, and only
  accepts an element whose neighbouring FString also decodes to that CVar's name. That proves the key-to-value
  distance, after which the same allocation is re-read for the missing names and the resolved object is **probed for
  its layout** before anything is written (`InstallConsoleObjectOverride` in `hook/main_ue5_layout.cpp`, logged as
  `installed via console registry`).
  Constraints that keep it safe: every read is `ReadProcessMemory` rather than a raw dereference (a live game frees
  heap regions mid-walk, and the kernel-checked copy fails instead of raising); and it never runs before an anchor
  exists, so it can only ever add to what the scan proved. **The walk must not stop at the first confirmed
  element** - the map's storage does not fit in one heap region, and stopping early made the second pass re-read a
  single 64 KB region (Talos 20260815_202743: anchored in 47 ms, then failed to resolve
  `r.Lumen.ScreenProbeGather.Temporal.MaxFramesAccumulated`, which is certainly registered). It records each region
  holding confirmed elements and resolves across all of them; confirmations count distinct anchors (tracked by
  object address, not list index, because the anchor list is rebuilt each pass) so a CVar registered from several
  sites cannot inflate the ratio.
- **Covering the map's own allocation is the completeness bar, not whole-heap exhaustion.** A `TMap` keeps its
  elements in one allocation; VirtualQuery only reports it in pieces wherever protection or state changes, and all
  those pieces share `AllocationBase`. So once every region of that allocation has been read, a name absent from
  them is absent from the registry, however much unrelated heap was never touched. Sweeping the whole heap instead
  is both far more expensive and strictly weaker evidence *about the map*: Talos (20260816_014003) read 3698 MB
  across 32 passes, never reached the end, and correctly reported that it could not prove anything - while its map
  sat in three regions found inside the first 180 MB. `ExpandToMapAllocations` now pulls in the remaining regions of
  the owning allocation in one enumeration, sets `allocationCovered`, and **stops the sweep**, which also removes
  the 32-pass background scan. It fails closed: an `AllocationBase` that turns out to be a large shared pool rather
  than a discrete block exceeds `kMaxAllocationExpansionBytes` (256 MB), claims nothing, and leaves the whole-heap
  sweep as the fallback.
- **The sweep is resumable, and only a finished sweep may support an absence verdict** (`SweepProgress` and its
  predicates in `hook/common/ue5_console_registry.h`). The 400 ms / 768 MB bound is *per pass*, not per sweep:
  Industria 2 (20260815_214219) covered 218 MB in 406 ms, stopped mid-heap with 31 of 34 anchors placed, and the
  old code then froze that partial result - once `g_map.valid` was set the search never ran again, so all 90 retry
  passes re-read the same two regions and the regions the walk never reached stayed unexamined for the session.
  The sweep now carries a cursor, parks on the chunk it did *not* read (so the chunk overlap still guarantees no
  element falls through a pause), and resumes there on the next pass until the heap enumeration is exhausted;
  cumulative bounds are 32 passes / 8 GB, after which it says it gave up. The all-anchors-placed early exit was
  removed: placing every anchor does not prove every region of the map was seen, and completeness is what the
  verdict rests on. Consequently the closing summary distinguishes **"absent from the fully swept registry"** from
  **"not found in the N MB swept before the sweep stopped at X - unproven, not known-absent"**; the old wording
  called both "never registered by the engine". Resolve passes are time-boxed too (100 ms) and rotate their
  starting region, so a budget that expires cannot starve the regions behind it. Locating the map is the expensive
  half - once anchored, retries only re-read the recorded regions (up to 90 attempts at ~1 Hz), which covers
  `ShowFlag.*` appearing during world/graphics init rather than at startup. A TMap growth reallocates the element
  storage, so the representative element is re-checked each pass and a move restarts the whole sweep (cursor,
  confirmed anchors and region set included) rather than producing silent misses. The cursor only moves forward:
  memory committed below it after CE swept there is new memory, and element storage that *moves* is caught by the
  anchor re-check instead.
- **`ShowFlag.*` is `FConsoleVariableBitRef`, not a variable holding a value - and until 2026-08-16 CE drove it as
  one, which made all four show flag overrides inert.** A bit-ref's value is a single bit in two process-wide force
  masks: its first two qwords are `{Force0MaskPtr, Force1MaskPtr}` and its third field is the bit index, so the
  fixed `ref+0x50` model read `*Force0MaskPtr` (an all-clear mask, hence the long-standing `prevValue=0` in every
  title and engine version) and then CAS-replaced the mask pointer with CE's eight-byte shadow. The engine's own
  `GetInt()`/`Set()` then consulted CE's storage instead of the mask, so the write reached nothing the renderer
  reads *and* removed the object's only route to the real mask. The proof is in the install log itself: the
  20260816_161158 Talos session reported the identical `object+0x58` qword `0x00007FF73B3A16F0` - an address inside
  the exe's writable data - for all four ShowFlag objects. A per-variable `{game, render}` shadow pair cannot be
  identical across four different variables; a shared mask pointer is exactly that.
  CE now classifies the object before writing (`hook/common/ue5_console_layout.h`, unit-tested in
  `tests/test_ue5_console_layout.cpp`) across three shapes: **reference pointer** (`FConsoleVariableRef<T>`,
  accepted only when the shadow pair actually mirrors the global the pointer addresses - the check the ShowFlag
  objects fail), **inline pair** (`FConsoleVariable<T>`, only at the proven `+0x50` value offset because zeroed
  padding satisfies it), and **bit reference**. Bit-ref writes set/clear CE's own bit with a compare-exchange on
  the containing word, so the other show flags sharing the byte are untouched; restore puts back only that bit.
  Two independent gates keep the bit path off everything else: it is offered only to `ShowFlag.` names
  (`ce::ue5_cvar::IsShowFlagSpec`), and a candidate is only accepted once a *second* show flag independently
  reports the same mask pair with a different bit index.
### `ShowFlag.*` force bits - state of knowledge (open, 2026-08-16)

This is the one unfinished part of the UE5 override work. Read this whole block before touching it; the failure
mode is a visibly broken frame in the user's game.

- **Why it matters:** UE ships **no vignette CVar at all**. Enumerating every UTF-16 literal in
  `Talos1-Win64-Shipping.exe` (5.4.4) turns up exactly three matches for "vignette": `SunColorVignetteIntensity`
  (an unrelated material parameter), `Vignette` (the show flag's short name) and `VignetteSF` (its localization
  key). There is no `r.Vignette*`, and no `r.DefaultFeature.*` entry for it either. Grain has only `r.FilmGrain`
  (+ `SequenceLength`, `CacheTextureConstants`), which covers the modern 5.1+ film grain pass but not the legacy
  tonemapper grain. So for **vignette, and for legacy-path grain, the show flag is the only per-effect lever.**
- **`r.Tonemapper.Quality` is rejected as the alternative.** UE's own help text (read out of the Talos binary)
  documents it as a cumulative ladder: "0: basic tonemapper only / 2: + Vignette / 4: + Grain / 5: + GrainJitter =
  full quality (default)", so anything below 2 does remove vignette - but it removes the rest of the ladder with
  it. User decision, 2026-08-16: **tonemapper quality must not be reduced to disable vignette or grain.** A test
  asserts the spec table never contains it (`PostProcessingBundleUsesDedicatedControlsAndShowFlags`); do not
  delete that assertion to make a change fit.
- **What a bit ref is:** `FConsoleVariableBitRef` at `object+0x50` = `Force0MaskPtr`, `+0x58` = `Force1MaskPtr`,
  `+0x60` = `BitNumber`. Both mask pointers are shared by every show flag in the process, which is what
  `CommitConfirmedBitReferences`/`ReportConfirmedBitReferences` use as proof of the shape (two flags must agree on
  the pair with different bit numbers).
- **The masks are read through the object's own pointer, never RIP-relative.** A scan of all 124 MB of `.text`
  finds only an inlined 48-byte copy between the two masks plus two destructor calls, and the file contains **zero**
  absolute pointers to them. So there is nothing to disassemble statically to recover the convention - an earlier
  conclusion that "the apply site is compiled out of Shipping" was wrong for this reason.
- **Measured bit maps** (`ProbeShowFlagBitNumbers` logs `show flag bit map — <name> bit=N` once per session, only
  when a bit reference was seen). Both mask geometry and indices are per-engine-version, which is why CE reads
  them per object and never assumes:

  | | Talos Reawakened 5.4.4 | Industria 2 Demo 5.6.1 |
  | --- | --- | --- |
  | mask spacing | 0x30 (48 B, 384 flags) | 0x40 (64 B, 512 flags) |
  | Bloom / Tonemapper / AntiAliasing / TemporalAA | 1 / 3 / 4 / 5 | 1 / 3 / 4 / 5 |
  | GlobalIllumination | 12 | 14 |
  | Vignette | 13 | 15 |
  | Grain | 14 | 16 |
  | DirectLighting | 28 | 32 |
  | MotionBlur | 37 | 41 |
  | SceneColorFringe | 46 | 51 |
  | Lighting | 109 | 108 |

  Invariant worth keeping: **`GlobalIllumination` sits exactly one bit below `Vignette` in both versions**, which
  is why an off-by-one was the first theory - it lands on GI.
- **What is proven:**
  - 0.1.6128 wrote four bits at once (Talos 13/14/37/46) and **all lighting disappeared**. Reverted in 0.1.6131.
  - 0.1.6134 writes **only `ShowFlag.Vignette`** (`DriveBitReference` in `main_ue5_layout.cpp`, gated on
    `kDrivenBitReference`), recorded before writing and handed back on teardown, with a compare-exchange on the
    containing word so neighbouring flags survive. Validated on **both** titles (`20260816_205827` Talos,
    `20260816_210946` Industria 2): no visual regression, and value CVars still verify 32/32 and 34/34.
  - Therefore **there is no uniform off-by-one**: bit 13 alone is harmless. The 0.1.6128 culprit is one of
    Grain 14, MotionBlur 37 or SceneColorFringe 46, and which one is unknown.
- **What is NOT proven:** that the write actually disables vignette. Neither test title visibly uses vignette or
  grain, so "no change" and "correct but invisible" are indistinguishable here. **This needs a title that visibly
  shows vignette or grain.**
- **Next steps when such a title appears**, cheapest first:
  1. Run it. If vignette disappears, the mapping is right - extend `kDrivenBitReference` to the other show flags
     one at a time, watching for the lighting regression to identify the bad bit.
  2. If vignette does not disappear, the masks are inert in Shipping for that build and the show flag route is
     dead for it; say so rather than reaching for the quality ladder.
  3. The version-agnostic fix, if per-title measurement proves unworkable: **derive the convention from the
     engine's own code at runtime.** CE holds the console object, so it holds the vtable; `GetInt` is the entry
     whose body reads `[this+0x50]` and `[this+0x60]`, and its shift/mask constants *are* the convention for that
     build. Decode those and write to match, failing closed when the shape is not recognised. That is the only
     approach that holds from UE 4.27 through 5.6 without per-title guesswork, since `FConsoleVariableBitRef` has
     been stable across that whole range.
- Practical exposure meanwhile: **vignette only**, plus legacy-path grain. Grain (modern), motion blur and
  chromatic aberration are carried by `r.FilmGrain`, `r.MotionBlurQuality` and `r.SceneColorFringeQuality`, all
  verified changing a value on both engine versions.

- An object matching no shape, or matching one shape at two offsets, is left untouched and its first 0x80 bytes are
  dumped (rate-limited to 6 per session). That is deliberate: guessing a layout is what produced the ShowFlag writes,
  and the dump is what lets the next run identify a per-title layout instead of re-deriving it from a refusal.
- Observed coverage: Industria 2 Demo (UE 5.6.1, session 20260815_214219) installs **38/40** requested CVars and
  verifies 38/38; Talos Reawakened (UE 5.4.4) installs 35/40. Note that the four `ShowFlag.*` counted as "installed"
  in every session before 0.1.6128 were the inert bit-ref writes described above. The remaining entries are the
  version-conditional/absent names plus the four show flag force bits. `r.Lumen.ScreenProbeGather.Temporal.MaxFramesAccumulated`
  was in this list until 0.1.6132: the layout dump showed its shadow at `object+0x58` reading `0x41C80000` = **25.0f**,
  i.e. the variable is a *float* while the spec table declared it `Int32`, so the Int32 plausibility check refused it
  every session. Correctly - installing it as an int would have written 10, a denormal float, into a float global and
  disabled temporal accumulation outright. The spec is now typed `Float`; the checks stay fail-closed in the other
  direction because an int-shaped 10 or 25 reinterprets as a denormal and is refused rather than written. A CVar that
  is "found but never validated" in one title and fine in another is a type mismatch until proven otherwise.
  Industria 2 also exercised drift recovery: two Lumen CVars were rewritten by the game ~30 s in and were
  re-asserted automatically.
- A configuration change that newly requests CVars re-opens the registry resolver (`ReopenConsoleRegistry`). The
  resolver otherwise stays closed for the rest of the session once it has finished with the previous request set,
  so enabling e.g. the post-processing bundle mid-session left its runtime-composed names permanently unresolved.
  Only the "stop looking" state is cleared: the located map, sweep coverage, and per-name layout refusals are facts
  about the title that a new request does not change.
- **Stale-risk / unverified:** the show flag force bits have **no end-to-end evidence** comparable to the RR
  bundle's `Feature 13 evaluation succeeded`, and neither did the redirect they replace. Note that
  chromatic aberration turned off only once the *Ref-redirect* path started mirroring, which is evidence that
  `r.SceneColorFringeQuality` (not `ShowFlag.SceneColorFringe`) is what actually gates the effect in UE 5.6. Talos in-game (session 20260815_191332) is the
  end-to-end proof for the RR bundle: `r.NGX.DLSS.DenoiserMode=1` installed as a Ref redirect and NGX then logged
  `Feature 13 evaluation succeeded; Ray Reconstruction is rendering`, with no Feature 1 SR fallback for the rest of
  the session. Ref redirect is therefore proven to reach the engine *through the wrapper* - which, as the mirroring
  defect above showed, says nothing about readers that cached the shadow pair instead. **No data-pointer-mode
  override has an equivalent end-to-end proof yet**, which is what the write-through and the read-back verification
  below exist for.
- **Read-back verification** (`VerifyOverrides` in `hook/main_ue5_install.cpp`, once a second from
  `RefreshOverrides`) closes the gap between "the write succeeded" and "the value is live". It re-reads the redirect
  slot, CE's shadow, and the write-through storage for every installed override. A slot no longer pointing at CE's
  shadow means the game re-registered the variable: the record is retired, the mirrored value handed back, and a
  rescan requested. A value that drifted means a game-side `Set()` reached storage CE owns: the configured value is
  re-asserted rather than reinstalled. Reporting is per-override capped at three and only lifts after 60 consecutive
  clean passes, so a constantly rewritten CVar stays quiet while a rare regression is still logged; the summary line
  is emitted only when the counts change. There is no bit-reference branch here: CE never writes a force bit, so
  there is nothing to verify or restore for one.
- New specs are **appended** to `kSpecs`, never inserted: `kDenoiserModeIndex` and `kTonemapperSharpenIndex` are
  positional, so an earlier entry silently retargets them. `PositionalSpecIndicesStillNameTheirCVars` pins both.
- Bumping `SHARED_MEMORY_VERSION` requires bumping the `SHARED_MEM_BASE_NAME` / `SHARED_MEM_DISCOVERY` literals in
  the same commit - they are hardcoded, not derived, and that lockstep is what stops a hook or Vulkan layer built
  against an older layout from opening the mapping (ABI 34). The event names *are* derived from the version.
  `SharedDefsTest.NameGeneratorsIncludeExpectedPidFormatting` pins all of them and is what catches a half-done bump.
- The same machinery overrides `t.MaxFPS`, `r.MaxAnisotropy`, and `r.VT.MaxAnisotropy` (shared-memory ABI 40 added
  `internalFpsLimit` and `internalAnisotropicFiltering`). `t.MaxFPS` is a `TAutoConsoleVariable<float>` in UE5, so
  the spec uses the float value type and the scan log prints it as a float; the two AF CVars are
  `TAutoConsoleVariable<int32>`. The literal scanner previously skipped first characters other than `r`/`s`
  (`FindRequestedLiterals` in `hook/main_ue5_scan.cpp`); `t` is now admitted for `t.MaxFPS`. The FPS limit and AF
  settings are independent of the RR/post-processing bundles and of each other.
- Third-party overlay coexistence at DX12 hook install: CE reads the Present vtable from a temp 2x2 swapchain,
  and creating it can enter an overlay that hooked the creation path first. Steam dispatches through callback
  slots that stay NULL until it has rendered on a real game swapchain, so entering its handler during startup
  recurses until the stack is gone (`0xC00000FD`, 750+ `gameoverlayrenderer64!OverlayHookD3D3` frames; 4 of 21
  sessions on 2026-08-15). `DX12Hook::Init` deferred the eager install for exactly this reason but
  `FindAndWrapPreExistingSwapchains` ran it on the next line anyway, so the guard was a no-op. Both the eager
  and the deferred decision now use the same startup window (`ShouldDeferEarlyDX12TempSwapchainPresentHookInstall`
  / `ShouldPostponeDeferredTempSwapchainPresentHookInstall`), keyed on the game's own D3D12 device, and
  `DX12Hook::ServicePendingPresentHooks` retries from the hook thread's service pass. Invariant, unit-tested:
  the postponement never outlasts the deferral condition, so a swapchain that pre-dates injection cannot be
  left without Present hooks.
- The UE5 override code is six units: `main_ue5.cpp` (policy/lifecycle), `main_ue5_scan.cpp` (literal and candidate
  discovery), `main_ue5_install.cpp` (install/refresh/verify/restore), `main_ue5_layout.cpp` (console-object layout
  probing and the three console-object install modes), `main_ue5_memory.cpp` (process-memory and PE primitives), and
  `main_ue5_registry.cpp` (console-registry resolution), with shared declarations in `main_ue5_internal.h`. The
  original split happened when `main_ue5_scan.cpp` had grown to 869 lines, past the 800-line ceiling, without being
  recorded in `tools/file_size_baseline.json` - lint would have failed on the next run.
- Post-processing removal uses `r.Tonemapper.Sharpen`, `r.FilmGrain`, `r.Tonemapper.GrainQuantization`,
  `r.MotionBlurQuality`, `r.SceneColorFringeQuality`, and matching `ShowFlag.*` CVars including vignette. It does not
  force `r.Tonemapper.Quality` down and does not disable `ShowFlag.PostProcessMaterial`: custom materials have no
  generic semantic label distinguishing sharpen from gameplay/damage/underwater/accessibility effects.
- **Film grain coverage is complete at the name level**; the gap was never a missing CVar. UE 5.1+ gates the whole
  film grain pass on `r.FilmGrain` (verified live in Talos: `1 -> 0`), `ShowFlag.Grain` gates it as a show flag, and
  `r.Tonemapper.GrainQuantization` is the UE4-era equivalent - kept because the layout machinery works on UE4 titles
  too, at the cost of one "literal not found" line on UE5. The remaining `r.FilmGrain.*` CVars
  (`SequenceLength`, `CacheTextureConstants`, `Texture`) shape grain rather than disable it, and per-volume
  `FilmGrainIntensity` is already gated by `r.FilmGrain`. What was actually broken was `ShowFlag.Grain` being an
  inert bit-ref write; adding further names would not have fixed it. Do not add a guessed CVar name here:
  `r.MegaLights.DownsampleMode` and `r.Tonemapper.GrainQuantization` each cost a permanent unresolved entry, and
  a name that exists in no engine version cannot be distinguished from one CE simply failed to reach.
- The policy deliberately does not spoof `SuperSamplingDenoising.Available`,
  `SuperSamplingDenoising.FeatureInitResult`, or `GetFeatureRequirements(Feature 13)`. It cannot add missing
  albedo/specular/normal/depth/motion-vector inputs, enable a disabled temporal upscaler, retrofit an older NVIDIA UE
  plugin, or make an unsupported GPU/runtime work. It also never blocks Feature 1 ordinary SR fallback: a failed RR
  create/evaluate must produce an ordinary frame rather than a blank output or engine assertion.
- CE adds authoritative lifecycle evidence around D3D11/D3D12/Vulkan NGX Create/Evaluate/Evaluate_C/Release. Feature
  13 is reported active only after a successful evaluation, not merely after handle creation; Feature 1 reports an SR
  fallback, and release republishes the remaining evaluated handles. Streamline's successful
  `slEvaluateFeature(kFeatureDLSS_RR=1001)` / `kFeatureDLSS=0` provides the same evidence when the public Streamline
  layer is visible. All transition/failure logs are bounded. Capability observations are diagnostic only.
- There is no CE hardware-ray-tracing gate. Software-RT UE projects can use the same policy when their NVIDIA plugin
  is RR-capable and their renderer supplies the required denoiser inputs. Conversely, merely shipping Streamline or
  copying `nvngx_dlssd.dll` cannot create engine-side RR integration that is absent.
- Expected proof sequence: `force policy enabled`, `persistent ... DenoiserMode=1 override installed`, Feature 13
  creation, then either NGX `Feature 13 evaluation succeeded` or Streamline `kFeatureDLSS_RR (1001) evaluation
  succeeded`. The install line reports `scanMs` plus validated/raw candidate counts. An ordinary Feature 1/feature 0
  evaluation while forced is an explicit fallback diagnostic; if it precedes the install line, RR discovery was too
  late and caused a live renderer transition. Talos runtime logs have confirmed forced Feature 13 evaluation and
  preset E without an on-disk CVar override; repeated level transitions and software-RT cases remain validation work.

## Graduated RR quality settings and typed custom values (2026-08-22)

- `ray_reconstruction_optimal_settings` is a nested `off|light|medium|full` preset. `light` writes
  `r.Lumen.Reflections.BilateralFilter=0`, `ScreenSpaceReconstruction=0`, `Temporal=0`, and `r.SSR.Temporal=0`;
  `medium` adds `r.Lumen.Reflections.DownsampleFactor=1`; `full` adds every remaining value from the former bundle.
  The old `on`/true spellings remain aliases for `full` so existing profiles retain their quality settings.
- The preset never selects `r.NGX.DLSS.DenoiserMode`. Its dedicated named control is
  `force_ray_reconstruction=on`, preserving the distinction between tuning renderer inputs and selecting the NVIDIA
  RR denoiser. An explicit custom DenoiserMode entry can still override it like any other supported CVar.
- `custom_cvar_overrides=name=value,...` resolves names only against `kSpecs`; it cannot make the scanner write an
  unknown CVar or guess a type. Canonical names are case-insensitive, and normalized aliases drop a leading `r.` or
  `t.` and replace dots with underscores (`tonemapper_sharpen` -> `r.Tonemapper.Sharpen`). Int32 values require
  integer syntax and Float values require a finite full-string number. Invalid entries are independently ignored,
  duplicates are last-wins, and custom values resolve after all presets/dedicated options.
- Parsing occurs at the host config boundary. ABI 45 transports only a 64-bit spec mask and 64 raw values whose
  types were already validated; the injected hook does not parse an untrusted expression. `kSpecs` is statically
  capped at that capacity. `SharedGraphicsConfig` grows from 420 to 688 bytes, and all mapping/event names move with
  `SHARED_MEMORY_VERSION` 44 -> 45.

## Depth of field, DLSS Super Resolution, and HDR (added 2026-08-21)

Three override families were added on top of the mechanism above. They change no machinery: each is a spec in
`ce::ue5_cvar::kSpecs` with its own `Activation`, resolved from `Settings` and installed by the same validated
redirect. Configuration lives in `[UE5]` and is parsed by `common/config_load_ue5.cpp` (split out of
`config_load_core.cpp` in the same change, so the whole `[UE5]` vocabulary is one unit).

| Setting | CVar(s) written | Type | Notes |
| --- | --- | --- | --- |
| `depth_of_field=off\|on` | `r.DepthOfFieldQuality` | Int32 | 0 is the engine's own "Off"; on writes 2, UE's registered default |
| `dlss_super_resolution=off` | `r.NGX.DLSS.Enable=0` | Int32 | the NVIDIA plugin's own documented switch |
| `dlss_super_resolution=on` | `r.NGX.DLSS.Enable=1`, `r.NGX.Enable=1`, `r.TemporalAA.Upscaler=1`, `r.AntiAliasingMethod=2` | Int32 | AAM_TemporalAA is the only path UE routes a third-party upscaler through |
| `dlss_super_resolution_quality` | `r.ScreenPercentage` | Float | only while SR is forced on; 25..100, quality-mode names map to the plugin's ratios |
| `hdr_output=off\|on` | `r.HDR.EnableHDROutput` | Int32 | engine help: "Creates an HDR compatible swap-chain and enables HDR display output" |
| `hdr_peak_luminance` | `r.HDR.Display.MaxLuminance` | Int32 | nits, 80..10000 |
| `hdr_paper_white` | `r.HDR.Display.MidLuminance` | Float | nits for 18% gray, 20..1000 |
| `hdr_ui_luminance` | `r.HDR.UI.Luminance` | Float | nits for UI elements, 20..1000 |
| `hdr_min_luminance` | `r.HDR.Display.MinLuminanceLog10` | Float | configured in nits (0.0001..10), written as log10 |
| `hdr_color_gamut` | `r.HDR.Display.ColorGamut` | Int32 | 0 Rec709, 1 DCI-P3, 2 Rec2020, 3 ACES, 4 ACEScg |

- **The names and types are measured, not assumed.** `tools`-free scratch scripts enumerated the NUL-terminated
  UTF-16 literals of a shipped UE5 binary (Gothic 1 Remake, `G1R-Win64-Shipping.exe`), then disassembled every
  RIP-relative `lea` that loads each name to find the registration call. UE registers int32 CVars through the
  console-manager vtable slot `+0x18` with the default in `r8d`, and float CVars through `+0x10` with the default in
  `xmm2` - the same discriminator the `r.MipMapLODBias` entry was fixed with in 2026-08. That is where
  `r.HDR.Display.MaxLuminance` being an **int** while `MidLuminance`/`MinLuminanceLog10` are **floats** comes from;
  guessing symmetry there would have written denormal garbage into an engine global. The help text sitting next to
  each literal supplied the semantics quoted above (`0: Off` for DoF quality, "nit level for 18% gray" for
  MidLuminance, "Enable/Disable DLSS SR or RR at runtime" for `r.NGX.DLSS.Enable`).
- **Version-conditional:** Black Myth Wukong (UE 5.0) has `MaxLuminance`, `MidLuminance`, `MinLuminanceLog10`,
  `ColorGamut`, `EnableHDROutput` and `r.HDR.UI.Level`, but **no `r.HDR.UI.Luminance`** - that one is newer. A CVar
  whose literal is absent is reported in the missing summary and skipped, which is the existing behaviour for
  `r.MegaLights.*` and friends, so `hdr_ui_luminance` simply does nothing on such a build. `r.HDR.UI.Level` was
  deliberately not added as a fallback: it is a multiplier, not nits, and the equivalence to a nit level is
  unmeasured.
- **Forcing DLSS on is more than the plugin's flag.** `r.NGX.DLSS.Enable` alone reaches nothing in a game that
  renders with TSR: UE only selects a third-party temporal upscaler when the AA method is `AAM_TemporalAA` (2) and
  `r.TemporalAA.Upscaler` is 1 (its own help text: "GTemporalUpscaler which may be overridden by a third party
  plugin"). The measured `r.AntiAliasingMethod` default in the Gothic build is 4 (TSR). So the on direction moves
  the game off TSR for as long as the override is installed; that is a visible rendering change and is documented as
  such in `config.ini`. The off direction touches nothing but `r.NGX.DLSS.Enable`, because disabling NGX entirely
  would also take DLSS-G down with it.
- **Quality is expressed as screen percentage** because that is what the plugin resolves into its quality modes.
  `r.ScreenPercentage` is only ever written while SR is forced on (`DlssSuperResolutionScreenPercentage` checks
  both), so the override can never silently change a game's render resolution on its own.
- **HDR output wins over the sRGB half of `display_gamma`.** `display_gamma=srgb` writes
  `r.HDR.Display.OutputDevice=0`, which is an SDR device; with `hdr_output=on` in the same configuration that write
  is suppressed in `Resolve` rather than fighting the HDR request. The pre-existing `SdrOutputDeviceOnly` guard is
  unchanged and still refuses the write when the *game* is already on an HDR device.
- **Depth of field deliberately does not use `ShowFlag.DepthOfField`.** CE classifies bit-ref console objects but
  does not write force bits (see the ShowFlag section above); a spec for it would resolve and then sit inert.
  `r.DepthOfFieldQuality=0` disables every DOF implementation by the engine's own help text, so the show flag adds
  nothing today. If the force-bit mapping is ever measured, that is the moment to reconsider.
- **ABI:** the nine new fields are appended to `SharedGraphicsConfig` (`sizeof` 384 -> 420) and
  `SHARED_MEMORY_VERSION` moved 43 -> 44, which also renames the shared mappings so an older hook or Vulkan layer
  can never open the new layout. `kUE5DlssScreenPercentageMin/Max` live next to the mip-bias helpers in
  `common/shared_defs_detail/abi_constants_and_config.h` and are pinned against the hook-side policy constants by
  `ConfigTest.UE5ConfigBoundsAgreeWithTheHookSidePolicy`.

### Open questions / stale-risk for the new overrides

- **No hardware run yet.** Everything above is proven from binaries, unit tests, and the engine's own registered
  help text; nothing has been observed in a running game. First run should check the install summary for
  `r.DepthOfFieldQuality`, `r.NGX.DLSS.Enable`, `r.AntiAliasingMethod` and the `r.HDR.*` names, then the
  `verified N/N` line.
- **When does the engine re-read `r.HDR.EnableHDROutput`?** UE reads it where it creates or reconfigures the swap
  chain, and its console-variable sink fires on `Set()` - which a redirect deliberately does not call. Installing
  before engine init (the normal launch path) is therefore the case that should work; toggling it mid-session may
  need a display/resolution change to be picked up. Unmeasured.
- **Forcing HDR on may leave the output device where the game left it.** UE's own HDR configuration path writes
  `r.HDR.Display.OutputDevice`/`ColorGamut` when it enables HDR; whether it does so for a game that never intended
  HDR is untested. `hdr_color_gamut` exists partly to have a lever if it does not.
- **DLAA vs upscaling:** with `dlss_super_resolution=on` and no quality mode, a game sitting at 100% screen
  percentage gets DLAA rather than a performance win. That is intentional (CE does not change render resolution
  unasked), but it is the most likely "it did nothing" report.
