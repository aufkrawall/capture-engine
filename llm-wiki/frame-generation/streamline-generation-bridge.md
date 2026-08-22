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
| Native D3D12 device continuity | `hook/apis/streamline_bridge_device_cache.{h,cpp}` |
| 1.x -> 2.x call translation | `hook/apis/streamline_bridge_translate.{h,cpp}` (x64 only) |
| The measured 1.x structures | `hook/apis/streamline_bridge_v1_abi.h` (x64 only) |
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

## The third bridged run: `sl.log` answered it in one line

`20260821_163534` (0.1.6215) is where Streamline's own log paid for itself. The startup C++
exception did not recur - that run reached the render loop, created its swapchain, and crashed
in the same place as the first: `Bridged_slSetConstants -> sl_interposer!slSetConstants+0x49 ->
0x0`. This time the cause is in NVIDIA's words:

```
d3d12Device.cpp:396[Release]   Destroyed D3D12Device proxy 0x... - native device 0x... ref count 0
pluginManager.cpp:1331[initializePlugins] D3D or VK API hook is activated without device being
                               created, did you forget to call `slSetD3DDevice`
sl.cpp:1115[slGetFeatureFunction] 'kFeatureDLSS_G' has not been initialized yet.
```

**The game's first D3D12 device is a capability probe it throws away.** The Witcher 3 creates a
device through the bridge at +1.9 s, Streamline proxies it, and the game releases it at +2.3 s -
`ref count 0`. The device it actually renders with is created seven seconds later. CE had marked
the runtime ready on that first device, so when the real one arrived `SetV2RuntimeDevice`
early-returned "already done", `slSetD3DDevice` was never called with it, Streamline's plugin
manager spent the rest of the session asking for that call by name, and the gate that exists to
prevent exactly this crash waved the call through because CE had told it a lie.

Two rules came out of it, and they generalise past this title:

- **Readiness is Streamline answering `slGetFeatureFunction`, never anything CE infers.** Not
  "we called `slSetD3DDevice`", not "the interposer created a device". Both were tried; both
  produced the same null call. `slGetFeatureFunction` returns a pointer out of the very plugin
  context whose absence makes `slSetConstants` jump through null, so it is not a proxy for the
  condition, it *is* the condition.
- **Hand over every distinct device, not the first one.** A device is an action CE takes, never
  a conclusion CE draws. The interposer's own device wins over CE's queue-derived one - it is
  Streamline's proxy, at the moment the SDK documents the call for - but a later interposed
  device supersedes an earlier one, which is what a throwaway probe requires.

The readiness probe is event-driven rather than polled: an epoch counter bumped when a device is
handed over and when the game's frame index moves, with at most one `slGetFeatureFunction` per
epoch and none at all once the answer is yes. A frame boundary is a real state transition - it is
what a game reaching its render loop looks like, and it is when Streamline finishes bringing
DLSS-G's context up around the swapchain - so this converges without a timer.

Also settled by that log: `featuresToLoad` **is** honoured (`Ignoring plugin 'sl.deepdvc' since
it is was not requested by the host`), so the earlier suspicion about the whole plugin set being
loaded was wrong - what was observed was Streamline probing each plugin's config and unloading
what it does not need.

## The fourth bridged run: two production-runtime assumptions failed

Session `20260821_234606` (0.1.6224) had verbose `sl.log` for the first time. CE handed the
first interposed device to Streamline successfully, but when the title asked for its real render
device roughly eight seconds later the forwarded V2 call returned:

```text
d3d12.cpp:76[D3D12CreateDevice] D3D12CreateDevice failed with error code 887a0007
```

The game treated that failure as fatal and threw an unhandled C++ exception (`0xE06D7363`)
about a second later. Earlier in the same log Streamline also said why DLSS-G could never have
engaged:

```text
Please provide correct application id when calling slInit - NGX based features will be disabled
Failed to initialize NGX, any SL feature requiring NGX will be unloaded and disabled
```

Both are bridge defects rather than capture-engine interference:

- **V2 interposer creation is not required.** The bridge had treated every `D3D12CreateDevice`
  import as a pass-through into V2, coupling ordinary D3D12 semantics to Streamline's proxy
  implementation. CE now calls Microsoft's `d3d12.dll` and explicitly hands every distinct
  resulting device to V2 with `slSetD3DDevice` - the SDK's supported manual-device route.
- **Zero application ID becomes a temporary ID in production.** V2 replaced zero with
  `kTemporaryAppId` (`100721531` in the log) and refused NGX. A late bridge cannot observe the
  game's original `slInit` application ID, so it supplies the other accepted identity: a stable
  project ID derived from the host executable path plus the host version. No title table exists,
  and the path itself does not leave the process.

