# llm-wiki Log

### 2026-09-04 - In-Game Benchmark Recording and Interactive HTML Reports

Added comprehensive in-game benchmark functionality for inject overlay:
- **Configuration & Hotkey (`[Benchmark]` section & default `CTRL+7`):**
  Added `[Benchmark]` section in `config.ini.template` with options `start_delay_seconds` (countdown before recording), `duration_seconds` (auto-stop timer, or `0` for manual toggle), and `output_dir` (default: `benchmarks` folder). Added `benchmark=CTRL+7` in `[Hotkeys]`.
- **Three-Phase Cycle State Machine (`BenchmarkManager` in `hook/common/benchmark_manager.*`):**
  State machine cycles `Idle -> Delaying (countdown) -> Recording -> Results -> Idle`.
  - Press 1: Starts countdown delay (if configured) or begins recording immediately.
  - Press 2: Stops recording, calculates stats, writes standalone HTML report, and renders summary card on screen.
  - Press 3: Clears benchmark results overlay and returns to Idle.
- **IPC & Shared Memory ABI 56:**
  Bumped `SHARED_MEMORY_VERSION` to 56 (`Local\CE_SM_56_*`, `Local\CE_Disc_56`). Added `SharedBenchmarkState` to `SharedMemoryLayout` and `ProcessCommand::ToggleBenchmark = 7` to authenticated IPC protocol. Dispatched via hotkey matcher ID `HOTKEY_ID_BENCHMARK = 5` in controller and inject client.
- **Dual Timing Modes & 1% / 0.1% Low Metrics:**
  Calculates Average FPS, Max FPS, Min FPS, 1% Low FPS, 0.1% Low FPS, and Average Frame Time ms for both Presentation (`Classic Frame Start`) and display cadence (`msBetweenDisplayChange`).
- **Telemetry & Hardware Sensor Capture:**
  Captures per-frame CPU load, max core load, CPU/GPU temperatures, powers, core & memory clocks, fan speeds, core voltages, and RAM/VRAM usages. Computes averages and peaks across all active sensors.
- **Standalone Offline Interactive HTML Report (`hook/common/benchmark_html_report.*`):**
  Generates modern dark-themed self-contained HTML reports named after profile/executable with timestamp (`<name>_YYYY-MM-DD_HH-MM-SS.html`). Includes live interactive timing mode switcher, responsive KPI cards, hardware telemetry table, and HTML5 Canvas timeline chart with synchronized hover crosshair and real-time telemetry inspection HUD.
- **In-Game Overlay Rendering (`hook/common/benchmark_overlay_render.*`):**
  Renders live countdown banner, recording badge with pulsating indicator, and post-run results card directly in game overlay, dismissing cleanly on the next hotkey press.



Two overlay regressions reported after a day of measurement runs: PC latency showed no value, and
`msBetweenDisplayChange` went jagged under DLSS 4 MFG. Neither was a code change. The measurement harness had
force-killed CaptureEngine about fifty times, and **an ETW session outlives the process that created it**: each
kill left a `CE_DisplayTiming_<pid>` session running, fourteen of them accumulated, and Windows then refused a new
one with 1450 `ERROR_NO_SYSTEM_RESOURCES`. Screen-change timing fell back to presentation timing, which is a
present cadence rather than a screen cadence - a sawtooth under frame generation - and the PC-latency estimate,
whose only working source here is `presentation/display estimate`, lost its pairing and read `source=unavailable`.

The evidence was one line in `sensors.log`: `Screen-change timing startup failed: 1450`. Session
`20260903_220337` still had `source=presentation/display estimate value=12.1ms`; every session after it read
`unavailable`, which is when the count crossed the machine's limit.

`captureengine/display_timing_session_reclaim.*` now sweeps before opening: it enumerates ETW sessions, keeps only
names matching `CE_DisplayTiming_%08X`, and stops the ones whose owning process is gone - never a live owner's,
never a stranger's. The sweep runs on every service start, so the leak cannot accumulate at all, and again if the
open is refused anyway. The 1450 case also names its own cause in the log now instead of printing a bare number.
Validated on hardware: with one leaked session present, the service logged `Reclaimed 1 leaked screen-change trace
session(s)` and started.

