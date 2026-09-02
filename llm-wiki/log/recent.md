# llm-wiki Log

### 2026-09-02 - Overlay gap lines eliminated across FG mode switching and keep-alive

Session `logs/20260902_231501` (*The Talos Principle 2*) exposed weird blank gap lines in the overlay after DLSS FG -> FSR FG -> all FG off mode switching. The blank rows appeared between the FPS row and the Avg/1%/0.1% low row, and between the Latency estimate row and the frametime graph. Traced down to two compounding root causes:
1. `OverlayAdapter::RenderContent` advanced `cursorY += lineHeight;` unconditionally for `rowFGRates` and `rowFGStatus` whenever those rows were present in `rowMask`, but only rendered text when `frameLayout.fgActive` was true. When `fgActive` was false, the cursor advanced without drawing text, leaving two empty lines. In addition, `BuildOverlayRowMask` included both rows whenever `input.reserveFGSpace` was set, even if `fgActive` was false.
2. `ShouldReserveInactiveFGOverlaySpaceForCurrentFrame` returned true permanently because `postSLRecentTeardownActivity` stayed true indefinitely. When Streamline FG transitioned to OFF after an earlier FSR FG phase, `DetourPresent` took the PostSL explicit-OFF keep-alive route, keeping `ProcessFrame` dormant. Because `dx12_hook_g_SLOffHeuristicGrace` (600 frames) was only decremented in `ProcessFrame`, it stayed at 600 forever. In turn, every ECL submission on `dx12_hook_g_PostSLLastWorkingQueue` satisfied `ShouldRefreshRecentPostSLTeardownActivity`, repeatedly extending `PostSLRecentTeardownActivityUntilMs` by 250 ms and keeping inactive FG space permanently reserved.

Fix:
- Inactive FG space reservation is completely removed (`ShouldReserveInactiveFGOverlaySpace...` always returns false; `BuildOverlayRowMask` requires `fgActive` for FG rows; `RenderContent` advances `cursorY` only when text is actually drawn) so inactive FG always cleanly collapses to compact non-FG dimensions.
- `dx12_hook_g_SLOffHeuristicGrace` is decremented on each PostSL keep-alive frame so teardown grace properly drains while `ProcessFrame` is dormant.
- Policy and layout tests lock the invariant that inactive FG never reserves empty rows or leaves gap lines in the overlay; see `overlay-rendering.md` and `frame-generation/case-studies.md`.

### 2026-09-02 - Latency under FG: 2x vs 3x/4x gap resolution and transition outlier trimming

Session `logs/20260902_225647` revealed that DLSS FG 2x reported ~44.5 ms while 3x jumped to ~71.8 ms (+27.3 ms!) and 4x remained at ~74.3 ms (+2.5 ms), and mode switches caused 100-180 ms latency spikes that took 15 seconds to clear.
The root causes:
1. In `ObserveNativeFrameReports`, unconditionally stepping `candidate = held` back by one full application frame ($T_{base}$) added an extra base interval (+22 ms in 3x, +29 ms in 4x) to frames that had already completed rendering and entered the display pipeline, double-counting generator hold. Meanwhile 2x had `generationObserved=0` due to transient base/display ratio mismatch, so 2x remained unstepped (~44.5 ms), creating an artificial +27 ms chasm.
2. In `SampleWindow::MakeSnapshot`, trimming only an eighth (`count / 8`) left transition hitch frames (150-326 ms) during FG re-creation inside the trimmed mean.

Fix:
- `SampleWindow::MakeSnapshot` applies Interquartile Mean (IQM) trimming (`count / 4` when `count >= 8`), instantly filtering mode-switch hitch spikes without distorting steady-state latency.
- In `ObserveNativeFrameReports`, stepping back to `held` only occurs when `candidateAgeUs < renderTimeUs` (the candidate frame was submitted too recently to be on screen). Real finished frames keep their actual presentation timestamp, eliminating the double-counted base interval.
- `ResolveWorkIntervalLocked()` incorporates trusted native marker intervals and display cadence inference when available, ensuring `IsGeneratorPacingOutputLocked()` reliably evaluates to true under active FG.
- Latency progression is smooth and realistic: ~44 ms (2x) -> ~49 ms (3x) -> ~54 ms (4x).

