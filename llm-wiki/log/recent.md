# llm-wiki Log

### 2026-08-21 - sl.log answers it: the game's first D3D12 device is a throwaway

`20260821_163534` (0.1.6215) reached the render loop and the swapchain, then crashed in the same
place as the first bridged run - `Bridged_slSetConstants -> sl_interposer!slSetConstants+0x49 ->
0x0`. Streamline's own log, added the run before, names the cause outright:

```
d3d12Device.cpp:396[Release]   Destroyed D3D12Device proxy ... ref count 0
pluginManager.cpp:1331[initializePlugins] D3D or VK API hook is activated without device being
                               created, did you forget to call `slSetD3DDevice`
sl.cpp:1115[slGetFeatureFunction] 'kFeatureDLSS_G' has not been initialized yet.
```

**The Witcher 3's first D3D12 device is a capability probe it throws away.** Created through the
bridge at +1.9 s, proxied by Streamline, released at +2.3 s at ref count 0; the device it actually
renders with arrives seven seconds later. CE had marked the runtime ready on that first device, so
when the real one came through `SetV2RuntimeDevice` early-returned "already done",
`slSetD3DDevice` was never called with it, and the gate that exists to prevent this exact crash
waved the call through because CE had told it a lie.

Two rules, both generalising past this title:

- **Readiness is Streamline answering `slGetFeatureFunction`, never anything CE infers.** Not "we
  called slSetD3DDevice", not "the interposer created a device" - both were tried, both produced
  the same null call. `slGetFeatureFunction` returns a pointer out of the very plugin context
  whose absence makes `slSetConstants` jump through null, so it is not a proxy for the condition,
  it is the condition.
- **Hand over every distinct device, not the first.** A device is an action CE takes, never a
  conclusion CE draws. The interposer's device (Streamline's proxy, at the documented moment)
  wins over CE's queue-derived native one, and a later interposed device supersedes an earlier -
  which is what a throwaway probe requires.

The probe is event-driven: an epoch bumped when a device is handed over and when the game's frame
index moves, at most one `slGetFeatureFunction` per epoch, none once the answer is yes. A frame
boundary is a real state transition - it is what reaching the render loop looks like, and when
Streamline finishes bringing DLSS-G's context up around the swapchain - so it converges without a
timer.

**Reflex now translates too, and a phantom field went with it.** DLSS-G does not engage with
Reflex off, so refusing `slSetFeatureConstants(Reflex)` - which the bridge did from the start -
would have left frame generation configured and inert. Re-reading the measured payload from
`20260821_042540` shows the 1.x `ReflexConstants` is 8 bytes: `mode`@0 = 1, +4 always 0, and
from +8 the captures disagree with bytes that read `00 46 00 00 f6 7f 00 00` - a `0x00007ff6....`
module address straddling +8 and +12, i.e. a caller's saved pointer. The earlier "frameLimitUs@12
= 565" was that stack tail, which is precisely what the probe's own documentation warns about and
what was not heeded the first time. **A field that is only ever non-zero in captures where it
disagrees with itself is not a field.** Only `mode` is carried; the rest keeps 2.x defaults.

The mirrored 1.x structures moved to `streamline_bridge_v1_abi.h` - a different kind of claim
from the code that calls a documented 2.x API, and the translation unit had reached the size
ceiling anyway.

Corrections to the previous entry, from the same log: `featuresToLoad` **is** honoured (`Ignoring
plugin 'sl.deepdvc' since it is was not requested by the host`) - what was observed earlier was
Streamline probing each plugin's config and unloading what it does not need. And the
`20260821_161620` startup C++ exception did not recur; unexplained rather than fixed.

### 2026-08-21 - Second bridged run: the device reaches 2.x, and CE was binding it twice

`20260821_161620` (0.1.6212) confirmed both fixes from the previous entry and gave the cleanest
state so far: 15/15 slots taken over, the 1.x runtime shut down, `leaving sl.interposer.dll
unhooked` for the superseded module, CE's hooks landing on the 2.x interposer instead, Streamline
2.12.0 up, and `the game's D3D12CreateDevice reached the CE-owned 2.x runtime`. So the game's
device creation does go through `sl.interposer!D3D12CreateDevice` and does reach the bridged
runtime.

Two defects left, both mine:

**The readiness probe vetoed a direct answer.** `slSetD3DDevice(...) returned sl::Result=0 and the
runtime still reports no device` - the confirming `slGetFeatureFunction` probe had been made the
authority over the call's own return code. A probe failure proves nothing (it also fails while the
DLSS plugin is still coming up), so it may only ever confirm, never veto.