**Two things worth keeping.** `QueryAllTracesW` writes each session's logger name *and* log file name into the
buffer it is handed, so a buffer sized for our own short session name is undefined behaviour rather than a failure;
size it `sizeof(EVENT_TRACE_PROPERTIES) + 2 * MAX_PATH * sizeof(wchar_t)` per session. And `logman query -ets` /
`logman stop <name> -ets` work unelevated for sessions the user owns, which is how a machine in this state gets
cleaned by hand.

**Left unresolved on this machine:** after that session churn, the display-timing collector starts cleanly and
receives *no events at all* (`runtimePresents=0`), and the committed HEAD build without any of this code behaves
identically - so it is wedged Windows ETW state, not CaptureEngine, and it needs a reboot to clear. The reclaim
path itself is proven; end-to-end screen-change timing has to be re-checked once the machine is rebooted.


### 2026-09-03 - The FSR-FG cost is the GPU starving, and it hangs on one pointer store

Follow-up to the entry below, which left the 2 ms open with "the remaining candidates are the GPU work CE appends
to AMD's command lists and any queue synchronization CE introduces". Both are now measured and both are wrong, and
so was the framing: the app is **GPU-bound**, not main-thread bound. Its render thread is at 100% of a core because
it is spinning inside the frame-generation runtime's `Present`, which is not the same claim.

Three instruments were built, and each of them is worth more than the answer it produced:

- **The app measures itself.** `testapp/dx12_fg_frame_phases.h` brackets the app's own `Present`, `ffxConfigure`,
  `ffxDispatch` and `ExecuteCommandLists`; `testapp/dx12_fg_gpu_timer.h` puts D3D12 timestamps around its own
  command list. Both read the same with and without an injected overlay, so they attribute rather than compare.
  Of the +2116 us per base frame, **+2029 is inside the proxy `Present`** and the app's own GPU work moves +209.
- **Board power at a fixed clock.** `nvidia-smi` needs no elevation and settles "more GPU work" versus "starved
  GPU" in one sample: 152 W -> 119 W at an unchanged 2.9 GHz while frames fall 29%. The PDH `GPU Engine`
  utilisation counter, by contrast, reads ~92% in every configuration and discriminates nothing.
- **A per-behaviour kill switch.** `hook/common/fg_cost_probe.h` reads `CE_FG_COST_PROBE` once and each bit removes
  exactly one thing CE does per present. Thirteen bits and two config settings were run; the overlay draw, both
  draw sites together, the whole DXGI present hook (`ProcessFrame` provably never ran), installing the FFX bridge
  at all, the ECL caller-module lookup, the Streamline-UI ECL observers, the startup block, the ECL diagnostics,
  the queue vtable hook, the device publish, the sampler detours, `[Overlay] enabled=false` and
  `[HardwareSensors] enabled=off` all recovered nothing.

**One bit recovers it**: `0x8000`, CE never storing the game's command queue in `g_CommandQueue` - 183 fps against
135.5, `Present` back from 6466 us to 4536 us. The device publish next to it recovers nothing, so it is the queue
adoption itself, through a per-frame consumer that survives every suppression above. That is where the next session
starts.

**Wall time, not cycles, for a hook that blocks.** The previous entry's conclusion that CE's hooks cost 1.4% of a
core was measured with `QueryThreadCycleTime` - which cannot see a thread waiting on a lock or a fence, and a
blocked thread is exactly what starves a GPU. `HookCpuCost` now carries wall microseconds with the forwarded call
subtracted alongside the cycles, reported as `avgWallUs`/`usPerMs` in the `[OVERLAY COST]` line.

**A real defect, found and fixed, that was not the 2 ms.** Under FSR FG the ECL detour re-ran full command-queue
registration on **1290 of 1290 submissions per second**. The fast path compares the submitting queue against the
four queues CE knows; a frame-generation runtime submits from its own internal queues, which are none of them, and
each registration re-pointed `g_CommandQueue` so the next submitter looked unknown again. Every one of those took
the global command-queue mutex, called `GetDesc`/`GetDevice` on the queue, did COM AddRef/Release on a driver
object and ran two `GetModuleFileName` calls under the loader lock - on the runtime's own submission threads.
`ShouldRegisterCommandQueueFromExecuteCommandLists` now treats registration as discovery: it runs while there is no
frame generation or no primary game queue, and never once both are true. ECL coverage does not depend on it (the
detour lives on the queue vtable, which every queue of the device shares). `DX12 DIAG: ECL timing/1s` gained
`registrations=`, 0 after the fix.

