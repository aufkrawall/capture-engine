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
| 2.x bring-up (load by full path, `slInit`, inventory) | `hook/apis/streamline_bridge_runtime.{h,cpp}` |
| 1.x -> 2.x call translation | `hook/apis/streamline_bridge_translate.{h,cpp}` (x64 only) |
| Passive layout recorder | `hook/apis/streamline_v1_feature_probe.{h,cpp}` |
| Generation classification | `hook/common/streamline_api_generation.h` |
| Tests | `tests/test_streamline_bridge_policy.cpp` |
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
being held to are *the same event*, with WMI notification (`WITHIN 0.5`), a 114 ms config
reload and a ~380 ms remote-thread `LoadLibrary` stacked in between. Session
`20260821_151924` shows the losing end: `d3d12=1` on the very first poll, i.e. `slInit` had
already run before CE was even notified the process existed.

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

So a late arrival now takes the imports over and shuts the game's 1.x runtime back down
through the `slShutdown` slot it saved while repointing it, reaching the same end state from
a later start. Only `DeclinedGameOwnsItsDevice` still refuses.

## The first bridged run: it works, and what it found

Session `20260821_155250` is the first in which any of this executed. The takeover itself
did exactly what it was designed to:

```
Streamline bridge ACTIVE: 15 of 15 sl.interposer import slots now reach CE
Streamline bridge: Streamline 2.12.0 initialised with plugins pinned to <staged folder>
Streamline bridge: shut the game's own 1.x Streamline runtime down (returned true)
inventory (after quiescing): sl.common.dll 2.12.0 / sl.dlss.dll 2.12.0 / sl.dlss_g.dll 2.12.0
                             / sl.reflex.dll 2.12.0 / sl.pcl.dll 2.12.0  - all from the staged folder
inventory (after quiescing): sl.interposer.dll 1.0.0  - the game's, still mapped, never called again
```

The 1.x quiesce works and does unload the plugins: every `sl.*` module in the process after
it is the staged 2.x one. Only the statically imported interposer image remains, inert.

It then crashed, and the crash named the next two problems exactly.

### The 2.x runtime had no device, and CE called it anyway

```
0xC0000005 at 0x0000000000000000, RIP=0
capture_hook_x64!Bridged_slSetConstants
sl_interposer!slSetConstants+0x49
0x0
```

Streamline 2.x's exports are forwarders into a plugin the manager binds at
`slSetD3DDevice`. Before that they do not return an error - they jump through a null
pointer. The same session shows the quieter half of the same cause: the feature entry points
never resolved, because `slGetFeatureFunction` needs the device too, so DLSS and DLSS-G were
being refused for a reason that would never have stopped being true.

`sl_core_api.h` states it per function: "requires DX/VK device to be created before calling
it" on `slSetConstants`, `slSetTagForFrame` and `slEvaluateFeature`, "Must be called AFTER
device is set" on `slGetFeatureFunction`. `slIsFeatureSupported` takes an `AdapterInfo` and
is the one call that legitimately answers early - and it did, correctly, in that same run.

Why no device: the game imports `D3D12CreateDevice` from `sl.interposer.dll` (confirmed from
its import table), so the bridge does forward device creation to the 2.x interposer - but
nothing made the runtime's binding CE's business, and nothing checked. Now three things do:

1. The bridge's own `D3D12CreateDevice` slot calls `slSetD3DDevice` on success.
2. CE's DX12 hook calls `NotifyD3D12Device` when it derives the device from a command queue -
   the route that covers an Agility SDK title, whose device comes from
   `ID3D12DeviceFactory::CreateDevice` and never touches an `sl.interposer` export at all.
3. **The gate asks the runtime rather than trusting either.** `slGetFeatureFunction`
   succeeding is Streamline's own answer to "do you have a device", so a runtime that bound
   the device through its own interposer is recognised without CE having done anything, and
   an `slSetD3DDevice` that returns an error because the device was already set is not
   mistaken for a failure. Until that answer is yes, every device-dependent call is refused.

### CE was hooking the module nothing calls

```
Streamline Hook: sl.interposer.dll speaks Streamline 1.x - CE installs only the hooks ...
Streamline Hook: registered the Streamline 2.x slSetTag/slEvaluateFeature dynamic routes
Streamline Hook: Inline hook installed for slSetTag at <1.x address>
Streamline Hook: Refusing to retarget slSetTag from <1.x address> to <2.x address>
    - the installed target is still mapped
```