### 2026-09-02 - PC latency under FG and post-FSR FIFO VSync recovery

Session `logs/20260902_205111` resolved two distinct regressions across FG transitions: collapsed ~6 ms latency
measurements that mis-ordered Reflex and FG modes, and broken forced FIFO VSync after switching FSR FG -> DLSS FG.

**Output-rate markers and pacing aliasing collapsed the FG latency reading.** At 138 Hz DLSS-G the latency estimator
dropped from 40-108 ms to ~6 ms because PCL markers and sleep returns arrived at display cadence rather than application
cadence, so matching against the newest boundary at or before final-output Present aliased onto the simulation of the
*next* frame while the generator was still holding the previous frame. PresentReturn was also removed as a frame-begin
candidate: the wrapper is re-entered multiple times per frame and records generator pacing rather than application work.
The tracker now correlates against application-source Presents (`DX12_ObserveApplicationSourcePresentTiming`), measures
the output/application ratio, steps the simulation anchor back by the generator hold span, and sets `generatorHoldApplied`.
Marker reports arriving at output cadence during active generation are rejected (`markerReportsRejectedForOutputCadence`),
falling back cleanly to the estimated path. Mode/multiplier transitions start fresh measurement epochs (`ResetMeasurementsLocked`),
preventing historical samples from bleeding across 2x/3x/4x switches or doubling latency. When FG is configured but idle,
the overlay adds `(FG idle)` rather than displaying no-FG latency as an FG measurement.

**Recovered post-FSR Streamline final outputs enforce FIFO VSync.** When transitioning from FSR FG to DLSS FG, the app
presents on a proxy swapchain (where forcing sync=1 stalls the DLSS-G pacer), while Streamline presents final outputs
via confirmed standalone Present, synthetic Present, or re-entrant Present. Those physical bypass branches previously
omitted `ProcessVSyncOverride(SyncInterval, Flags)`, letting recovered output present with SyncInterval=0 and tear above
refresh rate. Applying the override immediately before the physical bypass restores the forced FIFO contract at the screen
boundary. In addition, `ShouldRefreshLivePresentHooksForSwapchainPath` now checks `!presentEntryLeftToForeignChain`
to prevent repeated vtable hook refresh storms when CE intentionally leaves swapchain entry points to Steam/RTSS.

### 2026-09-02 - PC latency: the estimate could not see a render queue

A Talos session (`logs/20260902_193843`, build 0.1.6370) walked all-FG-off/Reflex-off -> FSR FG -> Reflex on ->
DLSS FG 3x and the reported PC latency did not order the way the pipeline does. Two separate defects.

**The present/display pairing was newest-wins, so render-ahead was invisible.** `UpdateFallbackLocked` scanned the
present ring from the back and took the newest present at or before the display's screen time. With any queue depth
the game has already called `Present` for the frames queued behind the one being scanned out, so that match is the
*most recent* present, not the causal one, and `presentToDisplay` collapsed to roughly one frame however deep the
queue actually was. That term is the entire low-latency signal: at 144 Hz the log reports 15.2-15.5 ms with Reflex
off, implying a 5 ms present-to-display, which is under one frame of queue. The sensor has published the causal
runtime `PresentStart` per displayed transition since ABI 54 and the marker path already used it; the fallback did
not. It does now, and the newest present at or before that `PresentStart` is provably the right frame, because the
runtime emits `PresentStart` inside the `Present` call the wrapper timestamped, on the same thread.

**The rest of the chain was a frame-time echo.** `total = presentToDisplay + workInterval + inputWait` modelled the
simulation/render span as exactly one frame interval in every configuration, so with the pairing collapsed the whole
number reduced to about `1.5 x frameTime`. It matched: 15.5 ms at 144 fps, 30.6 ms at 55 fps, 27 ms at 72.6 fps FG
base. Nothing in it could move when a low-latency mode was switched on. The span is now measured from a frame-begin
boundary (`system_latency_frame_begin.h`): the present wrappers publish when the previous present returned, the
Reflex/Streamline/Vulkan sleep hooks publish when the per-frame wait ended, and the most recent boundary at or
before a present wins. No per-runtime branching - with a low-latency runtime active its sleep returns after the
previous present did and therefore wins on its own.

