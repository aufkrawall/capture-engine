# llm-wiki Log Archive - 2026-08-16

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
