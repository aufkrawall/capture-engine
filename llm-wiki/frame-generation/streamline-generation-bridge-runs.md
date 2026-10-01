# Streamline generation bridge: run history

Chronology of the bridged Witcher 3 hardware runs behind
[streamline-generation-bridge.md](streamline-generation-bridge.md). The current rules distilled
from these runs live in that page's Invariants section; this page keeps the evidence and the
reasoning that led to each fix.

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
generation and reaches the render loop. (Superseded: the seventh run found the reset's actual
cause, a debug-layer setting reaching D3D12 while the device was retained.)

## The sixth bridged run: one bring-up route crossed the teardown boundary

Session `20260822_185158` reached the loading screen and produced a direct game-side access violation,
but it also disproved the claimed NGX ordering. The decisive sequence was:

```text
18:52:05.590  Streamline 2.12.128 initialised
18:52:05.592  FAILED to retire ... game nvngx_dlss.dll 3.1.1
18:52:05.592  FAILED to retire ... DriverStore nvngx_dlssg.dll 310.2.1
```

There was no longer a duplicate physical image; the corrected duplicate route made SL2 reuse the old
resident copy. That still violated the upgrade contract. SL2 acquired another loader reference before
the old runtime shut down, so the one safe post-shutdown release could not retire either image. The
dump consequently held game SR 3.1.1 and DriverStore FG 310.2.1, not the configured 310.7.128 copies.

The ordinary thunk path already called quiesce before `EnsureRuntimeReady`. The bypass was the
queue-derived `NotifyD3D12Device`, which called `EnsureRuntimeReady` directly while quiescence was
pending. The boundary is now a global state machine rather than call-site convention: every V2 route
must transition Pending -> Running -> Complete, and `EnsureRuntimeReady` refuses any other state.
The bridge remains inactive while import thunks are being repointed, so an early entrant forwards to
1.x without initiating teardown. Pending is published after the complete rewrite and before Active;
the active acquire therefore cannot observe an incomplete takeover or miss the teardown requirement.
Concurrent callers block on the state transition without polling; a same-thread path re-entered by
legacy `slShutdown` forwards to V1 until teardown returns, avoiding self-deadlock. Complete is
published only after shutdown, NGX retirement, and preloading/pinning both configured replacements.
Every physical SR/FG module is verified against the configured folder before the first V2 interposer
load, closing the small race between retirement and the replacement runtime's own feature loads.

The first DLSS evaluate later returned `eErrorMissingConstants` and the title dereferenced null about
six seconds after that. This is correlation, not an attributed stack: the dump contains no CE or SL
frame. The first translated set-constants and evaluate calls now log the 1.x input frame, actual 2.x
token frame, viewport and result, which will settle whether a frame-token translation problem remains
after the configured NGX generation is genuinely live.

## The seventh bridged run: the game's debug-layer setting reset the retained device

Session `20261001_034038` (0.1.6870, `witcher3.exe`, SL 2.14.1, Smooth Motion off) finally explains
the `DXGI_ERROR_DEVICE_RESET` from runs four to six. The retained device was healthy when CE
answered the capability probe at 44.1 s. At 47.5 s the next probe (`ppDevice=null`) found it
removed, and both native retries returned the same reset. The title threw `0xE06D7363`. The
System event log has no TDR (4101) in that window, so nothing GPU-side reset it.

The dump's unloaded-module list shows `D3D12SDKLayers.dll` and `DXGIDebug.dll` loaded and
unloaded. Disassembling the shipped executable at `witcher3+0x7f7a70`, the routine that ends in
the probe/throw at `+0x7f7c3f`, gives this sequence: `D3D12GetDebugInterface(IID_ID3D12Debug)`,
`QueryInterface(IID_ID3D12Debug5)`, slot 8 = `SetEnableAutoName(TRUE)`, release, then
`CreateDXGIFactory2` and the per-adapter `D3D12CreateDevice(adapter, 11_0, ..., nullptr)` loop.
The title never calls `EnableDebugLayer`.

Unbridged, the probe device is already destroyed at this point, so the setting has nothing to hit.
Bridged, CE's device cache, `slSetD3DDevice` and NGX keep the probe device alive. D3D12 creates one
device per adapter, so once that device is removed every later creation on the adapter returns its
removal reason for as long as those references live. Whether the reset happens inside
`D3D12GetDebugInterface` or in the setter is not separable from this evidence. The fix therefore
refuses before forwarding.

