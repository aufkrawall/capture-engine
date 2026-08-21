# llm-wiki Log

### 2026-08-22 - Streamline bridge default retry must cover null-output capability probes

Witcher 3 `20260822_003944` still ended at the same adapter-bound reset, but the expected
`null/default retry` diagnostic was absent. The fallback was incorrectly gated on `ppDevice`
being non-null; `D3D12CreateDevice` allows a null output as a support probe, and this title uses
that form on its later request.

- Run the device-lost-class null/default retry for both object-creating calls and null-output
  capability probes.
- Log the actual output-pointer value in retry/failure diagnostics; never dereference an absent
  output just to clear it.

### 2026-08-22 - Fresh Streamline bridge adapters can still be reset; add scoped default retry

Witcher 3 `20260822_003051` created the first bridged D3D12 device successfully and resolved all
feature contexts, but its later `ID3D12Device` request still returned `DXGI_ERROR_DEVICE_RESET`
with feature level 11.0—even after resolving a fresh DXGI adapter by the requested LUID.

- Keep LUID normalization, but on device-lost-class failures only (`REMOVED`, `HUNG`, `RESET`,
  driver internal error), retry once with a null/default adapter.
- Log both attempts, both adapter pointers and the feature level. This keeps multi-GPU selection
  changes visible instead of silently substituting hardware.

### 2026-08-22 - Streamline bridge adapters need a fresh LUID-matched instance

Witcher 3 `20260822_001759` proved that native `D3D12CreateDevice` alone was not enough. The first
bridged device succeeded and feature contexts came up, then the later render-device request failed
again with `DXGI_ERROR_DEVICE_RESET` while reusing the game's adapter object after short-lived
factory/proxy generations had been destroyed.

- Before calling D3D12, read the requested adapter's LUID, create a fresh OS `IDXGIFactory4`, and
  resolve an equivalent adapter by that LUID. This preserves multi-GPU intent while avoiding
  dependence on another module's adapter/factory object lifetime.
- Device failures now include requested vs resolved adapter, feature level and IID.

### 2026-08-22 - Streamline bridge: native device handoff and NGX project identity

Witcher 3 `20260821_234606` isolated two bridge failures. V2's interposer returned
`DXGI_ERROR_DEVICE_RESET` for the game's later real-device request, which made the title throw;
and V2 turned a zero application ID into its production temporary ID, disabling all NGX-backed
features.

