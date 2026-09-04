# llm-wiki Log

### 2026-09-04 - Fix DXGI swapchain COM reference leak and Streamline driver callback crash across FG switches (logs/20260904_143301)

Analyzed and resolved four minidumps from `logs/20260904_143301` produced during rapid native/FSR/DLSS FG transitions:

1. **Root Cause 1: Leaked COM references causing `E_ACCESSDENIED` (0x80070005) on swapchain recreation:**
   - Dumps: `crash_external_swapchain_access_denied_exhausted_f3172865.dmp` and `crash_external_fatal_exit_ExitProcess_e000eacc_14cc1ec5.dmp` (PID 13328).
   - In `hook/wrappers/dxgi_swapchain_wrap_present.cpp`, `CWrapDXGISwapChain::PromoteInterfaces()` called `m_pReal->QueryInterface(IID_PPV_ARGS(&m_pReal1))` unconditionally without checking `if (!m_pReal1)`. When the wrapper was constructed around an `IDXGISwapChain1`, the constructor had already stored and AddRef'd `m_pReal1`; calling `QueryInterface` again overwrote the pointer and added a second reference while the destructor only released it once.
   - In `hook/apis/dx12_hook_swapchain_create.cpp`, `DetourCreateSwapChainGlobal` and `DetourCreateSwapChainForHwndGlobal` wrapped `*ppSwapChain` / `*ppSC` with `new CWrapDXGISwapChain` (which AddRefs), but omitted `pReal->Release()` to consume the factory's returned reference (unlike `dxgi_factory_wrap.cpp` and `dx11_hook_detours.cpp`).
   - The extra references pinned the swapchain to the HWND across mode switches, causing `CreateSwapChainForHwnd` to fail with `DXGI_ERROR_ACCESS_DENIED` (`0x80070005`) once the deep recovery was exhausted.
   - Fix: Added `!m_pReal1..4` guards in `PromoteInterfaces()`, and added `pReal->Release()` after wrapping in `DetourCreateSwapChainGlobal` and `DetourCreateSwapChainForHwndGlobal`.