`Bridged_D3D12GetDebugInterface` answers the ID3D12Debug family (Debug..Debug6) with
`DXGI_ERROR_SDK_COMPONENT_MISSING` while `HasRetainedDevice()` is true. The title already handles
that failure, because its code skips straight to the factory. No capability is lost: the device the
bridge hands back already exists, so a layer configured now could never apply to it. DRED settings
and other debug interfaces still forward. Before any device exists, the request still forwards. Only
the cache decides: it holds exactly the devices the bridge created on the game's behalf. A
queue-derived handoff names a device the game itself still holds, so a reset there would also hit the
unbridged title.

Pending: a hardware run should show the refusal line, then `reused the prior successful D3D12 device`
on the real-device request and DLSS-G reaching the render loop.

## The eighth bridged run: Reflex markers arrive through slEvaluateFeature

Session `20261001_040020` (0.1.6871) ran without crashing. DLSS SR was aliased in motion (the
Boolean layout above), and DLSS-G added no frames. `sl.log` held 553x
`eDLSSGStatusFailReflexNotDetectedAtRuntime - sl.reflex must be enabled and active -1 != <frame>`.
In the open 2.x sl.reflex source, `kMarkerPresentFrame` is set only by a `ePresentStart` marker,
and `-1` means none ever arrived.

1.x has no marker or sleep export. The v1.1.1 Reflex guide has the title call
`slEvaluateFeature(nullptr, eFeatureReflex, frameIndex, marker)` with **the marker in `id`**. Sleep
is `id = eReflexMarkerSleep = 0x1000`, with frame 0. The 1.5.6 `sl.reflex` still registers that
evaluate callback (`latencyBeginEvaluation`). The bridge returned false for every evaluate with a
null command buffer, so all markers and sleeps vanished. The title also queried
`slGetFeatureSettings(Reflex)`, got a refusal, and kept Reflex mode 0 (it was 1 unbridged).

0.1.6872:
- Reflex evaluates route to `slPCLSetMarker` (1.x 0..8 = 2.x `PCLMarker` 0..8, 6 dropped) and
  `slReflexSleep`. Sleep with frame 0 uses the newest frame's token. The first title sleep stops
  CE's synthesized sleep.
- `slGetFeatureSettings(Reflex)` answers the four fields the 1.5.6 writer stores, from
  `slReflexGetState` / `slPCLGetState`.
- `TokenFor` keeps 16 recent frame -> token pairs (`RecentFrameTokens`). The single cached token
  thrashed between the game thread (constants for N+1) and the render thread (evaluate for N), and
  re-asked `slGetNewFrameToken` for an issued index.

**Validated in `20261001_041637`** (0.1.6872): the user reports SR and FG working. Display interval
is a steady 7.2 ms (138 fps at the Reflex cap of a 143 Hz panel) from a 28.9 ms base under 4x MFG,
with `generationObserved=1` and no `ReflexNotDetectedAtRuntime`. The title drives its own sleep
(frame 0). The run also showed that an FG-off translation forced Reflex off, although the title had
asked for mode 1, until the title's next constants call. 0.1.6873 hands back the title's last mode
(`ReflexModeForDlssgState`).

## The ninth bridged run: 1.x gates interpolation on `notRenderingGameFrames`

Session `20261001_090234` (0.1.6883, 4x MFG, FG preset B): heavy FG artifacts for the first seconds
after a save loaded, then clean. Pacing, overlay and both FG transitions were healthy. `sl.log` showed
no DLSS-G warning at all, so the runtime saw nothing wrong with its inputs.

The game's own 1.5.6 `sl.dlss_g.dll` explains what the bridge dropped. `presentCommon` (function
`0x19f20`) zeroes r15d at `+0x1a064`, then at `+0x1ab8b` runs
`cmp byte ptr [rax+0x1a0], r15b; cmove edx, ecx`: the interpolate flag survives only when the frame's
`Constants.notRenderingGameFrames` is exactly eFalse. eInvalid also suppresses and logs "cannot be
left as invalid"; an `ignoreNotRenderingGameFrames` config key exists. Both the 1.5.6 and the 2.14.1
plugin pass a constant 0 for the NGX parameter `DLSSG.NotRenderingGameFrames`, so the gate lives in
the plugin. 2.x has no field, and the bridge discarded it, so 2.x interpolated every frame the title
had excluded.

