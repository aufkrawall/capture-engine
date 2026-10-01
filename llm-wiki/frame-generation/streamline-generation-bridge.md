# Streamline generation bridge (`streamline_upgrade`)

Running a Streamline **2.x** runtime inside a game that shipped **1.x**, so DLSS-G /
multi-frame generation becomes reachable in titles otherwise stuck on SL1. The Witcher 3
next-gen (`sl.interposer` 1.5.6) is the reference title.

This page carries the **measured 1.x ABI**, which exists in no public source and cannot be
re-derived from documentation. Treat it as the primary reason this page exists.

## Source anchors

| Concern | Where |
| --- | --- |
| Policy (activation, feature/buffer maps, preference flags) | `hook/apis/streamline_bridge_policy.h` (unit-tested) |
| Runtime (import takeover, fallback, 1.x quiesce) | `hook/apis/streamline_bridge.{h,cpp}` |
| Core-export grouped hook publication | `hook/apis/streamline_inline_hook_batch.{h,cpp}` |
| 2.x bring-up (load by full path, `slInit`, inventory) | `hook/apis/streamline_bridge_runtime.{h,cpp}` |
| Native D3D12 device continuity | `hook/apis/streamline_bridge_device_cache.{h,cpp}` |
| Native D3D12 device creation (adapter normalization, probe answers, retry) | `hook/apis/streamline_bridge_native_device.{h,cpp}` |
| 1.x -> 2.x call translation | `hook/apis/streamline_bridge_translate.{h,cpp}` (x64 only) |
| Reflex: options, settings, markers, sleep | `hook/apis/streamline_bridge_reflex.{h,cpp}` (x64 only), `streamline_bridge_diag.h` |
| DLSS-G options, the `notRenderingGameFrames` gate, persistent present-time tags | `hook/apis/streamline_bridge_dlssg.{h,cpp}` (x64 only), policy `streamline_bridge_dlssg_gate.h` (unit-tested) |
| Present-marker guard (re-marks a title present without PRESENT_START) | `hook/apis/streamline_bridge_present.{h,cpp}` (x64 only), policy `PresentMarkerLedger` in `streamline_bridge_dlssg_gate.h` |
| The measured 1.x structures | `hook/apis/streamline_bridge_v1_abi.h` (x64 only) |
| Passive layout recorder | `hook/apis/streamline_v1_feature_probe.{h,cpp}` |
| Generation classification | `hook/common/streamline_api_generation.h` |
| Tests | `tests/test_streamline_bridge_policy.cpp`, `tests/test_streamline_bridge_debug_layer.cpp`, `tests/test_streamline_bridge_v1_reflex.cpp` |
| Config | `streamline_upgrade` (default off), alongside `streamline_dll_path` |

## What it is, and what it deliberately is not

It is **not** a DLL substitution. `streamline_dll_path` rewrites the paths of loads the game
and Streamline perform, and `StreamlineOverrideGenerationMatches` correctly refuses to let
that cross generations - a 1.x game imports five exports a 2.x interposer does not have, so
the loader would kill the process before its first frame.

The bridge instead adds a second, CE-owned 2.x runtime loaded by full path and repoints the
game's `sl.interposer` import slots at CE thunks in memory. Nothing on disk is renamed or
patched, and the takeover disappears with the process. That is also why the duplicate-instance
guard in `graphics_runtime_module_policy.h` needed **no** exemption: the bridge never asks for
a redirect. The inverse is required instead - while the bridge is active the ordinary `sl.*`
substitution stands down, because both mechanisms want the same configured folder for
opposite purposes.

The game's own `sl.interposer.dll` stays mapped - it is a static import of the executable and
nothing can prevent that - but after the takeover none of its exports is ever called again,
and on a late start its plugins are unloaded again by the quiesce below.

## Why "too late" is not where it looked

The first shipped version refused as soon as `sl.common.dll` was resident, on the reasoning
that 1.x loads its core from inside `slInit`, so that module proves the game already drove
its own runtime. The reasoning is right; the conclusion made the feature unreachable. Both
`streamline_upgrade=true` sessions refused, with the same line:

```
Streamline bridge: not activating - the game already drove its own Streamline runtime
```