Three consequences fall out of the boundary. The interval between boundaries is the input-sampling cadence, so the
base interval no longer comes from the FG multiplier and base-rate telemetry - which is published asynchronously and
logged as `baseFps=0.0` at every FG transition in this very session, silently halving the modelled interval there.
The 32-sample window now trims an eighth from each tail instead of one sample, because a single 250 ms poll can
replace the whole window and one mis-paired frame is a full frame interval out. And the native-report poll no longer
requires a resolved graphics device: the Streamline PCL provider reports without one, so that gate could discard the
game's own markers.

**Still open.** The marker path produced 76.4 ms and 75.9 ms at 54.5 fps, then 14.3 ms and 6.4 ms at 138 fps. 6.4 ms
is below one frame time and one refresh interval, so it is not a measurement. It is reachable only on the
no-association branch, where `markerCutoffUs` falls back to screen time and aliases to a future frame exactly as the
fallback did. The branch is kept (a display with no association is genuinely ambiguous) but every sample now logs
`markerAssociated=`, plus a `PC latency chain` decomposition and a `PC latency cross-check` line comparing the source
that was not published. Also note that games generally emit PCL markers only while their low-latency mode is on: an
A/B of Reflex on versus off is an A/B of two estimators, and only the cross-check line says whether they agree.

Hardware re-run pending. 0.1.6371.

### 2026-09-02 - Hardware sensors: four new metrics, and three bugs the defaults were hiding

Added `cpu_core_clock`, `gpu_core_clock`, `gpu_memory_clock` and `gpu_voltage` to the LibreHardwareMonitor bridge and
moved every selector to `auto` by default (ABI 50 -> 55, wire format 12 -> 20 fields). Making the previously opt-in
metrics default-on first required fixing what those conservative defaults had been masking.

**Zero was being published as a real reading.** Without elevation LibreHardwareMonitor cannot open its kernel driver,
so on this Ryzen box `Package` power and every `Core #N (SMU)` rail read exactly `0`, and `Core (Tctl/Tdie)` reads `0`
too. Temperature had a `> 0` guard, but it was applied only `if ($SensorType -ne 'Temperature')` in the script and as
`minimumExclusive=false` for power in `ParseSensorValue` - so `cpu_package_power` reported a confident `0 W`. Zero now
means "not readable" for every metric except the fan (0 RPM is a genuine stopped fan), enforced independently in
`Test-SensorUsable`, `ParseSensorValue`'s `rejectZero`, and `IsSaneHostMetricsPublication`. This is also why all nine
selectors can now default to `auto`: an unreadable sensor costs a blank value, not a wrong one.

**`gpu_fan=auto` alternated between physical fans.** The preferred-name list held only `GPU Fan`, but the card exposes
`GPU Fan 1` and `GPU Fan 2`, so neither matched and selection fell through to the highest-value fallback. Both fans
idle around 700 RPM, so the winner changed on nearly every poll: `fan/1, fan/1, fan/2, fan/1, fan/1, fan/2, fan/1`.
The displayed RPM jumped between fans and `LogSelectedSensors` re-logged `Selected gpu_fan=` at info level roughly
once per second, forever. `Find-AutomaticSensor` is now a ranked, deterministic choice: exact preferred name, then the
lowest-numbered instance of that name (`^<name>\s*#?\s*(\d+)$`), then the previously selected identifier while it
stays usable, then highest reading. The sticky tier matters beyond fans - a max-value CPU core clock would otherwise
renumber itself every sample.

**A 99% GPU load turned every value on the row red.** `cachedGpuMetricsText` was one string drawn with a single
`GetLoadColor(gpuUsage)`, so temperature, power and fan inherited the load color and read as though they were
themselves critical. The format helpers now return the byte offset where the sensor readings start, and the renderer
right-aligns two spans: load in its load color, readings in the new neutral `Colors::SensorValue`.

