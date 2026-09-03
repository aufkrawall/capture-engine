# llm-wiki Log

### 2026-09-03 - The FSR frame-time sawtooth was a flip latch, not a frame cadence

The overlay's frame-time graph and 1% low went jagged under FSR FG while the cube stayed visibly smooth
(`installed/captureengine/logs/20260903_073736`, `dx12_fg_switch_test`, 3840x2160 at 144 Hz VRR, vsync on, 2x FSR FG).
The user's instinct - that the picture was right and `msBetweenDisplayChange` was wrong - was correct.

The counters were perfect again: `runtimePresents=1394 submitAssociations=1394 published=1384 suppressed=0
regressed=0`, one sample per displayed frame. So the health line gained the thing that was missing the last time this
happened: the *shape* of the series. `publishedInterval(n=1440 meanUs=6945 stddevUs=2180 jaggednessUs=4360 p1Us=4600
p50Us=4900 p99Us=9200)` next to `runtimeInterval(n=1440 meanUs=6945 stddevUs=117 jaggednessUs=225)` - an exactly
correct mean over a 4.7 ms / 9.2 ms sawtooth, against a flat present stream of the same frames.

**4.7 ms is shorter than the panel can refresh.** That single line settles it without any further reasoning: at a
144 Hz maximum the minimum frame interval is 6.944 ms, so a series that repeatedly claims 4.7 ms is not a screen
cadence. Worth keeping as a check.

Dumping the raw driver timeline explained it. Every completion arrived as `HSyncDPCMultiPlane` with
`FlipEntryStatusAfterFlip=15` - the hardware flip queue - and those events sit at exactly two phases inside the blank
interval, 3.92 ms and 6.14 ms after the preceding `VSyncDPC`, one per interval, while `VSyncDPC` itself ran at a dead
flat 6.946 ms. The driver latches each flip a variable time ahead of the scanout that shows it, and under frame
generation the two frames of a pair become ready at different times, so the *latch* alternates while the *screen*
does not.

The NVIDIA scheduled-flip announcement, which fixed the DLSS-G case, is useless here and was measured rather than
assumed: on this path it leads the flip event by about **two microseconds**, so applying it changed the published
series by nothing at all (stddev 2187 -> 2171, i.e. noise). It is now consumed and discarded on the deferred path, so
it cannot strand on a driver thread and be applied to a later immediate flip.

