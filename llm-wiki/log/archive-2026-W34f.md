# llm-wiki Log Archive - 2026-08-21

Entries are newest-first. Rotated out of `recent.md`.

### 2026-08-21 - The 1.x -> 2.x call translation, and real SDK headers for the hook DLL

> The durable reference for all of this - the measured 1.x ABI, the activation rules and the
> recorder workflow - is now `frame-generation/streamline-generation-bridge.md`. The entries below
> are the chronology of how it was arrived at, including the two inferences that turned out wrong.

With the measured layouts in hand the generation bridge stops refusing and starts translating.
`hook/apis/streamline_bridge_translate.{h,cpp}` turns each of the eight 1.x entry points into its
2.x equivalent: `slInit` reports success because CE already brought the runtime up,
`slIsFeatureSupported` maps the feature and answers 1.x's adapter bitmask, `slSetTag` buffers tags
until `slEvaluateFeature` supplies the command buffer 1.x never carries, `slSetConstants` copies
`Constants` field by field, `slSetFeatureConstants` becomes `slDLSSSetOptions` /
`slDLSSGSetOptions`, and `slGetFeatureSettings` becomes `slDLSSGetOptimalSettings` written back into
the game's own struct. Frame tokens are cached per index so constants, tags and evaluate all see the
same one.

**The hook DLL now compiles against the real Streamline SDK headers** (`build_project.py` adds
`FG_SDK_INCLUDE_DIR/streamline/include` to `hk_cflags`). This is the important part: the translation
constructs `sl::Constants`, `sl::ResourceTag`, `sl::DLSSOptions` and friends, and hand-mirroring
eight more structs would have repeated the mistake that `sl::Preferences` already demonstrated - it
was wrong on its first write. Only the 1.x side is still mirrored, because no 1.5.6 header exists,
and each of those mirrors is either corroborated by two independent header sources (`Constants`,
`Resource`) or measured from a real session (`DLSSConstants`, `DLSSSettings`, `DLSSGConstants`).
CE's own `sl*`-prefixed types live in the global namespace and do not collide with the SDK's `sl::`
ones - verified by compiling both together, which also static-asserts `sl::kFeatureDLSS_G == 1000`.

**The 32-bit build caught the mirrors being pointer-size dependent**, which is what layout assertions
are for. `V1Constants` is 456 bytes and `V1Resource::native` sits at offset 8 only on x64; on x86 the
trailing `void* ext` and the pointer fields shrink and every assertion failed. Streamline has no
32-bit runtime at all - the build already skips the FG SDK runtimes for x86 - so the translation is
now compiled only for x64, with 32-bit fallbacks that refuse. Keeping the assertions rather than
relaxing them is what turned a silent mis-layout into a build error.

Deliberately still refused: Reflex constants (1.x drives Reflex through `slSetFeatureConstants`,
2.x through `slReflexSetOptions`, and that layout has not been verified) and any feature or buffer
type outside the measured tables. A refusal costs one feature; a guess would corrupt frame
generation silently.

Not yet validated in a game. `dlss_fg_factor` / `dlss_fg_preset` should apply for free, because the
bridge calls `slDLSSGSetOptions` on the 2.x runtime and CE's existing hook on that export is what
applies those overrides.

### 2026-08-21 - The first bridged measurement: DLSS_G is 1000 in Streamline 1.5.6, not 5

The passive recorder paid for itself on its first run. The Witcher 3 session
`20260821_041255` (build 0.1.6200, unbridged, all other overrides on) produced seven
`Streamline 1.x probe:` records, and one of them overturns an inference this workstream had
already written into the translation table.

**`eFeatureDLSS_G` is 1000 in 1.5.6, the same value 2.x uses.** The earlier value, 5, came
from reading sl.interposer 1.5.6's feature-name table positionally - DLSS, NRD, NIS, Reflex,
Debug, DLSS_G, Common - but that table is in declaration order, not value order. The game
settles it: it calls `slSetFeatureConstants` with feature **1000**, immediately after its
Reflex constants, which is exactly how a title brings DLSS-G up. Shipped as inferred, the
bridge would have translated a value the game never sends while refusing the one it does,
and frame generation would never have been configured at all. An ordered string table is
evidence of membership, never of value.

The feature map is therefore near-identity: DLSS 0, NIS 2, Reflex 3, DLSS_G 1000, Common
UINT_MAX all carry over unchanged. Only three values still refuse - 1.x Debug 4 (2.x
`kFeaturePCL`), 5 (nothing in 1.5.6, `kFeatureDeepDVC` in 2.x) and NRD 1 (removed).

**Layouts recovered from the same run**, all previously unpublished:
- `sl::DLSSConstants` - `mode` @0 (1 and 4 both seen), `outputWidth` @4 (3840), `outputHeight`
  @8 (2160), `sharpness` @12 (0.0), `preExposure` @16 (1.0), `exposureScale` @20 (1.0),
  `colorBuffersHDR` @24 (1). Field-for-field the public v1.1.1 layout, so that header is
  usable for the DLSS side after all.
- `sl::DLSSSettings` (OUT) - `optimalRenderWidth` @0 (1920), `optimalRenderHeight` @4 (1080),
  `optimalSharpness` @8 (0.35), then three zeroed `uint32`s. It sits exactly 24 bytes below
  the IN struct in the caller's frame, which pins its size at 24.
- `sl::ReflexConstants` - `mode` @0 (1 = low latency), `frameLimitUs` @12 (565).
- `sl::DLSSGConstants` - `mode` @0, `1` @4, captured with mode reading 0.

**The DLSS-G capture reading "off" was a defect in the recorder, not in the session.** That run
had frame generation genuinely active - `Hooked_CreateFeature: DLSS FG ACTIVATED (ID 0xB, 4x
multiplier)` at `04:13:35`, 1832 frames of perf metrics, SR preset M applied. The probe recorded
only the FIRST sighting per (call, feature), which landed at `04:13:15` during setup, twenty
seconds before FG came up; every later call carrying the enabled mode was discarded by CE's own
throttle. The captured struct therefore described "off" for a session that ran 4x frame generation
throughout.

Fixed by recording on **content change** rather than first sighting: a layout is static but its
values are the evidence, a field that differs between captures is by definition a live field, and
the off->on transition is what identifies the mode field at all. A per-slot cap keeps a genuinely
per-frame field (frame index, timestamp) from flooding, and the heartbeat still covers slot
collisions. Worth remembering as a general rule for this kind of probe: throttling by *sighting*
discards exactly the state transitions the probe exists to observe; throttle by *value* instead.

Also confirmed live in that session: the per-module generation gate (`sl.interposer.dll
speaks Streamline 1.x`), and `dlss_fg_dll_path` redirecting `nvngx_dlssg.dll` to the user's
own folder while `streamline_upgrade` was off.