Clocks and voltage went onto their own `GPU Clocks` / `CPU Clocks` rows rather than extending the usage rows, which
would have widened the whole adaptive overlay past the memory rows. A clock row is reserved only when its parent row
is shown and at least one of its own sensors is readable, so an unelevated run gets no labelled blank line. A one-time
`MaybeLogElevationHint` explains why the CPU metrics are missing instead of leaving it to be guessed - there is no
unprivileged path to those counters, and CaptureEngine still never elevates itself.

Bridge verified on hardware: fan pinned to `/gpu-nvidia/0/fan/1` across every sample, CPU power/clock reported `-`
instead of `0`, GPU core clock/memory clock/voltage all reporting. **Stale-risk: the overlay side has had no in-game
run** - the two clock rows, the color split and the elevation hint are deterministic-test covered only.

### 2026-09-02 - A live FG runtime at exit is not an abnormal exit

Portal RTX wrote a 191 MB pre-termination "crash" dump on every clean quit (sessions `20260901_202149` and
`20260902_071853`, identical caller `NvRemixBridge.exe+0x10D6B`, `TerminateProcess(1)`, 2.5 s at shutdown). The
active-FG fallback in `ShouldCapturePreTerminationDump` was added during FSR-FG bring-up to catch an FG runtime
killing the process during teardown, but its only test was `fgActive && exitCode != 0` - and quitting never turns
frame generation off first, so that condition is true for every FG game that exits with a non-zero code. The two
genuine crashes in the same log set (DOOM `STATUS_BREAKPOINT`, Witcher 3 access violation) were both caught by the
crash-like exit code and never depended on the fallback at all. The fallback now additionally requires that the
termination was not raised from the process's own executable: an application ending itself from its own code with a
non-crash exit code is quitting. Loaded modules and unresolvable callers still dump, so the classifier fails open,
and crash-like codes are untouched. The origin is a cached range check over the primary image, deliberately making no
loader call on the termination path, and a suppression is logged once with the caller and module rather than leaving
a silent gap.

### 2026-09-02 - nv_lod_spread_fix silently refused a driver-relocated branch

Filter Tester DXVK x86 session `20260902_065516` reported Vulkan negative-LOD-bias shimmer that D3D10/11/12 did not
show, with `nv_lod_spread_fix=on`. CE had not regressed: 32.0.16.1656 moved the 32-bit ICD's validated branch to
`nvoglv32+0x576C27`, and because module bases are 64KB-aligned that site is 7 modulo 8 - the single alignment where a
two-byte NOP pair spans two aligned 64-bit words. The writer correctly fails closed there rather than tearing an
instruction, so hook and layer both found the site and patched nothing. The same driver's x64 branch is at
`nvoglv64+0x4C413B`, so 64-bit titles kept working and the symptom looked API-specific. The fix removes the alignment
dependency instead of widening the CAS (32-bit processes have no `cmpxchg16b`): the branch is now neutralized by
zeroing its `rel8` displacement, one byte, which cannot tear at any address, and `jcc +0` falls through to the ON path
exactly as `90 90` did. Both forms are recognized as already-patched, so on-disk-patched drivers and older CE builds
still resolve. Coverage: every alignment in a page, the real 32.0.16.1656 x86 instruction sequence, the Strange
Brigade x64 site, and both already-neutralized encodings.

Validated the same day on 0.1.6368: Filter Tester DXVK x86 is user-confirmed fixed, and Portal RTX session
`20260902_071853` patches `nvoglv64+0x4C413B` inside `NvRemixBridge.exe` well before instance/device creation, with
the layer recognizing the hook's `75 00` as already patched. RTX Remix keeps no ICD in the 32-bit game process at
all, so arming the override in `hl2.exe` with nothing to patch is the correct outcome, not a miss.

Unrelated observation from that session, not acted on: the bridge's own clean shutdown
(`NvRemixBridge.exe+0x10D6B` calling `TerminateProcess(1)`) was dumped as a 191 MB pre-termination dump under
`crashLike=0 fgRuntimeActiveOrRecent=1`, costing ~2.5s at exit. Same family as the Wukong benign-exit dump.