**A trap worth remembering.** Three separate probes looked like they had found the answer, and all three were the
same artefact: making the ECL detour return early also stops queue discovery, so `g_CommandQueue`/`g_Device` stay
null and a large part of CE never initialises. A probe that removes a line removes everything downstream of it too;
check what else stopped happening before believing the number.


### 2026-09-03 - CE's FSR-FG cost is not the overlay, and a frame-rate A/B cannot find it

Follow-up to the 2.6 ms/frame figure measured while fixing the display-timing sawtooth. That number came from a run
with the test app's own DXGI video-memory stress on (96 `QueryVideoMemoryInfo` per frame), vsync on so the no-CE side
sat near the cap, and `gpu_load=6000`. Cleaned up - stress off, vsync off, `gpu_load=1200` - it is **2.06 ms per base
frame**, and the ladder says where it is not: injection disabled 0.00, frame generation off 0.00, display-timing
collector 0.02, **overlay 0.04**, log level +/-0.04, 1080p instead of 4K 2.06 (identical), `ffxConfigure` once
instead of per frame 0.00. The task's premise - that the 4K overlay composite was the cost - is wrong by roughly
fifty to one.

The app turns out to be **main-thread bound**: its render thread is pinned at 100% of a core in every configuration,
so anything on that thread is paid in frames. Per-thread accounting found no CE thread in the game process and an
essentially unchanged process total (2.39 cores without CE, 2.43 with) while the frame rate fell 193 -> 137. Same
CPU, fewer frames: the main thread is spinning longer inside the FFX runtime's `Present`, which is where a
frame-generation swapchain paces itself.

Two instruments were needed, and both are worth keeping:

- **Wall time is the wrong unit for a hook that wraps a blocking call.** A forwarded `Present` blocks for pacing the
  game would have paid anyway, so it looks expensive while costing the frame nothing. `hook/common/hook_cpu_cost.h`
  measures `QueryThreadCycleTime` across the hook and subtracts the cycles the forwarded runtime call consumed, via
  a thread-local tally that nested scopes restore rather than clear.
- **A hook's own share has to be separated from the runtime's.** Timing the FFX present-callback bridge against the
  callback it wraps put CE at 2 us per output frame against the runtime's 8 us; CE's share of the game thread's
  proxy `Present` is 0 us against the 6.4 ms that call blocks.

With the forwarded call excluded, CE's own CPU in the two hooks a frame-generation runtime drives hardest is the
DXGI present hook at ~226k cycles per call (~269/s) and `ExecuteCommandLists` at ~10k cycles (~915/s) - together
about **1.4% of one core**, against a loss worth far more. So the time is downstream of CE's hooks. Resolution
independence rules out a copy or a composite; the remaining candidates are the GPU work CE appends to AMD's command
lists (the bridge writes overlay breadcrumbs unconditionally) and any queue synchronization CE introduces. Left
open, and recorded as such in `overlay-rendering.md` - guessing further without GPU timestamps would be the same
mistake the display-timing sawtooth already punished once.

Instrumentation is gated on debug logging, because two counter reads per call on the two hottest hooks in the
process is small but not free. Test-app side, `fsr_configure_every_frame` became a `[Stress]` key and a
`--no-fsr-configure-every-frame` switch; it was hardcoded on, which silently made every measurement on this app a
measurement of CE's `ffxConfigure` hook (it turned out not to matter, but that could not be known without switching
it off). Also worth knowing for future runs: back-to-back fullscreen runs need a settle delay, or
`CreateSwapChainForHwnd` returns `E_ACCESSDENIED` and the app never enters FSR mode - two runs were lost to that
before it was recognised.


### 2026-09-03 - Startup tray responsiveness and complete PawnIO uninstaller teardown

1. **Instant Tray Responsiveness & 50x Startup Speedup**:
   - *Problem*: For several seconds after launching CaptureEngine, right-clicking the system tray icon opened the context menu with a multi-second delay.
   - *Root Cause*: During deferred startup (`main_kMsgCompleteControllerStartup`), `SyncPseudoOverlayConfiguration("startup")` resolved all 29 application profiles by calling `LoadConfig` from disk for each profile. Each `LoadConfig` called hundreds of `GetPrivateProfileStringA` queries and logged dozens of lines, blocking the main thread for ~4.4 seconds before any message in the queue could be dispatched.
   - *Fix*: Replaced redundant whole-file disk parsing with an in-memory section scanner (`ParseProfileDesktopOverlayOverrides`) using `GetPrivateProfileSectionA` to parse only the relevant `DesktopOverlay.*` override keys without re-reading the 1500-line config or loading unneeded graphics/audio/hotkey modules. Added `PumpStartupMessages()` checkpoints across child process spawn and connect phases.
   - *Result*: Controller startup time plummeted from 5,842 ms to 114 ms (51x faster). Tray menu clicks now open instantly.