**CE cannot win that deadline, and the reason is structural rather than a matter of
milliseconds.** `123.exe` (The Witcher 3, renamed - see the NVIDIA exe-name refusal) imports
its D3D12 and DXGI entry points **from `sl.interposer.dll`**, not from Microsoft's DLLs, and
`sl.interposer.dll` imports neither. The only module that pulls `d3d12.dll` into the process
is `sl.common.dll`, through its own import table - and that loads from inside `slInit`. CE's
delayed-injection gate waits for `d3d12.dll`. So CE's arrival signal and the deadline it was
being held to are *the same event*, with the then-current intrinsic WMI notification
(`WITHIN 0.5`), a 114 ms config reload and a ~380 ms remote-thread `LoadLibrary` stacked in
between. Session
`20260821_151924` shows the losing end: `d3d12=1` on the very first poll, i.e. `slInit` had
already run before CE was even notified the process existed.

**Update (0.1.6654):** the unelevated notification path is no longer that WMI query. It is
`ce::process_start::Poller`, a native `NtQuerySystemInformation` sweep every 250 ms, so the
notification component of that stack halves. This page is the counter-example worth remembering
when reading the claim in `dx12-injection-bootstrap.md` that detection latency has margin to
spare: it does for a title that takes seconds to reach its first swapchain (Alan Wake 2,
`20260918_162809`), and it does **not** for a 1.x Streamline title that reaches `slInit` inside
the notification window. Neither figure generalises to the other case.

Two things follow, and both were fixed:

1. **CE's own startup latency was the part it owned.** In session `20260821_151738` CE
   *was* in the process in time - DllMain at 15:18:11.99, its own LoadLibrary hooks live by
   15:18:12.27 - and then spent until 15:18:12.77 on other work before evaluating the bridge,
   roughly 400 ms of it in `FatalExitDump` quiescing peer threads to install inline hooks.
   `TryActivate()` now runs as the **first** thing the hook thread does with a loaded config,
   and the takeover itself was reordered to happen *before* the 2.x runtime is loaded, so the
   expensive half no longer sits inside the window it is racing.
2. **The deadline itself was wrong.** `slInit` is recoverable; device creation is not. The
   margin is not marginal: in the same session the 1.x core was resident by 15:18:12.3, its
   feature plugins did not load until 15:18:13.4-13.9, and the game's real swapchain was not
   created until **15:18:29.1** - sixteen seconds later.

Current process discovery first requests event-driven `Win32_ProcessStartTrace`, but treats it as
an optimization: access denial falls back to the intrinsic half-second query plus a catch-up scan.
The target-config callback is installed before either subscription or scan begins. Streamline's
correctness still rests on recoverable late takeover rather than assuming the event trace is
available. Core exports that arrive together in one Streamline module are now prepared together and
committed with one thread-quiescence transaction; unsafe individual targets retain the established
independent fallback.

So a late arrival now takes the imports over and shuts the game's 1.x runtime back down
through the `slShutdown` slot it saved while repointing it, reaching the same end state from
a later start. Only `DeclinedGameOwnsItsDevice` still refuses.

## Run history

The per-run evidence (eleven bridged hardware runs so far: device handoff, plugin pinning, the debug
layer, Reflex markers, `notRenderingGameFrames`, tag lifetime, unmarked presents) is in
[streamline-generation-bridge-runs.md](streamline-generation-bridge-runs.md).

## Invariants

- **Activation is all-or-nothing, decided once, before anything is touched.** It requires
  the opt-in, a configured path, a real V1-process/V2-runtime pairing, and every 2.x entry
  point the translation needs. A half-bridged process - some calls translated, a device
  created through one generation and driven through the other - is worse than either end
  state.
- **The takeover happens before the bring-up, and a call that arrives between them waits.**
  Repointing import slots is memory writes; loading the 2.x interposer and running its
  `slInit` costs hundreds of milliseconds. Doing the expensive half first means racing the
  game for it. `std::call_once` makes the first caller perform the bring-up and everyone else
  block, so no thread ever sees a half-built runtime - synchronisation, not a timing guess.
- **A failed bring-up forwards every call to the 1.x export its slot used to hold.** The
  slots are already CE's by then, so "no bridge" has to mean "exactly what would have
  happened unbridged", not a hole where Streamline was. The originals are read from the
  interposer's export table *before* the first slot is patched, so a call arriving mid-
  takeover always finds one.
- **The 1.x quiesce runs only on a thread the GAME is on.** Reaching a CE thunk proves the
  game has returned from whatever 1.x call it was in, so nothing is inside that runtime while
  it is torn down. From CE's hook thread it would be a genuine race against an `slInit` that
  may still be running, and no ordering on CE's side could rule that out. It also has to
  precede the 2.x bring-up: both plugin sets carry the same base names, and CE's Streamline
  hooks are keyed on those names.