0.1.6884 keeps a per-viewport request (title mode and frame count from DLSS-G constants, the flag from
common constants) and forwards `eOff` while the flag is set. `eRetainResourcesWhenOff` is set whenever
the title wants FG on, so resuming costs no rebuild. Reflex follows the title's intent, not the gate.
Granularity is the constants call, which can run one frame ahead of the present it gates.

Flag transitions and history resets now log (`title marked frame N ... as NOT a game frame`,
`title requested a history reset`).

**Confirmed in `20261001_092557`:** the title sets the flag on load and menu frames (transitions #1-#9)
and toggles DLSS-G mode with it.

## The tenth bridged run: 2.x expires tags that 1.x kept

Same session: brief dark flashes while traversing. Three times during FG gameplay `sl.log` shows
`Invalidating the hanging tag 0/1/2 (created at frame N, current frame N+2)`, then `Failed to find
global tag 'kBufferTypeDepth'` / `'kBufferTypeMotionVectors'`, then interpolation off and back on
~30 ms later. Tags and presents come from the same game thread (0x5164), and the per-present CSV shows
an extra present burst ~4.5 ms after a normal one (16 such gaps in the run), so the title presented
twice without re-tagging. The bridge sent no option change at those moments.

SL2's open `sl.common` (`commonEntry.cpp`, `ResourceTaggingGeneral::getTag`) expires every legacy
(frame-less) global tag once `getCurrentFrame() > uFrameWhenTagged + 1`, whatever its lifecycle; the
counter advances per app present. The 1.5.6 `sl.common.dll` has no expiry string or logic at all: a 1.x
tag lives until replaced.

0.1.6885 remembers the title's last depth, motion-vector, HUD-less and UI tags per viewport
(`RememberPresentTag`) and re-issues them on the title's 1.x present-end marker (5) while 2.x generates
(`RefreshPersistentPresentTags`). A null tag erases the entry. The re-tag happens on the game's thread
right after its Present returns, so it extends nothing past the lifetime SL already held from the
title's own tag. Frame-based tagging (`slSetTagForFrame`) was not used: 1.x `slSetTag` carries no frame
index to key it on.

## The eleventh bridged run: an unmarked present fails 2.x's Reflex check

Session `20261001_093949` (0.1.6885): the tag fix held (no `Failed to find global tag`, no
interpolation toggles during gameplay, `re-issuing 3 DLSS-G input tag(s)` logged), but dark flashes
remained. `sl.log` shows three `eDLSSGStatusFailReflexNotDetectedAtRuntime - sl.reflex must be enabled
and active 2667 != 2668` (also `3423 != 3424`, `3452 != 3453`). CE's per-present CSV has exactly three
irregular cadences in FG gameplay - a 4-present burst followed ~5 ms later by a second one - and their
spacing (21.826 s, 0.807 s) matches the `sl.log` failures to the millisecond. The marker summary stays
1:1 (`presentStart=3000 presentEnd=2999`), so the second present carried no markers at all.

SL2's open source explains the check. `sl.reflex` (`reflexEntry.cpp`) latches
`kCurrentFrame = getFinishedFrameIndex() + 1` only on a PRESENT_START marker; `getFinishedFrameIndex`
is sl.common's `ctx.currentFrame`, bumped by `presentCommon` on every non-`DXGI_PRESENT_TEST` present
(`commonInterface.cpp`). A present without its own PRESENT_START therefore arrives one ahead of
Reflex's frame, and DLSS-G skips it. The previous run's tag expiry hit first on the same extra present;
with tags kept alive, this check was the next one in line. 1.x had no such check.

0.1.6886 (`streamline_bridge_present.cpp`) inline-hooks the bridge's own 2.x `sl.common`
`slHookPresent` / `slHookPresent1`, resolved through its `slGetPluginFunction` (the plugin publishes
no other exports) and verified to lie inside that image. Before each counted present,
`PresentMarkerLedger` (`streamline_bridge_dlssg_gate.h`) checks whether the title's own PRESENT_START
reached 2.x since the previous present; when it did not, while 2.x generates, the bridge sends a
PRESENT_START/PRESENT_END pair for the title's last marked frame, reusing that frame's existing token
(never a new token for an old index). Test presents are neither counted nor marked. Logged as `title
presented without a Reflex PRESENT_START marker - re-marked frame N`, and the marker summary gains
`re-marked presents=N`.

Note: `title presented again without re-tagging` never fired in this run. It is driven by the
title's present-end marker, so it cannot see a present the title sends without markers; the
present-marker guard's log line is the one that counts those presents.

## The twelfth bridged run: flashes survive the re-mark

Session `20261001_105517` (0.1.6889): `ReflexNotDetectedAtRuntime` is gone and `sl.log` is silent
during gameplay, yet the user still saw occasional dark flashes. The three re-marks during
gameplay (`re-marked frame 788/1374/3631`) are the CSV's only 8-present groups (two 4x groups
~9 ms apart). Their spacing (16.93 s, 65.24 s) matches to the millisecond. DLSS-G now generates a
full group for the extra present. It flashed under all three handlings so far: tags expired
(092557), Reflex check skipped (093949), generated (105517). That points at the extra present's own
inputs or content, not at a single 2.x check. Unresolved: whether that present re-shows frame N
(re-issued `eValidUntilPresent` tags then point at resources the next frame may already be writing)
or is a new frame the title left unmarked (then frame N's constants are the wrong ones).

CE now logs `unmarked present #N` with the present's sync interval, flags, the time since the
previous present and that present's HRESULT/skip. It also logs the title's activity since the
previous present and before it: constants (frame), tags, upscaler evaluates (frame), Reflex
markers (last id, frame). A next run decides between the two readings. Also open: whether the
unbridged 1.x game shows the same flash.

## The thirteenth bridged run: the extra present is a re-present

Session `20261001_141737` (0.1.6894). The user saw one flash late in the run and then quit. The only
gameplay re-mark (`unmarked present #3`, frame 2721) is the CSV's only 8-present group, about 5 s
before FG ends. The two in-game unmarked presents logged the same pattern:

- **Since the previous present:** `constants=0 tags=0 evaluates=0`, 5 Reflex markers, the last of
  them the 1.x sleep (`0x1000`, frame 0).
- **Before it:** a normal frame with constants, 7 tags, the DLSS evaluate and PRESENT_START, all for
  frame 2721.
- **Present details:** `sync=0 flags=0x0`, 16-18 ms after a successful present.

So the title presents again without rendering a new scene: no camera, no upscale, no new tags. The
"unmarked new frame" reading is ruled out. Re-marking frame N attributes the present to the right
frame, yet the flash stays. That holds across all four handlings (expired tags, skipped, generated,
now confirmed as a re-present), which points at what the present shows rather than at how 2.x
classifies it. `sl.log` and the DLSS-G NGX log are silent at that moment. Still open:

- Does the unbridged 1.x game flash too?
- Which displayed frames are dark: the generated group, or the real frame?

(`unmarked present #1` at FG startup did carry constants, frame 529, so the ledger is not blind.)

### Absorbing the re-present

The unbridged comparison (`20261001_142552`, `streamline_upgrade=false`, the game's 1.x 2x FG) showed
no flash. Two facts settle the handling:

- **DLSS-G reads its inputs at present.** The 2.x guide says tagged buffers "are used during the
  `Swapchain::Present` call". At the re-present the title is already rendering N+1 into depth, motion
  vectors and HUD-less colour; the ledger only sees Streamline calls, which come late in a frame. Any
  handling that lets 2.x DLSS-G see the present lets it read half-written inputs, or resets its
  history.
- **The interposer runs every plugin's before-present hook.** In 2.x
  `sl.interposer/dxgi/dxgiSwapChain.cpp` calls them by ascending priority (sl.common 0 is always
  first; sl.dlss_g is 1000; sl.nvperf also hooks but is not loaded), whatever `skip` says. It calls
  the base `Present` only when nothing set `skip`.

So the guard also hooks the 2.x `sl.dlss_g` `slHookPresent(1)` (via that plugin's own
`slGetPluginFunction`). `PresentMarkerLedger::ClassifyPresent` returns `kAbsorb` when all of these
hold: the title sent no PRESENT_START, DLSS-G generates, nothing new arrived since the last present
(`IsRePresent`), it is the first unmarked present after a marked one, and all four present hooks
are installed. sl.common's hook then returns before `presentCommon` (no frame counted, so Reflex
stays in step without a re-mark) and sets `skip`. sl.dlss_g's hook, next on the same thread, returns
without running (`t_absorbingPresent`). Nothing reaches DXGI. sl.common's after-hook still runs;
it only recycles frame-based tags, which the bridge does not use.

Other cases are still re-marked as before: a present with new constants, a second unmarked present in
a row (a title that stopped marking must not freeze), or missing hooks. The log reads
`absorbed it (a re-present ...) after frame N`.

**Stale-risk:** this assumes the title queries `GetCurrentBackBufferIndex` before rendering its next
frame. DLSS-G hooks that call, and its index does not advance for an absorbed present. 2.x DLSS-G
fails with `eDLSSGStatusFailGetCurrentBackBufferIndexNotCalled` for titles that never call it, and W3
never logged that. A misordered frame right after an absorbed present would mean the title caches
the index.

**First absorb run (`20261001_144612`, 0.1.6895): it never engaged.** The guard logged
`could not hook 2.x sl.common slHookPresent1 at 00007FFF56028FA0`, the same address as
`slHookPresent`. Both bodies are `presentCommon(Flags, swapChain); return S_OK;`, so the linker folded
them; the earlier guard runs had the same `slHookPresent1=0` and simply covered both paths through one
hook. Without both hooks the guard fell back to re-marking, and the flash showed again (`unmarked
present #2`: a pure re-present of frame 1593). The fix: when both names resolve to one address, a
single signature-agnostic detour (`HookedSlHookPresentShared`) serves both. It forwards only the
four register arguments and never writes the fourth, which is `bool& skip` for Present and
`params` for Present1. The `skip` write moved to sl.dlss_g's detours, and absorbing requires those
two entry points to be distinct.

**Second absorb run (`20261001_145325`, 0.1.6896): no flashes, one long frame per absorb.** All four
in-game re-presents were absorbed (`absorbed it ... after frame 1393/2681/2899/3330`), and the
8-present CSV groups are gone. But each absorb lines up with a group arriving 4-7 ms after the previous
one instead of ~27 ms, followed by a ~45 ms gap (the CSV spacing 37.23 s / 6.30 s / 12.46 s equals the
absorb spacing). On screen that is one ~30 ms hold: the 10 s pacing windows with an absorb had 0.1% lows
of 31.6/34.4/55.8/77.9 fps against 80-97 elsewhere. The re-marked handling (`141737`) had shown a
smaller dip (85.3). Open question: does the absorbed call returning at once (a forwarded present
would sit in DLSS-G's hook) let the title start N+1 early? 0.1.6897 logs `absorbed-present timeline`
(`streamline_bridge_present_timeline.h`): every title call, Reflex sleep entry/return, present and
DLSS-G hook return, from the present before the absorbed one until two presents after it.

**Timeline run (`20261001_150639`, 0.1.6897): the absorb costs no time; the early frame comes before it.**
Two in-game absorbs (frames 790 and 3823), both with the same shape (ms from the absorbed present):
`-18.9 PRESENT[fwd]` (frame N) ... `-13.1 sleep-ret, simS(N)` (the frame index did NOT advance),
`-9.0 sleep(0)`, `+0.0 PRESENT[ABSORB]` (no SL calls at all, ~9 ms after that sim like a normal
render), `+15.8 sleep-ret, simS(N+1)`, `+29.4 PRESENT[fwd]` (N+1). DLSS-G's hook returns within 0.1 ms for
forwarded presents too, so the "absorbed call returns at once" hypothesis is wrong. In the CSV the
groups run 29.3 / **9.4** / 47.6 / 29.9 ms: frame N's group came ~19 ms early, and the re-present sat in
N's regular slot; N-1 to N+1 is the normal two slots (57 ms). The user notes W3 stutters on its own too.
Open question: what released frame N's sim early (a short Reflex sleep, a skipped sleep, or the title).
0.1.6899 (dbe816f5) starts the timeline three presents before the absorbed one. It also records only the
outermost DLSS-G return, because sl.dlss_g's slHookPresent runs its own hooked slHookPresent1 and
every present logged two `dlssg-ret`.