Session `20260822_001759` showed that merely calling Microsoft's API was not enough: the later
real-device request failed with the same reset even without V2 interposer creation. CE now reads
the requested adapter's LUID, creates a fresh OS DXGI factory, resolves an equivalent adapter from
that factory, and gives that instance to D3D12. This preserves multi-GPU intent while keeping
another module's object lifetime out of D3D12. Device failures log requested/resolved adapters,
feature level and IID.

Session `20260822_003051` proved that even a freshly LUID-matched instance could be rejected with
the same reset while an earlier device through the route succeeded. For device-lost-class HRESULTs
only, CE makes one logged retry with DXGI/D3D12's default adapter. On multi-GPU systems this is a
visible compatibility fallback, not a silent policy: the log names both HRESULTs and both adapter
pointers.

Session `20260822_021816` motivated retaining the successful device, but initially reused it only
after both recreation attempts failed. Session `20260822_174509` disproved that ordering: the
redundant call itself returned `DXGI_ERROR_DEVICE_RESET`, and by the time the fallback checked the
retained device it had been reset too. The title threw before making a single translated feature
call.

The cache therefore answers a compatible same-LUID object request **before entering D3D12**, after
checking `ID3D12Device::GetDeviceRemovedReason` and querying the requested COM interface. A higher
feature-level request, a different adapter, an unsupported interface, or an unhealthy device still
takes the native creation path. Cache entries use the created device's actual adapter LUID rather
than the requested/default adapter argument, and default-adapter reuse is tracked explicitly. The
handoff path compares canonical `IUnknown` identity, so requesting `ID3D12Device1` from the same
object cannot spuriously rebind the 2.x runtime merely because that interface has a different
pointer value.

## The fifth bridged run: the duplicate guard did not guard an absolute request

Session `20260822_182415` had a different state from `20260822_174509`. CDB found both the game's
`nvngx_dlss.dll` 3.1.1 and the configured 310.7.128 image live; for FG, DriverStore 310.2.1 and the
configured 310.7.128 image were both live. The hook log had already said why, but its claimed
outcome was false:

```text
Loader redirect refused for ...\npi\sl\nvngx_dlss.dll: nvngx_dlss.dll is already loaded from
...\The Witcher 3\bin\x64_dx12\nvngx_dlss.dll ... keeping the loaded copy
Loader: runtime module loaded: nvngx_dlss.dll -> ...\npi\sl\nvngx_dlss.dll
```

An empty redirect means "call the loader with the original request." That preserves the loaded
copy only when the original request names it. The CE-owned 2.x runtime requested the configured
absolute path, so the supposed refusal replayed exactly the path that mapped the duplicate. A
duplicate decision now returns the already-resident physical path instead of empty.

The bridge also owns the generation transition rather than depending on that fallback. For a real
V1-process/V2-folder pairing it pre-registers `nvngx_dlss.dll` and `nvngx_dlssg.dll` from the
Streamline folder before taking over the imports, then patches the already-resident 1.x SL modules'
LoadLibrary IATs so absolute internal loads reach the same copies. If a legacy image already won,
the quiesce snapshots it before `slShutdown` and releases that captured foreign feature reference
after shutdown but before 2.x initialization. It releases once only: draining an opaque loader
count could steal lifetime from an independent integration. The inventory now enumerates every
physical SR/FG image so a duplicate cannot hide behind `GetModuleHandle`'s first answer.

The device was healthy at the 11.7-second cached capability probe and reset before the final
object request. The mixed NGX state is the concrete unsafe difference in this run; treating it as
the reset's cause remains an inference until the next runtime validation proves a single NGX
generation and reaches the render loop.

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
| `ReflexConstants` | `mode`@0 **only**; the struct is 8 bytes | 1 (and +4 always 0) |
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
- **Reflex activation alone is not runtime detection.** 1.x drives options through
  `slSetFeatureConstants`, while native 2.x titles also call `slReflexSleep` once per frame.
  Witcher 3 `20260822_015042` accepted `eLowLatencyWithBoost` yet kept reporting
  `eDLSSGStatusFailReflexNotDetectedAtRuntime`; there was no sleep traffic. While bridged DLSS-G
  is on, CE now resolves `slReflexSleep`, promotes the mode as before, and issues exactly one
  sleep per translated game-frame token. Turning DLSS-G off stops the sleeps and restores off.
  Frame-limit and marker fields keep 2.x defaults because they were never measured.
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

Last verified 2026-08-22 (session `20260822_015042`; ABI measurements from The Witcher 3 sessions
`20260821_041255` and `20260821_042540`, activation timing from `20260821_151738` and
`20260821_151924`, the bridged runs from `20260821_155250`, `20260821_161620` and
`20260821_163534`).