- **The quiesce includes the old runtime's dynamically loaded NGX feature images.** Preload and
  early legacy-module IAT patching try to make both generations choose the configured SR/FG
  images. If late injection still loses, only images captured while 1.x was live, differing from
  the configured runtime copy, are released after successful `slShutdown` and before 2.x init.
- **Quiescence is a global prerequisite, not a thunk-ordering convention.** Device discovery and
  every future V2 bring-up route must cross the same state machine; `EnsureRuntimeReady` itself
  rejects Pending, Running and Failed. Complete means legacy shutdown and NGX retirement succeeded,
  both configured replacements are pinned, and no foreign SR/FG image is resident. It is published
  before any code may load the V2 interposer.
- **The generation that authorises an ABI-sensitive hook is the MODULE's, never the
  process's.** The bridge makes two generations resident on purpose, so a process-wide latch
  would authorise 2.x-shaped hooks on the still-resident 1.x interposer - the truncation
  that killed The Witcher 3 (`20260820_221409`). Only the one GetProcAddress route, keyed on
  symbol name alone, takes a process-wide answer.
- **No Streamline version is pinned anywhere.** The staged runtime is expected to be
  restocked with newer DLLs, so generation comes from the interposer's `VS_FIXEDFILEINFO`
  major and `sl::kSDKVersion` is reconstructed from its full version. A "known good
  versions" range was written and removed: it can only go stale.
- **Anything unverified is refused, never approximated.** A refusal costs one feature; a
  guess corrupts frame generation silently, and no test in this repo can catch that.
- **No feature context, no call.** Most 2.x exports jump through a plugin pointer the manager
  binds late, so every device-dependent translation is refused until Streamline says the
  context exists. This is a crash, not a courtesy - it happened twice.
- **A 1.x non-game frame never reaches 2.x DLSS-G with generation on.** 1.x skipped interpolation
  unless `notRenderingGameFrames == eFalse`; the bridge forwards that as `eOff` with resources retained.
- **A 1.x present-time tag stays valid until the title replaces it.** 2.x expires legacy tags after one
  extra present; the bridge re-issues DLSS-G inputs after every present to keep the 1.x lifetime.
- **Every present 2.x counts carries a PRESENT_START.** 2.x DLSS-G skips a present whose Reflex frame
  lags the present count; when the title presents without its own marker, the bridge re-marks its last
  frame from inside 2.x `sl.common`'s present hook (reusing that frame's token).
- **1.x Reflex markers and sleep are evaluates with a null command buffer.** `id` is the marker;
  never drop a Reflex evaluate for its command buffer.
- **Readiness is asked, never inferred.** The only signal is `slGetFeatureFunction` succeeding.
  Both inferences that were tried - "slSetD3DDevice returned eOk" and "the interposer created
  the device" - produced the same null call.
- **The first device a title creates may be a throwaway.** Hand over every distinct one.
  Explicitly selected native/interposer devices supersede each other; queue-derived discovery is
  only a fallback and never overwrites an explicit handoff.
- **A proven device answers a compatible redundant recreation before the driver is called.** A
  device-lost-class call can reset the already-retained object, making after-failure recovery
  impossible (`20260822_174509`). Reuse requires the same physical-adapter LUID, equal-or-lower
  minimum feature level, a healthy device, and the requested COM interface. Otherwise ordinary
  native creation remains authoritative.
- **A device the bridge keeps alive must not see process-global D3D12 reconfiguration.** The
  title believes its probe device is gone. A debug-layer request reaching D3D12 now would reset the
  retained device and poison the adapter (`20261001_034038`), so it is refused before forwarding.
- **Device identity is COM identity, not an interface pointer.** Different D3D12 interfaces on the
  same object may have different addresses. Canonicalize through `IUnknown`, retain that identity,
  and call `slSetD3DDevice` only for a genuinely distinct successfully accepted device.
- **The probe is event-driven, not polled and not once-only.** Once per epoch, where an epoch is
  a device handed over or the game's frame index moving; nothing at all once the answer is yes.
- **A module the bridge routed around gets no hooks.** With two generations resident and one
  forward pointer per symbol, hooking the inert one costs the live one its hook.
- **What CE cannot check in advance, it reports after the fact.** Whether the game created
  its device before injection is unknowable from inside the process - CE never saw it. So the
  bridge counts device/factory creations arriving through its own pass-through slots, and the
  first feature call that depends on one says so plainly if none ever did. An unanswerable
  precondition becomes a fact in the log instead of silently absent frame generation.