- Bridge `D3D12CreateDevice` no longer forwards to the V2 interposer. It creates through
  Microsoft's `d3d12.dll`, unwraps a factory-adapter proxy with `slGetNativeInterface` when
  necessary (balancing that call's AddRef), and explicitly hands each distinct native device to
  V2. Explicit devices supersede one another; queue-derived discovery remains fallback-only.
- Since late activation cannot observe the game's original 1.x `slInit` application ID, CE gives
  V2 a deterministic project identity from the host path plus host version. This avoids both a
  game table and embedding the local path in the identity sent to NVIDIA.
- Regression coverage pins the stable project-ID format and source-level absence of V2-targeted
  device creation. Focused `StreamlineBridgePolicyTest.*` passes; runtime confirmation in
  Witcher 3 is still manual.

### 2026-08-21 - Inject-overlay DPI: nearest display truth, not game-window awareness

RoboCop `20260821_224340` initialized the DX12 overlay at `dpiScale=1.00` on a 150% display. Its first
Streamline-created swapchain was 2560x1440; after Alt+Tab/fullscreen recovery it became 3840x2160, and warm
DX12 resize reuse preserved the font atlas that had been rasterized for the wrong scale.

- The adapter previously called `GetDpiForWindow()`. That reports the target window's awareness-virtualized
  DPI (96 for an unaware app), not the physical display scale. This is the same trap already fixed in the
  pseudo-overlay.
- Inject adapters now query `GetDpiForMonitor(MDT_EFFECTIVE_DPI)` for `MonitorFromWindow(referenceHwnd)` and
  use the shared pseudo-overlay fallback policy. The legacy `LOGPIXELSX` path remains only for systems without
  Shcore. Initialization logs the resolved monitor/DPI/scale.
- Do not infer DPI from backbuffer dimensions: logical-to-physical resolution changes are a presentation-mode
  change, while monitor effective DPI is the stable scaling input.
- Source regression: `OverlayDpiSourceTest.InjectOverlayScaleUsesNearestMonitorEffectiveDpi`. Focused tests
  pass; fresh RoboCop runtime confirmation is still manual.

### 2026-08-21 - Three new UE5 override families, and reading the types out of the binary

Added `[UE5]` `depth_of_field`, `dlss_super_resolution` (+ `dlss_super_resolution_quality`) and the HDR set
(`hdr_output`, `hdr_peak_luminance`, `hdr_paper_white`, `hdr_ui_luminance`, `hdr_min_luminance`,
`hdr_color_gamut`). No new machinery: each is a spec in `ce::ue5_cvar::kSpecs` with its own activation, installed
by the existing validated redirect. Full contract in [ue5-cvar-overrides.md](../ue5-cvar-overrides.md), which is
also where the whole UE5 section of `graphics-overrides-and-frame-pacing.md` moved (that page had reached the
800-line ceiling).

What is worth remembering from the work itself:

- **The CVar types were measured, not assumed.** Enumerating UTF-16 literals in a shipped UE5 binary
  (`G1R-Win64-Shipping.exe`) and disassembling the RIP-relative `lea` that loads each name lands on the
  registration call: UE registers int32 CVars through the console-manager vtable slot `+0x18` with the default in
  `r8d`, floats through `+0x10` with the default in `xmm2`. That is how `r.HDR.Display.MaxLuminance` turned out to
  be an **int** while `r.HDR.Display.MidLuminance` and `r.HDR.Display.MinLuminanceLog10` are **floats** - the
  symmetric guess would have written denormal garbage into an engine global, the same failure mode
  `r.MipMapLODBias` had. The help text sits next to each literal and supplies the semantics for free
  ("nit level for 18% gray", "0: Off", "Enable/Disable DLSS SR or RR at runtime").
- **Forcing DLSS on is not one flag.** `r.NGX.DLSS.Enable=1` reaches nothing in a TSR game: UE only routes a
  third-party temporal upscaler when the AA method is TAA and `r.TemporalAA.Upscaler=1`. The measured
  `r.AntiAliasingMethod` default in that build is 4 (TSR), so the on direction also writes
  `r.NGX.Enable=1`, `r.TemporalAA.Upscaler=1`, `r.AntiAliasingMethod=2`. The off direction stays minimal
  (`r.NGX.DLSS.Enable=0`) because killing NGX would take DLSS-G with it.
- **Depth of field uses the quality CVar, not `ShowFlag.DepthOfField`.** CE classifies bit-ref objects but does not
  write force bits, so a show-flag spec would resolve and sit inert.
- **Two settings that could contradict each other now have an order:** `display_gamma=srgb` writes an SDR
  `r.HDR.Display.OutputDevice`, and that write stands down while `hdr_output=on`.
- Version-conditional: UE 5.0 (Black Myth Wukong) has no `r.HDR.UI.Luminance`; it is reported missing and skipped.
  `r.HDR.UI.Level` was not added as a fallback because it is a multiplier, not nits, and the equivalence is
  unmeasured.
- ABI: nine appended `SharedGraphicsConfig` fields, `sizeof` 384 -> 420, `SHARED_MEMORY_VERSION` 43 -> 44 (mapping
  names renamed with it). `common/config_load_ue5.cpp` was split out of `config_load_core.cpp` so the `[UE5]`
  vocabulary is one unit.
- Stale-risk: **nothing has run in a game yet.** Proven from binaries, unit tests, and the engine's own registered
  help text only. First run should check the install/verify summary for the new names, and whether forcing
  `r.HDR.EnableHDROutput` mid-session is picked up at all (the engine reads it at swapchain creation, and a
  redirect never fires its console-variable sink).

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