**CE bound the same device twice.** The 2.x interposer created and bound the device inside the call
CE was returning from, and CE then called `slSetD3DDevice` on it again - documented as "NOT thread
safe and should be called IMMEDIATELY after main device is created", issued from inside that very
creation, and through CE's own inline hook on that export. Streamline offers interposed creation
**or** `slSetD3DDevice` for a host that made its own device; doing both is a second bind, not a
belt-and-braces. The bridge now marks the runtime ready without calling anything when the
interposer created the device, and keeps `slSetD3DDevice` for the Agility SDK route
(`ID3D12DeviceFactory::CreateDevice`) that Streamline never sees.

Also: the device gate is now a plain atomic read. The version that asked the runtime on every call
would have put a `slGetFeatureFunction` - CE inline-hooks that export on the bridged interposer, so
it re-enters CE's own Streamline layer - in front of every tag and constant call on the render
thread, for as long as the device was missing.

**The run still ended in an unhandled C++ exception `0xE06D7363`**, seven seconds after the last
Streamline interaction, before the game presented and before it made a single feature call.
Unattributed: `ntdll!RtlUserThreadStart` caught it, so the throw site was fully unwound before CE's
pre-termination hook ran and the dump holds no trace. This game has a documented history of exactly
this exception shape at startup that the user reproduced with CE not injected at all
(`20260820_142322`) - which is a reason not to assume it is the bridge's, and equally not to assume
it is not.

Two changes exist to settle it next time. **`sl.log`, verbose, into CE's session directory at trace
log level** - CE's log says what CE did, not what Streamline made of it, and NVIDIA's own account of
plugin loading, device binding and feature init is the missing half. And **the hand-mirrored
`sl::Preferences` is gone**, replaced by the SDK's own struct: it had already been wrong once
(`BaseStructure` puts `next` at 0 and `structType` at 8, the reverse of how the declaration reads),
re-verifying it recurs every time the staged SDK moves, and the header is on the hook DLL's include
path anyway. The same argument that put the real `sl::Constants` behind the translation.

Noted for later: Streamline loads every plugin in the staged folder regardless of `featuresToLoad`
(`sl.deepdvc`, `sl.directsr`, `sl.dlss_d`, `sl.nis`, `nvngx_dlssd` at 40 MB), about 3.7 s inside the
game's first bridged call. Successive plugins load at the same base address, which is consistent
with SL2 probing each plugin's config and unloading what it does not need.

### 2026-08-21 - The bridge runs: takeover and quiesce work, and the 2.x runtime had no device

`20260821_155250` is the first session in which any of the generation bridge executed, and it
settles the parts that were guesses. The takeover took all 15 import slots, the staged 2.12.0
runtime came up, and the 1.x quiesce did what it was designed to - every `sl.*` module left in
the process afterwards is the staged 2.x one, with only the game's statically imported
`sl.interposer.dll` still mapped and never called again. So "the game loads no more Streamline
DLLs from its own directory" is reached, by unloading them rather than by beating `slInit`.

Then it crashed, and the crash named the next two problems exactly.

**The 2.x runtime had no device, and CE called it anyway.**

```
0xC0000005 at 0x0000000000000000, RIP=0
capture_hook_x64!Bridged_slSetConstants -> sl_interposer!slSetConstants+0x49 -> 0x0
```

Streamline 2.x's exports are forwarders into a plugin the manager binds at `slSetD3DDevice`;
before that they do not return an error, they jump through null. The same run shows the quiet
half of the same cause - the feature entry points never resolved, because `slGetFeatureFunction`
needs the device too, so DLSS and DLSS-G were being refused for a reason that would never have
stopped being true. `sl_core_api.h` states the requirement per function; `slIsFeatureSupported`
is the one call that legitimately answers early, and it did.

Three things now supply and confirm the device: the bridge's own `D3D12CreateDevice` slot calls
`slSetD3DDevice`; CE's DX12 hook calls `NotifyD3D12Device` when it derives the device from a
command queue (the route that covers an Agility SDK title, whose device comes from
`ID3D12DeviceFactory::CreateDevice` and never touches an sl.interposer export); and the gate
**asks the runtime** rather than trusting either - `slGetFeatureFunction` succeeding is
Streamline's own answer to "do you have a device". That last part matters in both directions: a
runtime that bound the device through its own interposer is recognised without CE having done
anything, and an `slSetD3DDevice` that errors because the device was already set is not mistaken
for failure. Until the answer is yes, every device-dependent call is refused.

**CE was hooking the module nothing calls.**

```
Streamline Hook: sl.interposer.dll speaks Streamline 1.x - CE installs only the hooks ...
Streamline Hook: registered the Streamline 2.x slSetTag/slEvaluateFeature dynamic routes
Streamline Hook: Inline hook installed for slSetTag at <1.x address>
Streamline Hook: Refusing to retarget slSetTag from <1.x address> to <2.x address>
    - the installed target is still mapped
```