- **The hook DLL compiles against the real SDK headers** (`build_project.py` adds
  `FG_SDK_INCLUDE_DIR/streamline/include` to `hk_cflags`), so only the 1.x side is
  hand-mirrored. CE's own `sl*`-prefixed types are global and do not collide with `sl::`.

## The measured 1.x ABI

There is **no public Streamline 1.5.6 header**: NVIDIA published no 1.x release at all and
the upstream 1.x tags stop at **v1.1.1**, which predates DLSS-G. Sources, in decreasing
authority for this title:

1. **The game's own 1.5.6 binaries** - authoritative for anything version-specific.
2. **A real captured session** - authoritative for the per-feature structs.
3. **OptiScaler's vendored `external/streamline1/`** - a genuine SL1 header set, good for
   the stable core (`Constants`, `Resource`, function signatures) but it **predates DLSS-G**,
   so never trust it for the feature enum.

### Function signatures (1.x, all return `bool` in AL)

```
slInit               (const Preferences&, int applicationId)
slShutdown           ()
slIsFeatureSupported (Feature, uint32_t* adapterBitMask)
slSetTag             (const Resource*, BufferType, uint32_t id, const Extent*)
slSetConstants       (const Constants&, uint32_t frameIndex, uint32_t id)
slSetFeatureConstants(Feature, const void* consts, uint32_t frameIndex, uint32_t id)
slGetFeatureSettings (Feature, const void* consts, void* settings)
slEvaluateFeature    (CommandBuffer*, Feature, uint32_t frameIndex, uint32_t id)
```

Returning `bool` matters: a zeroed 32-bit result where the caller reads one byte is a silent
behaviour change.

### Feature values - near-identity, and an inference that was wrong

| Feature | 1.5.6 | 2.x | Translation |
| --- | ---: | ---: | --- |
| DLSS | 0 | 0 | identity |
| NRD | 1 | `kFeatureNRD_INVALID` | refuse (removed) |
| NIS | 2 | 2 | identity |
| Reflex | 3 | 3 | identity |
| Debug | 4 | `kFeaturePCL` = 4 | **refuse** (collision) |
| - | - | `kFeatureDeepDVC` = 5 | **refuse** (5 is nothing in 1.5.6) |
| DLSS_G | **1000** | 1000 | identity |
| Common | UINT_MAX | UINT_MAX | identity |

**`DLSS_G` is 1000, not 5.** `sl.interposer` 1.5.6's feature-name table lists DLSS, NRD, NIS,
Reflex, Debug, DLSS_G, Common in that order, and reading position as value put DLSS-G at 5.
The table is in *declaration* order. Session `20260821_041255` settles it: the game calls
`slSetFeatureConstants` with feature **1000**, immediately after its Reflex constants. Shipped
as inferred, the bridge would have translated a value the game never sends while refusing the
one it does. **An ordered string table is evidence of membership, never of value.**

### `BufferType` - full identity

sl.common 1.5.6's name table holds exactly 38 entries (0..37, `Depth` ..
`TransparencyAndCompositionMaskHint`) and every one lands on the same value in 2.x, which only
appends beyond 37. `UIColorAndAlpha` is 23 in both (1.x spelled it `UIHint` in older releases -
same slot). So a range check, not a mapping table.

### Structure layouts (x64)

`Constants` - **432** bytes, **no** BaseStructure header. Every 1.x `Boolean` is
`enum Boolean : char`: one byte. The game's own 1.5.6 `sl.common.dll` validates the eight of them as
`cmp byte ptr [rbx+0x19c..0x1a3], 2`, so `depthInverted`@0x19c .. `motionVectorsJittered`@0x1a3 and
`ext`@424. Until 0.1.6872 the mirror read them as dwords (456 bytes). That made
`cameraMotionIncluded` come from `notRenderingGameFrames` (eFalse), so SL2 added camera motion
again. The result was DLSS sharp at rest and aliased in motion, and DLSS-G interpolating the wrong
motion (`20261001_040020`). The last three flags were also read past the struct. Translation to 2.x:
prepend the 2.x `BaseStructure`, copy `cameraViewToClip`..`reset` verbatim, route
`notRenderingGameFrames` (no 2.x field) to the DLSS-G gate (ninth run, see the run history), keep the three motion-vector/projection Booleans, leave
2.x `minRelativeLinearDepthObjectSeparation` at its **40.0f** default rather than zero, drop `ext`.
**Byte-level width claims about a 1.x struct need the binary's own instruction, not a header copy.**