2. **Complete PawnIO Driver Uninstallation Teardown**:
   - *Problem*: Uninstalling PawnIO via tray context menu or `--uninstall-pawnio` failed with "PawnIO driver uninstallation was cancelled or did not complete", even when launched elevated.
   - *Root Cause*: `PawnIO_setup.exe -uninstall` expects `HKLM\...\Uninstall\PawnIO` and `InstallLocation` to exist. When absent (e.g. driver installed via INF, pnputil, or prior tools like FanControl), `PawnIO_setup.exe -uninstall -silent` exited in 19 ms without doing anything, leaving the `PawnIO` kernel service running and `IsDriverInstalled()` returning true. Additionally, if CaptureEngine's `sensors` child process was active, it held open device handles to `\\.\PawnIO`, preventing driver unload.
   - *Fix*: 
     - Added child sensor process termination before uninstallation to release open device handles.
     - Implemented direct Windows Service Control Manager teardown (`OpenSCManagerW`, `OpenServiceW`, `ControlService(SERVICE_CONTROL_STOP)`, `DeleteService`).
     - Added automatic discovery of all associated DriverStore packages (`FindPawnIoOemInfs`) via `Owners` multi-string and `C:\Windows\INF\oem*.inf` scanning, followed by forced package removal via `pnputil.exe /delete-driver <oem*.inf> /uninstall /force`.
     - Cleaned leftover registry keys and `%ProgramFiles%\PawnIO`.

3. **Seamless Elevated Restart Handover**:
   - *Problem*: Clicking "Restart as Administrator now" after installing PawnIO failed with error message "CaptureEngine is already running".
   - *Root Cause*: Two bugs combined: (1) `RestartAsAdministrator()` called `PostMessageA(trayHWnd, WM_CLOSE, 0, 0)`, but `TrayIcon::WndProc` had no handler for `WM_CLOSE`, so the message was ignored and the old unelevated instance never terminated; (2) The new elevated instance launched immediately and attempted to acquire `Local\CaptureEngine_Instance_Mutex`, colliding with the still-running old process.
   - *Fix*:
     - Handled `WM_CLOSE` in `TrayIcon::WndProc` to trigger `StartShutdownAnimation()` and `callbacks.onQuit()`, cleanly terminating the message loop.
     - Passed `--restart-from-pid=<PID>` from `RestartAsAdministrator()`.
     - In `main_entry.cpp`, if `--restart-from-pid` is present, the new instance waits via `OpenProcess(SYNCHRONIZE)` and `WaitForSingleObject` for the prior PID to fully exit before proceeding.
     - Added a retry loop (20 x 50 ms) around `CreateMutexA("Local\\CaptureEngine_Instance_Mutex")` to gracefully absorb teardown timing windows before erroring.

### 2026-09-03 - Offline PawnIO setup, system tray context menu, and local integrity verification

Replaced external package manager (`winget`) retrieval of the PawnIO kernel driver with a fully self-contained offline installer bundled directly in `plugins/LibreHardwareMonitor/PawnIO_setup.exe`.
- **Integrity & Security**: Authenticode certificate and pinned SHA-256 digest (`1f519a22...`) are strictly verified via BCrypt and WinVerifyTrust before executing under UAC elevation, preventing local privilege escalation through file tampering.
- **Packaging & Licensing**: Staged official signed v2.2.0 binary and recorded provenance in `plugins/LibreHardwareMonitor/installed-files.json`. Included full GNU GPLv2 and vendor notices in `tools/licenses/`. Build system verifies signatures and packages the installer directly in release archives.
- **Tray & UI Integration**: Replaced single-action tray click with a full context menu (`Open config`, `Install PawnIO` / `Uninstall PawnIO`, `Close`). Startup prompt offers install / not now / don't ask again.
- **Elevation & Lifecycle**: Added auto-elevation fallback for CLI flags (`--install-pawnio`, `--uninstall-pawnio`), confirmation dialog before uninstallation, and 1-click elevated restart offer after successful installation.

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