A 2.x-shaped hook landed on the inert 1.x image - the truncation the generation gate exists to
prevent - and then held CE's single forward pointer per symbol, so the hook on the runtime that
actually runs was refused as a duplicate. CE would have watched a dead module while the live one
went unobserved: `dlss_fg_factor`, `dlss_fg_preset` and the overlay's FG state machine, blind.
`StreamlineModuleSupersededByBridge` now leaves a bridged-away 1.x module entirely unhooked. The
generation alone identifies it - the bridge only activates for a 1.x process with a 2.x runtime,
so while it is active any V1 Streamline module is by construction the superseded one.

Also: per-symbol creation logging instead of one shared latch (it reported only
`CreateDXGIFactory1`, the first to arrive, and said nothing about `D3D12CreateDevice`), and
`ResolveFeatureFunctions` now logs a PARTIAL resolution instead of staying silent - a plugin
missing from the staged folder looked identical to a deviceless runtime, and those need
different fixes.

Not yet reached: a working DLSS or DLSS-G through the bridge. Nothing past `slSetConstants` has
executed successfully.

### 2026-08-21 - The bridge refused every time, and the deadline it was held to was unreachable

Two Witcher 3 sessions with the translation shipped, `20260821_151738` (`streamline_upgrade=false`)
and `20260821_151924` (`true`), and the bridged one never translated a single call:

```
Streamline bridge: not activating - the game already drove its own Streamline runtime
```

**The gate was checking a condition CE structurally cannot beat.** `123.exe` imports its D3D12 and
DXGI entry points *from `sl.interposer.dll`*, and neither the executable nor the interposer imports
`d3d12.dll` - checked with a PE import dump, not assumed. The only module that pulls `d3d12.dll` in
is `sl.common.dll`, via its own import table, and that loads from inside `slInit`. CE's delayed
injection waits for `d3d12.dll`. So the signal CE injects on and the deadline it was being held to
are the same event, with WMI notification (`WITHIN 0.5`), a 114 ms config reload and a ~380 ms
remote-thread `LoadLibrary` in between. `20260821_151924` shows it plainly: `d3d12=1` on the first
poll, meaning `slInit` had already run before CE was notified the process existed.

`20260821_151738` shows the other half - the part CE owns. There CE *was* early enough (DllMain
15:18:11.99, its LoadLibrary hooks live by 15:18:12.27) and then spent until 15:18:12.77 on
unrelated hook installation, ~400 ms of it in `FatalExitDump` quiescing peer threads, before even
evaluating the bridge.

Both were fixed, and neither fix is a race won:

- `TryActivate()` moved to the **first** thing the hook thread does with a loaded config, and the
  takeover was reordered to precede the 2.x bring-up. Repointing import slots is memory writes;
  `LoadLibrary` + `slInit` is hundreds of milliseconds. Doing the cheap half first means the game
  cannot get past CE at all, and a call arriving mid-bring-up blocks in `std::call_once` instead of
  racing. A failed bring-up forwards every call to the 1.x export its slot held, so "no bridge"
  means "exactly what would have happened unbridged".
- **The deadline moved to where it actually is.** `slInit` is recoverable - CE takes the imports
  over and shuts the 1.x runtime back down through the `slShutdown` slot it just saved. Device
  creation is not. The margin is enormous: in `20260821_151738` the 1.x core was resident by
  15:18:12.3, its feature plugins (`sl.dlss_g`, `sl.reflex`, `sl.dlss`) did not load until
  15:18:13.4-13.9, and the game's real swapchain not until **15:18:29.1**. Everything the old gate
  refused to prevent happened *after* the point it refused at.

The quiesce runs only from a call the game makes, never from CE's hook thread: reaching a CE thunk
proves the game has returned from whatever 1.x call it was in. It also has to precede the 2.x
bring-up, because both plugin sets carry the same base names and CE's Streamline hooks key on those.

Also fixed while reading the translation against the real 2.x header: `slEvaluateFeature` was
passing `inputs=nullptr`, so every evaluation silently used viewport 0 while the constants had been
set on the game's `id`; and the feature-function lookup sat behind `std::call_once`, which latches
the nulls it gets when asked before the device exists - `slGetFeatureFunction` documents that
requirement - and would then have refused DLSS and DLSS-G forever.

Two diagnostics were added because the refusal named a decision without naming the state behind it:
a Streamline module inventory (name, version, full path) at the takeover, the refusal, and after the
quiesce; and a record of whether device/factory creation actually reached the 2.x runtime, since
whether the game made its device before injection is unknowable in advance but perfectly observable
afterwards.

`streamline_bridge.cpp` split: the 2.x bring-up moved to `streamline_bridge_runtime.{h,cpp}`, along
the same seam - slot ownership on one side, the slow load-and-init on the other.

Still not validated in a game. The next `streamline_upgrade=true` run is the first that can reach
the translation at all.