A 2.x-shaped hook landed on the inert 1.x image - the argument truncation the generation gate
exists to prevent - and then held CE's single forward pointer per symbol, so the hook on the
runtime that actually runs was refused as a duplicate. CE would have watched a module nothing
calls while the live one went unobserved: `dlss_fg_factor`, `dlss_fg_preset` and the overlay's
FG state machine, all blind. `StreamlineModuleSupersededByBridge` now leaves a bridged-away
1.x module entirely unhooked.

## The second bridged run: further, and two more defects

`20260821_161620` (build 0.1.6212) confirmed both of the above fixes and produced the
cleanest state so far:

```
Streamline bridge ACTIVE: 15 of 15 sl.interposer import slots now reach CE
Streamline bridge: shut the game's own 1.x Streamline runtime down (returned true)
Streamline Hook: leaving sl.interposer.dll unhooked - the generation bridge routed every call away
Streamline Hook: Inline hook installed for slSetTag at 00007FFE262473D0    <- the 2.x interposer
Streamline bridge: Streamline 2.12.0 initialised with plugins pinned to <staged folder>
Streamline bridge: the game's D3D12CreateDevice reached the CE-owned 2.x runtime
```

So the game's device creation does go through `sl.interposer!D3D12CreateDevice` and does reach
the bridged runtime, and CE's hooks now land on the runtime that is actually called. Two things
were still wrong.

**The readiness probe vetoed a direct answer.**

```
Streamline bridge: slSetD3DDevice(...) returned sl::Result=0 and the runtime still reports no device
```

`slSetD3DDevice` accepted the device and CE held every call back anyway, because the confirming
`slGetFeatureFunction` probe had been made the authority. A probe failure proves nothing - it
also fails while the DLSS plugin is still coming up - so it may only ever confirm, never veto.

**CE bound the same device twice.** The 2.x interposer had just created and bound the device
inside the call CE was returning from, and CE then called `slSetD3DDevice` on it again - a
call NVIDIA documents as "NOT thread safe and should be called IMMEDIATELY after main device is
created", issued from inside that very creation, and through CE's own inline hook on that
export. Streamline offers interposed device creation **or** `slSetD3DDevice` for a host that
made its own device; doing both is not a belt-and-braces, it is a second bind. The bridge now
marks the runtime ready without calling anything when the interposer created the device, and
keeps `slSetD3DDevice` for the route where it is actually required - an Agility SDK title,
whose device comes from `ID3D12DeviceFactory::CreateDevice` and which Streamline never sees.

The run still ended in an unhandled C++ exception (`0xE06D7363`) about seven seconds after the
last Streamline interaction, before the game presented a frame and before it made a single
feature call. **That one is unattributed.** `ntdll!RtlUserThreadStart` caught it on the main
thread, so the throw site was fully unwound before CE's pre-termination hook ran and the dump
holds no trace of it. Note that this game has a documented history of exactly this exception
shape at startup which the user reproduced with CE not injected at all (`20260820_142322`),
so it must not be assumed to be the bridge's - and must not be assumed not to be, either.
Two changes exist to settle it next time:

- **`sl.log`, verbose, in CE's session directory, at trace log level.** CE's log can say what
  CE did but not what Streamline made of it. NVIDIA's own account of plugin loading, device
  binding and feature init is the missing half.
- **The hand-mirrored `sl::Preferences` is gone**, replaced by the SDK's own struct. The mirror
  had already been wrong once (`BaseStructure` puts `next` at 0 and `structType` at 8, the
  reverse of how the declaration reads) and re-verifying it field by field is work that recurs
  every time the staged SDK moves. The header is on the hook DLL's include path anyway.

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
- **No device, no call.** Most 2.x exports jump through an unbound plugin pointer before the
  device is bound, so every device-dependent translation is refused until it is. This is a
  crash, not a courtesy - see above. The gate itself is a plain atomic read: an earlier version
  asked the runtime on every call, which put a `slGetFeatureFunction` - CE inline-hooks that
  export on the bridged interposer - in front of every tag and constant, on the render thread.