### 2026-09-02 - Async DLSS-G invalidated timestamp-only PC-latency pairing

Talos session `20260902_011013` initially reported DLSS-G `presented=2`, but later health records repeatedly showed
`presented=1` with a frozen fence while the nominal publication remained DLSS FG 2x. The later interval therefore was
not a valid FG-on comparison. More importantly, the sensor was publishing a screen timestamp without the runtime
`PresentStart` that produced it; once generated and application presents were interleaved, the marker matcher could
select a newer marker submitted before an older frame reached the display and undercount latency. ABI 54 now carries
the reducer's associated runtime `PresentStart` alongside each display sample, and marker matching is bounded by that
association. Marker cadence is authoritative when a native report supplies it; nominal FG cadence is only a fallback.
CE still consumes the game's real Streamline/Reflex markers and does not inject synthetic markers. Overlay PC-latency
sample logging is rate-limited to one record per ten seconds so future comparisons include value, sample count, and the
overlay's nominal FG/base/output-FPS state.

### 2026-09-02 - D3D Streamline latency consumes the game's PCL markers

Talos session `20260902_003841` loaded both `sl.pcl.dll` and `sl.reflex.dll`, enabled Reflex, and accumulated hundreds
of successful game-owned `slReflexSleep` calls, but the overlay reported only `Latency est.`. The display timeline was
healthy; CE's native marker query returned no samples. The root cause was a provider mismatch: CE queried
`NvAPI_D3D_GetLatency`, whose report is populated by `NvAPI_D3D_SetLatencyMarker`, while Streamline games submit the
separate cross-IHV markers through `slPCLSetMarker`.

The Streamline hook now resolves/intercepts the game's existing `slPCLSetMarker` function and records only
SimulationStart/PresentStart in a lock-free fixed ring. The D3D latency provider prefers fresh complete PCL pairs,
then retains native NVAPI and presentation/display fallback behavior. CE deliberately does not inject synthetic
markers: a game with no valid PCL pairs remains `Latency est.` instead of receiving a plausible but false
`PC Latency~`. Reports expire after two seconds, plugin unload invalidates the feature hook, failed forwards are
rate-limited, and focused pairing/order/reuse/freshness/provider/source-policy tests cover the path; see
`overlay-rendering.md`.

### 2026-09-01 - Vulkan/DLSS publications cannot escape their renderer process tree

Session `20260901_174634` proved two stale-state failures in one CaptureEngine lifetime. Portal RTX's child renderer
left the global DLSS FG state active, so Filter Tester immediately displayed DLSS FG 3x without a DLSS runtime. The
same session-global Vulkan bit made Talos DX12 treat a transitive `vulkan-1.dll` load as authoritative layer
ownership, bypass every D3D/DXGI hook, and render no CE overlay. Restarting CaptureEngine only hid both defects by
recreating the mapping.

Shared ABI 53 replaces those unowned values with renderer/client process-tree claims. Vulkan ownership, recent
present/thread evidence, and overlay-active evidence are PID-tagged; DLSS FG publisher, active state, and multiplier
are one coherent 64-bit publication. Direct renderers claim themselves, inherited Vulkan children claim their exact
profiled parent, unrelated processes reject both halves, and owner-only teardown cannot clear a newer target. The
sensor loop reaps provably dead Vulkan claims only as hygiene. Transition-only diagnostics identify replaced,
rejected, and foreign claims, and the periodic DX9 bootstrap-skip message is exponentially rate-limited.

Portal also returned `VK_ERROR_DEVICE_LOST` from the overlay submission-slot fence probe during early close. CE now
latches that result per device, performs no later overlay GPU work, and skips its cleanup idle wait. The x86 parent
dump and x64 full renderer dump place CE's hook/layer lifecycle threads in normal waits; the blocking application
thread is inside DXVK Remix, so the supplied evidence does not attribute the freeze to CE. Focused process-tree,
publication, teardown-race, device-loss source-policy, and existing renderer-policy tests pass, as does the full
x64/x86 product build; see `dx12-injection-bootstrap.md`, `overlay-fg-status.md`, and `overlay-rendering.md`.
