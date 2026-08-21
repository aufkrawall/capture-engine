# llm-wiki Log Archive 2026-W34g

Covers 2026-08-21 (Streamline 1.x/2.x bridge activation attempts). Newest-first.

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