`Resource` - `{ ResourceType type (1 byte); void* native@8; void* memory@16; void* view@24;
uint32_t state@32; void* ext@40 }`, 48 bytes. Independently confirms the offsets
`streamline_api_generation.h` already encodes. Note `type` is a **1-byte** enum, so CE's
existing 4-byte read at offset 0 works only because the padding is zero - it is fail-closed,
so a garbage read yields "no record" rather than a bad barrier.

Measured from The Witcher 3 session `20260821_042540` (4x FG active, 4968 frames):

| Struct | Layout | Observed |
| --- | --- | --- |
| `DLSSConstants` | `mode`@0, `outputWidth`@4, `outputHeight`@8, `sharpness`@12, `preExposure`@16, `exposureScale`@20, `colorBuffersHDR`@24 | 1 and 4; 3840; 2160; 0.0; 1.0; 1.0; 1 |
| `DLSSSettings` (out) | `optimalRenderWidth`@0, `optimalRenderHeight`@4, `optimalSharpness`@8 | 1920; 1080; 0.35, then zeroes |
| `ReflexConstants` | `mode`@0 **only**; the struct is 8 bytes | 1 (and +4 always 0) |
| `ReflexSettings` (out) | `lowLatencyAvailable`@0, `latencyReportAvailable`@1, `statsWindowMessage`@4, 64 reports, `flashIndicatorDriverControlled`@0x1e08 | the 1.5.6 writer's own stores |
| `DLSSGConstants` | `mode`@0 (**0 = off, 1 = on**), `numFramesToGenerate`@4 (unconfirmed) | 0 -> 1, 68 ms before `DLSS FG ACTIVATED`; +4 constantly 1 |

**`ReflexConstants` had a phantom field, and it is worth knowing how.** An earlier reading of
this table recorded `frameLimitUs`@12 with a value of 565. Re-reading the same capture shows
the struct is 8 bytes: every record has `mode`@0 = 1 and +4 = 0, and from +8 the captures
disagree, with the disagreeing bytes reading `00 46 00 00 f6 7f 00 00` - a `0x00007ff6....`
module address straddling +8 and +12. That is a caller's saved pointer on the stack, not data.
The probe's own documentation warns about exactly this ("a long tail of unstable values usually
means the 96-byte dump ran past the end of the struct"), and the warning was not heeded the
first time. **A field that is only ever non-zero in captures where it disagrees with itself is
not a field.** Only `mode` is translated; 2.x's `frameLimitUs`, `useMarkersToOptimize`,
`virtualKey` and `idThread` keep their defaults.

`DLSSConstants`' leading run is the same as 2.x `DLSSOptions`, which is why that translation
is nearly a field copy. `DLSSSettings` sits exactly 24 bytes below the IN struct in the
caller's frame, pinning its size; only its three measured fields are written back, because
that is all the real 1.5.6 runtime filled.

## How to measure more

`streamline_v1_feature_probe.cpp` hooks both opaque-payload calls, records, and **forwards
unchanged**. Two rules learned the hard way:

- **Run UNBRIDGED.** With `streamline_upgrade=on` the bridge (at the time) refused `slInit`,
  the game concluded Streamline was unavailable, and it never reached `slSetFeatureConstants`.
  The productive session is an ordinary one with DLSS and FG genuinely working.
- **Throttle by VALUE, not by sighting.** Keeping one record per (call, feature) looked
  obviously right and destroyed the first measurement: the record landed during setup with
  `DLSSGConstants.mode` reading 0, FG activated twenty seconds later, and every call carrying
  the enabled mode was discarded by CE's own throttle. A layout is static but its *values* are
  the evidence - a field that differs between captures is by definition a live field, and an
  off->on transition is the only thing that identifies a mode field.

To read a capture: `python tools/analyze_sl1_probe.py <hook_debug.log>`. It parses the
`Streamline 1.x probe:` records and prints every 4-byte slot under uint32/int32/float
interpretations, marking the slots that **changed between captures** - those are the live
fields. A long tail of unstable values usually means the 96-byte dump ran past the end of the
struct into stack leftovers, which is how the structs' sizes were bounded.

## Diagnostics / failure modes

- `Streamline bridge ACTIVE: N of M ... import slots now reach CE` - the takeover succeeded.
  The tail says whether the game's 1.x runtime had already initialised.