The fix is `captureengine/display_timing_vblank.h`: record `VSyncDPC` per `VidPnSourceId` and round every deferred
completion onto an observed blank. Nothing models a refresh period - under VRR the blank stream follows the frame
rate (measured 116 blanks/s at 116 fps output, the clock's median period tracking 6.95 ms -> 8.60 ms), so variable
refresh is followed rather than assumed.

Two things the first version got wrong, both caught by running it rather than by reading it:

- **Below the refresh cap it dropped a third of the frames.** With the latch lead spread wider than a refresh period,
  two completions land in one blank interval and rounding maps both onto the same blank; the later frame was then
  published as a dropped frame the screen had actually shown (published `n=774 mean=12691 us` against runtime
  `n=1158 mean=8632 us`). A display shows at most one new frame per blank *and shows them in order*, so blanks are
  now claimed in completion order. After: published `n=1158 mean=8637 us`, runtime `n=1159 mean=8637 us`.
- The bound on that ordered walk was dead code - it returned on the first step every time, so nothing stopped a
  frame rate above the refresh rate from marching it forward. The walk now starts at the completion's own blank.

Measured result at the cap: `publishedInterval stddevUs 2180 -> 14, jaggednessUs 4360 -> 12, p1/p99 4.6/9.2 ms ->
6.8/6.9 ms`, sample count unchanged. The published series is now flatter than the presentation series (14 us against
115 us), which is the right way round: the screen changes on the blank grid while the presents jitter around it.

The metric now agrees with the eye in both directions, which is the real validation. At 144 fps the user sees smooth
and it reports jaggedness 12 us; at 116 fps output under a synthetic 4K load the user saw judder and it reports
p50 8.0 ms against p99 13.7 ms. Before the fix it drew a sawtooth in both cases.

Separately measured while answering "is the judder CaptureEngine's fault": at `gpu_load=6000` (50x the normal test
load, deliberately GPU-bound) CE costs about 2.6 ms per base frame - 68.3 fps base without it, 58.0 with, twice over.
That is what pushed the run off the refresh cap into the region where FSR FG's smooth ~8.6 ms present cadence lands
unevenly on the blank grid. The judder mechanism is not CE, but the frame rate that exposed it partly was. Not
investigated further here; the number needs confirming at a realistic load and in a real title before it means
anything about overlay cost.

Still an inference, flagged in the topic page: whether a latched flip belongs to the next blank (implemented) or the
previous one. It shifts every sample by one constant refresh period, so no frame time, FPS, low or graph value
depends on it - only the PC-latency estimate.

### 2026-09-03 - The FSR startup latch that outlived its swapchain

Talos, 2D menu, DLSS FG -> FSR FG -> DLSS FG a few times: the overlay vanished and never came back. The user
deliberately quit from the menu rather than returning to 3D, where it would have recovered, so the end of
`installed/captureengine/logs/20260903_070055` is the stuck state itself - 789 identical
`InitOverlaySync: ENTER (syncInit=0 ...)` triples in the last four seconds.

CE was quiesced by its own protection. An official FFX swapchain create arms a latch that suppresses every CE GPU
side effect until AMD's runtime reaches an enabled `ffxConfigure`; while it is armed `InitOverlaySync` deliberately
keeps the sync resources idle. The second FSR selection (07:01:56.610) armed it and never resolved it: the game
raised only the FFX frame-generation *swapchain* context, and CE observed no `ffxConfigure` at all after 07:01:52.633.
None of the three existing exits could fire either. The context-count exit needs `liveContexts == 0`, but a sibling
FRAMEGENERATION context outlived the toggle so the count went 2 -> 1 and stopped. The explicit-enable exit needs a
successful `slDLSSGSetOptions` enable, but DLSS-G returned through GetState (`FG state transition OFF->ON via
GetState`). So DLSS FG ran with the FSR latch still armed, and PostSL skipped every single present with
`init=1 sync=0 list=0 alloc=0`.

The fix is a lifetime correction, not another special case: a provisional latch is bounded by the thing it protects.
It is now retired when the FFX frame-generation swapchain context is destroyed (keyed on that effect id - the
process-wide FG context count was always the wrong bound), when a Streamline runtime creates the new presentation
swapchain (placed before the PostSL prewarm, which is otherwise quiesced too), and at the Streamline-FG-ON edge for
the case where neither of the other two ever happens. All three stay gated on FSR FG not being API-active, so a real
AMD pre-configure window is untouched.

Worth recording for the next reader: the published FG status was correct throughout this session. The window where
the overlay showed no FG (07:01:56 -> 07:01:59) had no FG context configured at all - the game had built an FFX
interpolation swapchain and torn it down again without ever enabling FG. The symptom was only the disappearance.

Hardware re-run pending: switch FG modes several times in the Talos menu and confirm the overlay survives, with
`Authoritative Streamline presentation ownership superseded provisional official FFX startup` or `Official FFX
frame-generation swapchain context was destroyed before its enabled ffxConfigure` in the log.


### 2026-09-03 - CaptureEngine now installs the sensor library, and offers the PawnIO driver

Two follow-ups to the native bridge. Neither adds a shipped script.

**The four LibreHardwareMonitor files are no longer user-supplied.** `tools/build/build_lhm_plugin.py` fetches the
official v0.9.6 `LibreHardwareMonitor.zip` at build time and installs the CPU/GPU closure. The verification is the
point, not the download: pinned HTTPS URL, pinned SHA-256 (the digest GitHub publishes for that exact asset) and
pinned byte size, both checked while streaming; extraction limited to four hard-coded base names with destinations
built from that constant list rather than from archive member paths, so a `../` entry cannot escape; duplicate
candidates, oversized members and non-PE payloads refused; and the three Microsoft dependencies re-checked for an
Authenticode certificate after extraction. `LibreHardwareMonitorLib.dll` is unsigned upstream, which is exactly why
the archive digest is not optional. A verification failure is fatal; an unreachable network is not, because the
feature is optional and degrades to native usage telemetry. `installed-files.json` records per-file digests so a
tampered or stale install is replaced on the next build. `tools/tests/test_lhm_plugin.py` covers all of it, including
the traversal case.

Licensing checked before shipping: MPL-2.0 permits binary redistribution provided the Source Code Form stays
available under the MPL and recipients are told how to get it (section 3.2), and it is file-level copyleft, so
CaptureEngine's own MIT sources are unaffected. The notice file now carries that statement plus the tagged source
URL, and the packaging allowlist grew to the four files. Stale-risk: the upstream MPL-2.0 and MIT license *texts* are
referenced by URL rather than included under `licenses/`, which should be closed before a public release.

**PawnIO turned out to be a second requirement nobody had documented.** LibreHardwareMonitor 0.9.6 has dropped
WinRing0 - the assembly embeds PawnIO bytecode modules only, and `PawnIO.sys` is a separate WHQL-signed install. So
shipping the library ships no kernel driver at all, which is what made bundling defensible in the first place. But it
also means the README's "run as administrator" was only half the story: this machine *has* PawnIO installed and still
read every CPU rail as exactly zero without elevation, so both are required.

`captureengine/pawnio_setup.cpp` detects the driver by its service key and, when CPU metrics are requested and it is
missing, offers installation once. CaptureEngine neither bundles nor downloads the driver - it delegates to the
Windows Package Manager (`namazso.PawnIO`) under a single UAC prompt, falling back to opening the project page. Three
deliberate choices:
- The prompt runs on its own thread. The controller loop dispatches WM_HOTKEY itself, so a modal dialog on that
  thread would swallow hotkeys for as long as it stayed open.
- `--install-pawnio` / `--uninstall-pawnio` are elevated roles of the product executable, not shipped scripts. A
  script beside the executable that installs a kernel driver under elevation is a local privilege-escalation vector
  for anyone who can write that file - strictly worse than the PowerShell bridge just removed.
- Uninstall is command-line only. PawnIO is a shared system component LibreHardwareMonitor and other tools may also
  be using, so CaptureEngine never removes it on its own initiative.

Three answers, only the last persisted (per user, `HKCU\Software\CaptureEngine`). The TaskDialog needs the v6 common
controls, so the manifest gained that dependency, with a `MessageBox` fallback if it is unavailable. Stale-risk: the
prompt itself is unverified on hardware - this machine already has the driver, so the dialog and the elevated install
path have never been exercised end to end.

### 2026-09-03 - LibreHardwareMonitor bridge is native; the PowerShell script is gone

`plugins/LibreHardwareMonitor/CaptureEngine.LibreHardwareMonitor.ps1` was the only interpreted, user-editable
executable file CaptureEngine shipped. It is deleted. The bridge is now a role of `captureengine.exe`
(`--sensor-bridge`), launched by the dedicated sensor service exactly as before: suspended, in a kill-on-close job,
with an explicit inherited-handle list, NUL stdin/stderr, the same bounded tab-delimited stdout protocol, and the same
random named shutdown event. Process isolation, the parser, the freshness rules and the fuzz target are unchanged.

How the native bridge reaches a managed library with no managed code of our own:
- Host the in-box .NET Framework 4 runtime: `mscoree!CLRCreateInstance` -> `ICLRMetaHost::GetRuntime("v4.0.30319")` ->
  `ICLRRuntimeInfo::GetInterface(CLSID_CorRuntimeHost)` -> `Start` -> `GetDefaultDomain`. `metahost.h` is absent from
  the MinGW headers, so the two hosting interfaces are hand-declared with only the slots used.
- Late binding by name through `IDispatch` does **not** work here, in either direction, and this was measured rather
  than assumed: the CLR returns `E_NOTIMPL` from `GetIDsOfNames` on `_AppDomain`/`_Type`/`_Assembly` (their dispatch is
  typelib-backed and `mscorlib.tlb` is not registered by default), and LibreHardwareMonitor's concrete hardware classes
  are `internal`, so their CCWs answer `QueryInterface(IID_IDispatch)` with `E_NOINTERFACE`. `LibreHardwareMonitorLib`
  itself has no `[assembly: ComVisible(false)]`, so `Computer` *does* have a working AutoDispatch `IDispatch` - relying
  on that would have worked for the root object only and broken on every hardware node.
- The mechanism that does work universally is `_Object::GetType` (slot 10) followed by `_Type::InvokeMember_3`
  (slot 57), which is plain `System.Type.InvokeMember` reflection and ignores COM visibility entirely. Four frozen
  mscorlib vtables carry everything: `_AppDomain` 37/38 (`CreateInstance`/`CreateInstanceFrom`), `IObjectHandle` 3
  (`Unwrap`), `_Object` 7/10, `_Type` 57. Slot numbers were read out of `mscorlib.tlb` with a `LoadTypeLibEx` dumper,
  not guessed.
- `IComputer.Hardware` is `IList<IHardware>`; the runtime refuses to marshal a constructed generic type to a COM
  interface pointer ("Generic types cannot be marshaled to COM interface pointers", HRESULT 0x80131509), so the root
  list can never cross the boundary. The bridge binds the public `HardwareAdded`/`HardwareRemoved` events to two
  `System.Collections.Queue` objects with `Delegate.CreateDelegate` (relaxed binding lets an `IHardware`-taking
  delegate bind to `Queue.Enqueue(object)`; the `CreateDelegate(Type, object, string)` overload rejects it, the
  `MethodInfo` overload accepts it) and drains them each poll. This is also strictly better than re-reading a list,
  because hot-plugged hardware arrives and leaves through the same path. Everything below the roots -
  `IHardware.SubHardware`, `IHardware.Sensors` - is a plain array and marshals as a SAFEARRAY.
- `SensorType`/`HardwareType` ordinals come from the loaded assembly via `Enum.GetNames`/`Enum.Parse`; nothing is
  hardcoded, and any `HardwareType` whose name starts with `Gpu` counts as a GPU.

Side benefits taken while porting:
- Sensor selection moved into `captureengine/sensor_selection_policy.h` as dependency-free logic and finally has
  direct unit coverage (`tests/test_sensor_selection_policy.cpp`, 12 cases): preferred-name ranking, lowest-numbered
  instance, sticky previous identifier, highest-value last resort, zero-is-unreadable except the fan, active-GPU tie
  handling, and the identifier grammar. As a PowerShell script this logic had none.
- The nine-metric wire order, maxima and zero-rejection rule are declared once in that header and read by both the
  emitter and `sensor_plugin.cpp`'s parser, so they cannot drift; a `static_assert` pins the snapshot member order.
- Dropping PowerShell also drops ExecutionPolicy, AMSI script scanning, and a plaintext script next to the executable
  that anyone could edit.

Verified non-elevated on the Ryzen 5700X / NVIDIA machine: `CE_LHM_READY 0.9.6.0`, three samples with real GPU
temperature, package power, fan RPM, core clock, memory clock and voltage, `gpu_fan` pinned to `/gpu-nvidia/0/fan/1`,
every CPU rail reported unavailable rather than zero (cross-checked against a direct LibreHardwareMonitor read: all
CPU rails really are 0/null without the kernel driver), and exit code 0 through the shutdown event. Stale-risk: the
overlay-side rendering of these values has not been re-checked in a game since the port.