- **Bind the device once, through one mechanism.** Interposed creation or `slSetD3DDevice`,
  never both. Which one applies is knowable: if the game's device creation came through the
  bridge's slot, the interposer already has it.
- **A readiness probe may confirm, never veto.** `slGetFeatureFunction` succeeding proves a
  device is bound; it failing proves nothing, because it also fails while a plugin is still
  coming up.
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

`Constants` - 456 bytes, **no** BaseStructure header. Identical in upstream v1.1.1 and
OptiScaler's set. Translation to 2.x: prepend the 2.x `BaseStructure`, copy
`cameraViewToClip`..`reset` verbatim, **drop `notRenderingGameFrames`** (no 2.x field), keep
the three motion-vector/projection Booleans, leave 2.x
`minRelativeLinearDepthObjectSeparation` at its **40.0f** default rather than zero, drop `ext`.

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
| `ReflexConstants` | `mode`@0, `frameLimitUs`@12 | 1; 565 |
| `DLSSGConstants` | `mode`@0 (**0 = off, 1 = on**), `numFramesToGenerate`@4 (unconfirmed) | 0 -> 1, 68 ms before `DLSS FG ACTIVATED`; +4 constantly 1 |

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
- `Streamline 1.x probe: ...` - a recorded payload (fires unbridged too).

## Open questions / stale-risk

- **Partly validated.** The takeover, the 2.x bring-up, the 1.x quiesce, `slIsFeatureSupported`
  and the device reaching the 2.x interposer are all proven. Nothing past `slSetConstants` has
  ever executed successfully, so the tag/constant/evaluate path and DLSS-G itself are untried.
- **The `20260821_161620` startup exception is unattributed**, and the throw site is not
  recoverable from that dump. Next run: read `sl.log` in the session directory first. If it
  shows a healthy Streamline, the next thing to bisect is CE's own inline hooks on the bridged
  2.x interposer - they moved there in 0.1.6212 and are the other behaviour change of that
  build.
- **Streamline loads every plugin in the staged folder regardless of `featuresToLoad`**
  (`sl.deepdvc`, `sl.directsr`, `sl.dlss_d`, `sl.nis`, and `nvngx_dlssd` at 40 MB), taking about
  3.7 s inside the game's first bridged call. Consistent with SL2 probing each plugin's config
  and unloading the ones it does not need - successive plugins were observed loading at the same
  base address - but not confirmed, and it is worth knowing whether a leaner staged folder
  shortens that stall.
- **`slShutdown` on a 1.x runtime that has only been `slInit`ed is expected to unload its
  plugins, but that is not verified.** The inventory line printed straight after the call is
  there to settle it: if `sl.common.dll` is still listed from the game's folder afterwards,
  1.x kept it mapped and the "no Streamline DLLs from the game directory" goal is only
  partly met - the runtime is still idle either way.
- `DLSSGConstants` beyond `mode` is unconfirmed. `+4` is mapped to `numFramesToGenerate`
  because it was constantly 1 and 2.x defaults to 1 - plausible, not proven, and it is the
  field `dlss_fg_factor` interacts with.
- **Reflex is deliberately unconfigured**: 1.x drives it through `slSetFeatureConstants`,
  2.x through `slReflexSetOptions`, and that mapping is unverified. DLSS-G depends on Reflex,
  so this is a prime suspect if FG does not engage.
- The `slSetTag` deferral (1.x carries no command buffer, so tags flush at the next
  `slEvaluateFeature`) is the same pattern the unbridged 1.x overlay route uses, but has not
  been exercised through the bridge.
- `Extent` is assumed to match 2.x's `{top,left,width,height}`; only consumed when the game
  supplies one.
- `dlss_sr_dll_path` / `dlss_fg_dll_path` keep working while bridged (they are NGX runtimes,
  not `sl.*`), but should point at the same folder as `streamline_dll_path`: a bridged runtime
  resolves its own `nvngx_*` out of the folder it was pinned to. CE logs any disagreement.

Last verified 2026-08-21 (build 0.1.6215; ABI measurements from The Witcher 3 sessions
`20260821_041255` and `20260821_042540`, activation timing from `20260821_151738` and
`20260821_151924`, the bridged runs from `20260821_155250` and `20260821_161620`).