2. **Root Cause 2: Streamline mid-process unload causing driver callback DEP crash (0xC0000005):**
   - Dumps: `crash_20260904_143617_972_pid21396_tid12612.dmp` and `crash_external_fatal_exit_NtTerminateProcess_c0000005_bbde10db.dmp` (PID 21396).
   - In `testapp/dx12_fg_switch_render.cpp`, `ReinitializeDX12ForFSR` and `ReinitializeDX12ForNativeOff` were calling `ShutdownStreamlineSerialized()`, which called `slShutdown()` and `FreeLibrary(sl.interposer.dll)`.
   - Freeing `sl.interposer.dll` unloaded `sl.dlss_g.dll` and freed its code pages. However, NVIDIA's driver runtime (`_nvngx.dll` and `160_E658703.bin`) remained resident in the process and retained a registered telemetry/evaluate callback pointer (`0x0000014125CADD30`) inside global `0x713df0`.
   - On returning to DLSS, `slSetD3DDevice` -> `NVSDK_NGX_D3D12_Init_Ext` checked `[0x713df0] != 0` and called into the unmapped memory, immediately triggering an access violation / DEP crash (`0xC0000005`).
   - Fix: Added `useStreamlineSwapChain` parameter to `CreateSwapChainResources` / `InitDX12` so native swapchains can be created without unloading Streamline. Replaced mid-switch `ShutdownStreamlineSerialized` calls with `ApplyReflexMode(false, ...)` so Streamline remains resident throughout the process lifetime (matching NVIDIA's programming model and real game behavior in *The Talos Principle*) and is only torn down at process exit.

3. **Regression Coverage & Verification:**
   - Added unit tests in `tests/test_swapchain_probe_lifetime.cpp`: `PromoteInterfacesDoesNotDuplicateReferencesWhenAlreadyPopulated`, `GlobalSwapchainDetoursConsumeFactoryReferenceAfterWrapping`, and `StreamlineLifecyclePreservesRuntimeAcrossModeSwitches`.
   - Full `--verify` gate passed with 0 warnings, ASan/UBSan green, all 713 clang-tidy translation units cached and clean.

### 2026-09-04 - Fix FSR FG real on-screen frame pacing stutter by prioritizing official presentCallback and eliminating extra ECL/signals on AMD presentation queue

Fixed root cause of intermittent on-screen frame pacing stutter in *The Talos Principle: Reawakened* (`Talos1-Win64-Shipping.exe`) with native AMD FSR Frame Generation:

1. **Root Cause (Route B extra ECL on AMD's presentation queue):**
   - When a foreign overlay like Steam was loaded, `DecideBelowForeignChainFSRDeepDraw` (`hook/common/dx12_overlay_policy/ffx_routing.h`) activated Route B (`TryCompositeOverlayBelowForeignChainForRuntimeOwnedFSR`).
   - Route B caused CE to voluntarily yield AMD's official, zero-overhead `presentCallback` (`ShouldYieldFFXPresentCallbackToTopmostRoute`).
   - Instead, on AMD's presenter thread inside `DetourPresent`, Route B executed a separate `ExecuteCommandLists` call directly on AMD's presentation queue (`dx12_hook_g_SwapchainQueue`) right before `CallOriginalPresent`.
   - AMD's presenter thread uses high-precision QPC timers to pace generated vs real frames. Submitting an extra command list on AMD's queue desynced AMD's pacing timer and caused GPU queue contention right before VBlank scanout. Depending on queue slack at startup, if an extra ECL pushed GPU completion past the scanout deadline by even a fraction of a millisecond, the driver's hardware flip queue slipped by 1 VBlank and permanently locked into alternating 1 vs 2 VBlank flips (~6.6 ms / ~17.7 ms alternating flips, steady state ~83.3 FPS stutter).
2. **Prioritizing official FFX `presentCallback` (Part 1):**
   - In `hook/common/dx12_overlay_policy/ffx_routing.h`, `DecideBelowForeignChainFSRDeepDraw` makes Route B unavailable (`kUnavailable`). The official FFX `presentCallback` is never yielded to an extra swapchain-queue submission; CE renders directly into AMD's provided command list (`desc->commandList`) with zero extra `ExecuteCommandLists` calls, zero extra fences, and zero presenter-thread stalls.
3. **Skipping deferred signals and overlay completion waits on runtime-owned queues (Part 3):**
   - In `hook/common/dxgi_shared_present_core.cpp` (lines 356 and 479) and `hook/common/dxgi_shared_present1.cpp`, `InvokeDX12WaitForOverlayCompletion` and `InvokeDX12FlushDeferredSignal` are now skipped whenever `HookHasRuntimeOwnedNativeFGPresentPath()` or `DXGIShared::DoesFGRuntimeOwnSwapchain()`. This prevents any deferred signals or completion queries from touching AMD's presentation queue.
4. **Regression Tests & Verification:**
   - Updated `tests/test_ffx_below_foreign_chain_policy.cpp` (`PrefersOfficialPresentCallbackOverSwapchainQueueDrawInHealthyState`).
   - Passed full `--verify` content-validated product build, native tests, Python self-tests, file-size ratchet, and ASan/UBSan validation.

### 2026-09-04 - Faithful msBetweenDisplayChange reporting and elimination of FSR FG VEH rearm overhead

Two root-cause improvements resolving FSR FG frame pacing and overlay fidelity:

1. **Faithful `msBetweenDisplayChange` without sugarcoating:**
   - The user clarified the core project requirement: the frame-time graph must faithfully reflect
     real on-screen frame pacing (`msBetweenDisplayChange`) with VRR, GPU maxed out, VSync capping,
     and uncapped FPS, across all FG modes (all FG off, FSR FG, DLSS FG).
   - In `captureengine/display_timing_policy.h`, `ResolveDeferredScreenTimes` marks unclocked completions
     under VRR as `screenTimeResolved = true`, because on VRR panels without a fixed VBlank grid the
     unrounded hardware completion timestamp is the physical display transition time itself.
   - In `hook/common/performance_metrics.cpp`, when `DisplayChange` is preferred and the timing service
     is healthy, `m_effectiveSource` selects `FrameTimeSource::DisplayChange` directly. The overlay
     no longer forces fallback to `Presentation` when the display stream has natural variance or an
     alternating sawtooth (e.g. FSR FG on VRR). Fallback occurs only when the timing service is
     unavailable, stopped, or stale (> 2 seconds).
   - `GetLastDisplayFrameTimeMs()` returns the active display frame time from the display series.

2. **Eliminated per-frame render-thread VEH breakpoint rearm overhead in FSR FG:**
   - In `hook/apis/ffx_hook_install.cpp`, `CallFfxConfigureOriginalGuarded` was pausing and re-arming
     the 0xCC VEH breakpoint on AMD's executable code page on *every application frame* on the game's
     render thread (1800 times in 30 seconds). Each call ran 4 `VirtualProtect` syscalls (acquiring
     the process-wide `MmAddressCreationLock`) and 2 `FlushInstructionCache` syscalls (broadcasting
     cross-core TLB shootdowns).
   - The one-shot disarm was only hooked for no-callback mode.
   - In `hook/apis/ffx_hook_context.cpp` (`Hooked_ffxConfigure`), as soon as the present-callback bridge
     is established (`installedPresentCallbackBridge || retainedAlreadyBridgedPresentCallback`), the
     protected `ffxConfigure` VEH breakpoint is permanently disarmed via
     `DisarmProtectedFfxConfigureVehBreakpoint("present-callback bridge established")`.
   - Completely eliminates render-thread syscall overhead and memory lock contention during active FSR FG.


### 2026-09-04 - Accept a display stream that is measurably flatter than presents even when partially unlabelled


Follow-up report against 0.1.6481 in session `20260904_095110`: switching FSR FG -> DLSS FG
still showed the "jigsaw frame time graph" briefly or intermittently.

Under DLSS FG in this session, `screenTimeShare` sat at ~492 permille (about half of completions
were immediate flips carrying announced screen times, while others were deferred/unresolved).
Because `ScreenTimeCadence.IsScreenTime` required 900 permille to select display-change timing,
the gate refused the stream and fell back to presentation timing (the DLSS-4 burst pattern,
jaggedness ~18-23 ms). Yet the display timing stream was flat (jaggedness ~450 us). Refusing a
stream because a fraction of completions lacked a provenance label threw away the true screen
measurements.

Fix: in `hook/common/performance_metrics.cpp`, `PerformanceMetrics` now computes `windowJaggedness`
(mean absolute difference between neighbouring intervals in arrival order). The display stream is
accepted if either `provenSamples` passes the 900 permille share OR `flatterThanPresents`:
`displayJaggednessUs <= allowedJaggednessUs` (with a 1.5x hysteresis band if already selected) and
both series have valid window statistics.

This correctly rescues DLSS FG when completions are partially deferred/unlabelled, while firmly
rejecting the noisy flip-latch stream under FSR FG below the refresh cap (where display jaggedness
is ~2000-4500 us and presents are ~350-540 us).

### 2026-09-04 - The screen-time gate has to follow a regime change, not average across it

User report against 0.1.6480: switching FSR FG -> DLSS FG leaves the frame-time graph
showing the presentation series (the DLSS-4 burst pattern) and it only goes flat "after a
few seconds". Session `20260904_092817` confirms it and the cause is the gate added the
same morning, not the display-timing service.

Timeline: DLSS FG confirmed at 09:29:07.081 (`postsl-first-confirmed-render`), and the
sensor's completions switch wholesale from `hsyncDpcMpo` to `immediateMpoFlip` at the
same moment - immediate flips carry the driver's scheduled-screen-time announcement, so
every one of them is a resolved screen time. The overlay nevertheless stayed on
presentation timing until **09:29:14.269**, 7.2 s later.

`ScreenTimeCadence` was a decaying counter halved at 512 samples. That is the wrong
instrument for the thing it measures: the stream does not drift between regimes, it
*switches*, and a decaying total carries the old regime into the new one. Simulated from
the steady state of a long latch-only stretch, recovery needs about 830 resolved samples,
which is 10 s at the 83 fps that session was running - matching the 7.2 s observed once
the mixed handover window is accounted for. It is now a window over the last 128 samples
(two `uint64` words plus a running count), so recovery completes in at most one window,
about a second of frames, whatever the stream did before it. The 90%/50% hysteresis is
unchanged, and `kMinimumSamples` drops 64 -> 32 because the window is smaller.

Deliberately not done: resetting the cadence on an FG-type change would make recovery
instantaneous, but it would also show display-change timing optimistically for the first
32 samples after *every* switch, including switches into a regime that cannot resolve.
Presentation timing is never wrong, only less informative, so a short delay in that
direction is the benign failure and a short burst of latch times is not.

**What the same session says about the gate's verdicts, which are correct.** Under DLSS FG
(windows 09:29:23 and 09:29:33, all `immediateMpoFlip`): published/screen
`stddev=661/925 us jaggedness=473/650 us` against runtime `PresentStart`
`stddev=11457/11670 us jaggedness=22772/23088 us`. That inversion is the whole point of
the collector - Streamline issues a generated group of presents in a burst and the screen
consumes them evenly - and it is exactly the "jigsaw" the user was seeing while the gate
still had them on presentation timing. Under FSR FG in the same session every completion
was `hsyncDpcMpo` with `usableClock=0`, so there was no screen-time series to show and
presentation timing is the honest answer. Both verdicts were right; only the latency of
the second one was wrong.

Also fixed: a suppressed source-transition log left `lastObservedFrameTimeSource` stale,
so every later comparison ran against a source that was no longer current and a flapping
stream reached the log as unrelated one-off lines. The observed source is now recorded
whatever the rate limit decides, and the line carries `suppressedChanges=`.

### 2026-09-04 - The single-frame hold was a Reflex-on assumption

User rejection of the previous result, and correctly: at matched cadence FSR FG (Reflex off)
read 62-65 ms against DLSS FG (Reflex on) at 66-73 ms, and a generator without a low-latency
mode cannot be *faster* than one with it. The expected separation is 10-25 ms the other way.

Session `20260904_042922` pins the configuration exactly - `ReflexLimiter: Game ACTIVATED
Reflex (via Streamline)` at 04:29:36.644, `DEACTIVATED` at 04:30:04.852 - so Reflex is on
only while DLSS FG is on, and every FSR-FG and no-FG window runs it off.

Root cause: `MatchApplicationPresentLocked` stepped the anchor back exactly one application
frame. That step is the *interpolation hold* - the generator must hold a complete source
frame to interpolate toward - and the code's comment argues exactly that. It is not the
game's queue depth. The two are the same number only when a low-latency mode pins the queue
to one frame, which is precisely what Reflex does and what FSR FG here has nothing doing.

The queue is real and saturated: CE's own `[OVERLAY COST] FFX proxy Present` telemetry
measured `runtimePresentAvgUs=4857-9163` - AMD's proxy blocks the game thread for 5-9 ms of
every ~22 ms application frame. That is back-pressure from a full queue sitting *above* the
DXGI present, where `presentToDisplay` cannot see it (2.5-4.2 ms under FSR against 17-25 ms
under DLSS, at the same cadence). Same blindness as the previous fix, one level up.

Fix: count the in-flight application frames by conservation over both streams and step back
that many, floored at one. Timestamps cannot answer which frame is on screen without being
circular; conservation can, because every application frame is displayed exactly
`fgMultiplier` times. The count needs a known-empty seed - FG switching on, or an
application-present gap over 250 ms - and is dropped on any evidence the display stream was
incomplete, including a new `NoteDisplayStreamGap()` that `ConsumeDisplayTiming` raises when
it skips publication sequences.

Unit topologies confirm it is exact and linear: measured depths 2/3/4 report `appQueue=2/3/4`
and 73.5/94.5/115.5 ms - one application interval per queued frame, nothing else moving.

`system_latency_metrics.h` was at 799 of 800 lines, so the marker path
(`SubmitNativeReport` plus its three native-only helpers) moved to
`system_latency_marker_reports.h` as out-of-line inline members, mutually included and guarded.
618 lines and 210.

Hardware run pending. What to read: `appQueue=` in the chain line under FSR FG, and whether
the DLSS FG cross-check still agrees within a few ms - under Reflex the depth must measure 2,
which reproduces the previous step exactly, so a moved DLSS value means the count is wrong.

Open, and separate: without FG the log reports 6.1 ms with Reflex against 36-43 ms without,
30+ ms apart. That is a wider separation than the user's own 10-25 ms estimate of the real
on-screen difference, and 6.1 ms rests on Talos's own PCL markers reporting a 1.9 ms
simulation-to-present and a 0.4 ms present-to-flip. Whether the marker path is under-reading
there is unexamined.

### 2026-09-04 - The overlay's FSR-FG frame-time variance was the flip-latch clock

User report: frame pacing under FSR FG in Talos is "sometimes worse, sometimes better"
regarding constant micro stutter / frame-time variance. Two sessions of the same build
(0.1.6475), same title, same settings, six minutes apart: `talosfsrfgbad` and
`talosfsrfggood`. Nothing in CE's state machines differs between them - same route
(`confirmedStandaloneNormalRoute` / `below-foreign-chain-fsr`), same epochs, same
present-callback bridge, same 2x factor, same 3 log lines per rendered frame.

What does differ is only what the overlay *reported*, and it is not what the frames did:

| steady window | overlay (display series) | same frames at Present |
| --- | --- | --- |
| bad: fps / 1% low / stddev | 85.1 / **54.9** / **2978 us** | 84.9 / 66.7 / **748 us** |
| good: fps / 1% low / stddev | 90.7 / **67.1** / **1426 us** | 91.1 / 74.4 / **612 us** |

The mean is right to 0.3% in both and only the *values* are wrong - the exact failure
mode `display-change-timing.md` warns about. The sensor health line names the cause
directly: `usableClock=0`, `unresolved=3001` of `published=3063`, and `publishedInterval`
equal to `latchInterval` to within 10 us, i.e. nothing was rounded and the overlay was
drawing raw `HSyncDPCMultiPlane` flip-latch timestamps. `publishedInterval p1Us=6600` is
below the panel's own 6946 us minimum frame interval, which is the impossibility check
that settles it. Latch jaggedness 4575 us (bad) against 2043 us (good) with runtime
`PresentStart` jaggedness 351-485 us in both: the presents were even, and all of the
variance the user was looking at lived in the flip path. Under variable refresh below the
cap the blank clock has no grid, so this is the permanent state there, and the DPC noise
that rides on it varies run to run - which is exactly "sometimes worse, sometimes better".

Fix, in three connected places:

- `common/display_timing_shared.h` publishes what a timestamp *is* (`flags`,
  `kDisplayTimingScreenTimeResolved`, ABI 57). The producer knew; the ring did not carry
  it, so no consumer could tell a screen time from a latch time.
- `PerformanceMetrics::ScreenTimeCadence` judges the stream over a decaying window and
  `RefreshEffectiveSource` will not select `DisplayChange` for a stream that is not
  publishing screen times. Presentation timing - the same frames, measured where the
  measurement is exact - drives the graph, lows and variance instead. Two thresholds
  (90% to select, 50% to keep) stop a stream near the boundary switching every window;
  a stream is trusted until there is enough evidence against it, so nothing is withheld
  at startup. `[Overlay] Frame timing source:` now reports `screenTime=`/`screenTimeShare=`.
- The recording correlator applied the matched sample's *per-sample cadence residual* to
  the file's source timestamps (`NormalizeFinalOutputDisplayTimestampQpc`). With latch
  times that is +/-3 ms of measurement noise written into a CFR recording the game never
  had. `ResolveDisplayTimingAfterWatermark` now reports the matched sample's provenance
  and the correlator keeps the virtual (present-derived) cadence for latch-only samples,
  while the *smoothed* phase keeps learning from them. New `latchOnly=` health counter.

Not changed: the display series still accumulates every sample (an unresolved timestamp
is still an ordered displayed transition, only its interval is untrustworthy), and system
latency still observes all of them.

### 2026-09-04 - Two unbounded per-frame log lines on the game's render thread under FSR FG

Found while reading the sessions above. With `log_level` defaulting to `trace`, CE wrote
about 135 lines per second from Talos's render thread (`T:5830`) for as long as FSR FG
ran - each a global mutex plus two unbuffered `WriteFile` calls, 1.7-2.1 MB per 45 s:

- `FFX Hook: Armed VEH breakpoint ... (forward-call rearm)` - 1775 lines in 26 s. Only
  the `post-call rearm` reason was metered; the forward-call reason logged every time.
  Both are steady-state heartbeats and are now metered together
  (`ce::log_meter::ShouldLogCadence(n, 20, 300)`); genuine transitions still log always.
- `Streamline Hook: Viewport N state ...` - 3099 lines in 26 s. `stateChanged` compared
  against whether the map held an entry, but the disable path *erases* it, so an
  absent-before/absent-after steady state was indistinguishable from a transition on
  every query. It now compares against the last state actually written to the log
  (`streamline_hook_g_ViewportLoggedStates`).

Open, with evidence, not fixed: the protected-official-FFX `ffxConfigure` entry
breakpoint was hit **zero** times in both sessions (`s_vehHitLogCount` never logged) -
the game reaches `Hooked_ffxConfigure` through the IAT route. Yet
`CallFfxConfigureOriginalGuarded` still pauses and re-arms it around every forward:
one `VirtualQuery`, four `VirtualProtect` on AMD's executable code page and two
`FlushInstructionCache` per application frame, on the render thread, to maintain a
breakpoint nothing hits. The project's own measurement puts `ffxConfigure` at 2 us
without CE and 40 us with it (two calls per frame). The clean fix is a trampoline so the
0xCC never has to come out - which also closes the window where a concurrent thread runs
the runtime's `ffxConfigure` unhooked - but it rewrites a path under the "FG must never
break" constraint and needs a hardware run to land safely.

### 2026-09-04 - FSR FG had no application-source Present at all

User observation: real Reflex/PCL PC latency tracks the screen far better than the
no-Reflex estimate, and Talos reports ~45 ms under FSR FG against ~70 ms under DLSS FG.
Session `20260904_034526` settles which of the two is wrong. In the DLSS-FG window both
sources are live and the cross-check agrees to within a few ms
(`estimate=68.8 vs published 67.7`, ten consecutive samples), so the estimator is
calibrated. The FSR-FG window runs at nearly the same cadence (application 21.0 ms,
output 11.2 ms against 23.2 ms / 11.0 ms) and reports 45 ms, and the entire difference
sits in one term: `presentToDisplay` is 2-4 ms under FSR and 17-25 ms under DLSS.

Root cause: **`applicationPresents_` was empty for the whole FSR-FG window.** The only
caller of `ObserveApplicationPresent` was `DX12_ObserveApplicationSourcePresentTiming`
inside `ProcessFrame`/`DX12_ProcessFrameMinimal`, guarded by `applicationSourcePresent`,
which `ShouldApplyDX12PrerenderLimitOnPresent` grants only on the tracked game Present
thread. Under FSR FG the game presents into AMD's frame-generation swapchain proxy and
AMD's *presenter thread* issues the real DXGI present, so CE's `DetourPresent` never runs
on the game thread and the app-callback route (`DX12_RenderOverlayViaFFXPresentCallback`)
does not reach `ProcessFrame` at all. Consequences, all silent:

- `MatchApplicationPresentLocked` always failed, so the generator hold fell back to the
  modelled `(fgMultiplier - 1) x displayInterval` floor instead of the measured span.
- `ResolveWorkIntervalLocked` fell through to `ResolveFgBaseIntervalLocked`, so the whole
  window's `baseInterval` is `round(1e6 / baseFps)` from the FG runtime's published base
  FPS rather than measured cadence - confirmed exactly on every logged line.
- `presentToDisplay` measures only AMD's presenter-thread present to scanout (~4 ms). The
  render-ahead the association is supposed to expose lives *above* the DXGI present here,
  inside FidelityFX's own queue, so it was invisible from both terms.

Fix: `DX12_ObserveFFXProxyApplicationSourcePresent` in the FFX proxy-present detour
(`hook/apis/dx12_hook_ffx_proxy_present.cpp`), outermost entry only, before any routing
decision - the measurement must not depend on which overlay-composition route is live.
The proxy Present *is* the application's Present: game thread, once per rendered frame.
A same-frame duplicate from a synchronous passthrough is rejected by the existing 3 ms
minimum application interval.

Diagnostics: `generatorHold=` in `[Overlay] PC latency chain` was a bool and printed `1`
for both the measured and the modelled hold, which is precisely why the shortfall looked
healthy. It now prints `measured` / `modelled` / `none`.

Modelled reconstruction of the logged FSR topology (21 ms application, 11 ms output, 4 ms
present-to-display): 46.5 ms modelled versus 63.0 ms measured, against 66-73 ms of real
markers in the DLSS window at the same cadence.

**Confirmed on hardware, session `20260904_042922`.** FSR FG now reads 62-65 ms at
baseFps ~45 / outputFps ~90 - the same cadence that read 45 ms before - so the FSR-FG-on
against all-FG-off delta is ~+25 ms instead of ~+7 ms. The conclusive evidence is not the
value but `frameBeginInterval`: `0us` on every FSR line before, `22268-23328us` after. The
application-source stream carries a frame-begin anchor with it, and **Talos calls its
low-latency sleep even under FSR FG**, so the FSR path is now fully measured
(`frameBegin=low-latency-sleep`, `anchorToPresent=41826-54687us` against a modelled floor
of `baseInterval + displayInterval = 35044us`), not merely hold-corrected.

That immediately caught a defect in the new diagnostic itself: `generatorHold` printed
`modelled` on those lines. `holdMeasured` was only set in the no-anchor branch, but the
step back onto the held application frame is what makes the hold measured in *both*
branches - the anchor form spans from that frame's own boundary, the no-anchor form from
its Present. Only the `expectedGeneratorHoldUs` addition is a model. Corrected: the flag is
now seeded from `holdApplied` and cleared only when the hold could not be applied.

Open: the same structural gap applies to any generator that paces from its own thread
without CE seeing the application Present (Intel XeSS-FG, AFMF, third-party proxies). Only
the FidelityFX proxy is wired.
