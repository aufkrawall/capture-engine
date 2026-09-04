# llm-wiki Log

### 2026-09-04 - The blank clock judges periodicity, not density

The frame-time graph was spiky under FSR FG with variable refresh below the panel's cap
(user report, Talos). Root cause: `VerticalBlankClock` could round a deferred flip
completion onto a blank only when it had *observed* that blank, so under VRR 61% of
completions kept the driver's flip-latch timestamp while 39% were moved onto a blank.
Two different quantities in one series is jaggier than either alone: published
`jaggedness=7186 us p1=800 us p99=27100 us` on a 10.7 ms mean, against `runtime
jaggedness=328 us`. An 800 us interval is shorter than the panel can produce.

Discovering that density is not the criterion took two wrong turns worth recording:

- The first design gated on *blanks per completion* (a density ratio). It was refuted by
  a run at the fixed 144 Hz cap that reported only 657 blanks for 4247 flips, where the
  previous identical run reported 4264 - the driver simply does not report every blank,
  and a density gate would have switched the clock off in the regime it works best in.
- The gate must record every attempt, not only the ones it lets through. A record fed
  only by permitted claims makes refusing self-sustaining.

The answer is `MeasureGrid`: the smallest gap in the retained window is one refresh, every
other gap must be a whole multiple of it, and the period is refined over the whole span.
A sparse-but-regular stream is extrapolated (bounded to 64 periods); an aperiodic one
yields no grid and every completion keeps its own timestamp. `BlankCadence` then gates
only the answer, over a decaying window.

Measured after: at the cap, published stddev 7 us from a latch series with 2199 us; below
the cap, published now *equals* latch (1699 vs 1699) instead of nearly doubling it, and p1
rose from 800 us to 9800 us. **Confirmed in Talos by the reporter**: jaggedness 7186 -> 2362 us,
p1 800 -> 7200 us, published within 10 us of latch across three consecutive windows. The residual under VRR is the driver's own latch jitter -
provably artifact at the cap, unattributed below it, and needing a screen-time source we do
not have. Health line gained `usableClock`, the blank-stream gap shape, and `latchInterval`
next to `publishedInterval` so the next reader can tell the screen from this service without
a rebuild. `display_timing_service.cpp` was at the 800-line ceiling, so the ETW session and
provider enablement moved to `display_timing_startup.cpp`. 0.1.6473, `--verify` green.

Note: this work was reconstructed after an earlier uncommitted version of it was lost from
the worktree. The scratchpad patch scripts and the measurement runs were what survived.

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