- `Streamline bridge inventory (<when>): sl.X.dll a.b.c resident from <path>` - one line per
  resident Streamline module, at the takeover (or the refusal) and again after the quiesce.
  This is the line that distinguishes a game one call into `slInit` from one that has already
  bound every feature plugin; without it a refusal named a decision but not the state.
- `Streamline bridge: shut the game's own 1.x Streamline runtime down` - the late-start path.
  The inventory line that follows is the proof of what is left resident.
- `Streamline bridge: the CE-owned 2.x runtime reports a bound device` - everything is live
  from here. Until this appears, DLSS and DLSS-G are deliberately held back.
- `Streamline bridge: holding <call> back - the 2.x runtime has no device yet` - one line per
  call, and harmless before device creation. Still appearing once the game is rendering means
  the device never reached the runtime by any of the three routes.
- `Streamline bridge: feature entry points PARTIALLY resolved` - the staged folder is missing
  a plugin. Distinct from the deviceless case on purpose; they need different fixes.
- `Streamline Hook: leaving sl.interposer.dll unhooked - the generation bridge routed every
  call away` - the superseded 1.x module is being left alone, as it must be.
- `Streamline bridge: the game's D3D12CreateDevice reached the CE-owned 2.x runtime` - the
  device is behind the right interposer.
- `... but the game never created its device or factory through the bridge` - it is not, and
  DLSS-G will not engage. CE was injected too late; nothing in memory can undo that.
- `Streamline bridge: not activating - <reason>` - names which precondition failed.
- `Streamline bridge: refusing <call> - <why>` - one line per distinct reason, so a first
  bridged run diagnoses itself.
- `Streamline bridge: <call> returned sl::Result=N` - the 2.x runtime rejected a translated call.
- A healthy bridged FG session has no `slReflexSleep` refusal and no
  `eDLSSGStatusFailReflexNotDetectedAtRuntime` records in `sl.log`.
- `Streamline 1.x probe: ...` - a recorded payload (fires unbridged too).

## Open questions / stale-risk

- **Runtime path validated through DLSS-G bring-up.** Session `20260822_015042` proved that a
  late takeover, device handoff, tags/constants/evaluation, NGX SR and the SL2 proxy swapchain
  can all run together. Its remaining blocker was Reflex *runtime detection*, not the call path.
- **The `20260821_161620` startup C++ exception did not recur** in `20260821_163534`, which
  reached the render loop. It remains unexplained rather than fixed; if it returns, `sl.log`
  is now there to say whether Streamline was involved.
- **Reflex detection needs the title's PRESENT markers** (see the eighth run). The synthesized
  per-frame sleep is a fallback for a title that drives none. Frame-limit and marker fields of
  `ReflexConstants` keep 2.x defaults because they were never measured.
- **`slShutdown` on a 1.x runtime that has only been `slInit`ed is expected to unload its
  plugins, but that is not verified.** The inventory line printed straight after the call is
  there to settle it: if `sl.common.dll` is still listed from the game's folder afterwards,
  1.x kept it mapped and the "no Streamline DLLs from the game directory" goal is only
  partly met - the runtime is still idle either way.
- `DLSSGConstants` beyond `mode` is unconfirmed. `+4` is mapped to `numFramesToGenerate`
  because it was constantly 1 and 2.x defaults to 1 - plausible, not proven, and it is the
  field `dlss_fg_factor` interacts with.
- `slSetTag` translates immediately to deprecated 2.x `slSetTag`; each tag is
  `eValidUntilPresent`, so the command buffer is allowed to be null. Deferral overflowed in
  `20260822_005204` and left evaluate with incomplete inputs.
- `Extent` is assumed to match 2.x's `{top,left,width,height}`; only consumed when the game
  supplies one.
- `dlss_sr_dll_path` / `dlss_fg_dll_path` keep working while bridged (they are NGX runtimes,
  not `sl.*`), but should point at the same folder as `streamline_dll_path`: a bridged runtime
  resolves its own `nvngx_*` out of the folder it was pinned to. CE logs any disagreement.

Last verified 2026-10-01 (`notRenderingGameFrames` gate validated in `20261001_092557`; tag persistence
validated in `20261001_093949`; the present-marker guard from that session pending a hardware run; device-reset cause from `20261001_034038`, validated); before that 2026-08-22 (session `20260822_015042`; ABI measurements from The Witcher 3 sessions
`20260821_041255` and `20260821_042540`, activation timing from `20260821_151738` and
`20260821_151924`, the bridged runs from `20260821_155250`, `20260821_161620` and
`20260821_163534`).
