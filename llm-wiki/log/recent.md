# llm-wiki Log

### 2026-09-13 - SB DX12 fps limiter: the 2 ms duplicate-present window ate 46 genuine frames/s

Session `20260913_122208` (Strange Brigade DX12, `FpsLimiter.general_fps=90`, `general_limiter_mode=basic`, inject
capture): the game presented ~130 fps with alternating short/long frame times while `fps_limiter_trace.log` reported
a flawless cadence - `waited=120 late=0 avgFps=90.0` every window. Both halves were true. Apply() ran 129 times/s
(2160 paced + 1126 `activeDedup` over 25.5 s, matching the 3448 perf-CSV present rows over 26.7 s and the overlay's
own `source_current_fps` of ~128.9). The escapes were the whole gap: ~88 paced/s + ~46 deduped/s = the observed rate.

Root cause: the non-boundary Apply() path classified a call as a duplicate present by wall clock - a 2 ms window
since the last Apply return. Strange Brigade DX12 renders a frame in 1-2 ms, so a genuine next present repeatedly
landed inside that window, returned without taking a grid slot, and reached the swapchain unpaced. The logged
`sinceReturnUs` values cluster at 1.1-1.95 ms, not the tens of microseconds a real Present+PresentEx duplicate
takes. The dedup never updates `lastApplyReturnQpc`, so the pattern is "paced frame, short unpaced frame ~1.5 ms
later, wait to the next slot" - exactly the short/long alternation the user saw.

The window could only misfire there: `IsRecursivePresent()` (`dxgi_shared_g_presentThreadId`/`presentDepth`) and the
`IsInWrapperPresent()` early return already reject every nested, cross-thread and wrapper-owned re-entry before
`ExecutePresentCore` reaches Apply(), so a second Apply() for one presented frame is structurally impossible on the
DXGI path. This is the same defect already fixed for Strange Brigade Vulkan in 2026-08 (`gateEveryPresent`), which
never reached the D3D sites.

Fix: `Apply()`'s second parameter is now `ce::fps_limiter_policy::PresentSite` - a structural call-site contract
instead of a bool. `kDuplicateProne` keeps the legacy window for sites whose second call genuinely is the same frame
(DXVK Present+PresentEx, the D3D9/D3D8/DDraw/OpenGL wrappers). `kFinalOutputBoundary` is the old `gateEveryPresent`,
unchanged, and stays the only contract allowed to own `OutputGroupAdmission`. The new `kUniqueApplicationPresent`
covers the four DXGI top-level boundaries (`DetourPresent`, `DetourPresent1`, `CWrapDXGISwapChain::Present`/
`Present1`): the duplicate window is skipped entirely and every entry takes a cadence-grid slot under the blocking
cadence lock.

Deliberate boundary: while frame generation is producing, a `kUniqueApplicationPresent` site keeps the established
window. That DXGI stream also carries runtime-owned generated presents (FFX presents an interpolated frame from its
own proxy swapchain) that CE cannot classify structurally there yet; gating every entry would spend a base-rate grid
slot on a generated present AND block the runtime's presenter thread inside CE's cadence lock - the documented FFX
freeze class. This is not the rejected `strictGrid = boundary && !FGActive` escape from Portal RTX: a real
final-output boundary stays unconditionally strict. Classifying generated presents on the DXGI path the way the
Vulkan boundary does is the open follow-up.

Diagnostics: the `LOCAL timer cadence active` / `LOCAL timer start` lines now carry `site=` and `strictGrid=`.
A fixed run must show `activeDedup=0` in the DX12 stats lines.

Tests: `tests/test_fps_limiter_present_site.cpp` (the bug as a requirement - an immediate second Apply on a unique
site must take a grid slot; the FG-active site must keep the window; the inactive fast path must not stall; pure
policy table for `ShouldGateEveryApplyOnCadenceGrid`) and a `PresentPacingPolicySourceTest` that pins the contract
at all four DXGI call sites. Hardware run pending.


### 2026-09-13 - DX12 dynamic glyph boxes were an upload/allocator ownership race

A supplied DLSS-G 4x screenshot showed the first `3` of the graph's dynamic `33 ms` ceiling label as a box while
the adjacent identical `3` rendered correctly. The shared ASCII atlas and CPU text construction therefore could not
explain the per-instance failure. Static lifetime reconstruction found that the x64 descriptor-free backend reused
four persistently mapped VB/IB slots independently of PostSL's fence-selected pool of up to 16 command allocators,
while PostSL disabled the upload guard. At generated-output cadence the CPU could wrap the smaller ring and rewrite
vertices still being read by the GPU; digit-count and string changes made those mixed bytes visible.

The descriptor-free, textured, normal, and PostSL paths now share a 16-slot allocator/upload lifetime domain and
force each draw's upload slot to its proven-complete allocator index. PostSL publishes the precise next overlay-fence
value before recording and signals that same value after submitting the list. Missing descriptor-free coupling uses
the guarded ring only with a live/nonzero completion guard; otherwise the draw is refused and rate-limit logged.
Focused `DX12UploadSlotGuardTest` coverage passes. The first complete verification run exposed a known
scheduler-sensitive FPS-limiter test: it required every admitted callback to spend at least 3 ms asleep even though
a callback arriving after its cadence deadline correctly returns immediately and re-bases. The no-FG integration
test now checks deterministic boundary, group-owner, generated-slot, and concurrency-skip counters instead of elapsed
wall time. The final `--verify --skip-updates --concise` gate passed on build `0.1.6527`, including both hook
architectures, the full native and Python suites, clang-tidy/file-size ratchets, and ASan/UBSan. Proprietary-driver
and game confirmation remain pending at this entry.

### 2026-09-12 - DLSS-suspended PostSL capture and screenshot ordering

Static reconstruction found one shared transition seam behind ignored inject screenshots and overlay pixels in
`capture_include_overlay=false` recordings. A real PostSL callback can remain the exact output/overlay owner after a
game suspends DLSS-G for a cutscene, but final-output capture previously required the active FG signal. The callback
could therefore draw first while capture either disappeared or later used the nominally overlay-free ProcessFrame
location. Screenshot routing had the complementary error: global PostSL active/confirmed latches made ProcessFrame
yield even when the current callback returned on scene cooldown or render-lock contention, leaving the request
Pending with no producer.

Real presented-output callbacks now choose a final-generated or suspended-base capture domain from the live DLSS-G
signal. Both domains retain same-queue before/after-overlay ordering; the suspended domain publishes ordinary base
metadata and shares the base cadence gate. A Present-scoped capture claim prevents duplicate ProcessFrame capture,
and base gating occurs only after Phase1 route selection. Screenshot ownership during suspension requires the actual
callback or a successful PostSL draw in this Present, while the post-ProcessFrame include path rechecks request state.
The D3D12 screenshot producer also derives its device from the exact backbuffer, retains and validates the submission
queue's COM device identity, passes a live swapchain rather than a released `IDXGISwapChain3`, completes post-claim
readback setup failures explicitly, and emits rate-limited stage diagnostics. Focused DXGI, final-output, and
screenshot policy/regression suites pass; proprietary-driver/game validation remains pending.

### 2026-09-12 - Screen grab privacy: Virtual desktop switching and Task View privacy blackout improvements

Investigated delayed/unreliable video blackening under `black_when_no_fullscreen_focus=true` with `dxgi_dup` monitor capture when switching Windows virtual desktops (`Win+Tab` hotkey or desktop navigation). Root causes identified:
1. When switching virtual desktops, Windows cloaks windows on inactive desktops using DWM cloaking (`DWM_CLOAKED_SHELL` 0x02 via `DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, ...)`), but `IsWindowVisible()` remains `TRUE` and `IsIconic()` remains `FALSE`. Because Windows focus handover to the new desktop lags by 100-500 ms, `TryClassifyWindowFullscreenLike()` previously continued treating the invisible/cloaked game window on Desktop 1 as an active fullscreen foreground window while `dxgi_dup` was already duplicating the physical display showing Desktop 2.
2. `Win+Tab` opens Task View (`XamlExplorerHostIslandWindow` / `Windows.UI.Core.CoreWindow`, owned by `explorer.exe` or `xamlexplorerhost.exe`), which spans the entire monitor and was previously classified as `fullscreenLike = true`.
3. Inactive desktop backgrounds (`WorkerW` / `Progman`) and taskbars could be classified as fullscreen-like if focused on Virtual Desktop 2.
4. In monitor-scope capture (`dxgi_dup`), once a game establishes verified fullscreen focus on the captured monitor, switching to another virtual desktop that contains another fullscreen window (e.g., browser or document viewer) previously had no process continuity check while the target game remained alive.

Fixes implemented:
- Added `IsWindowCloaked()`, `IsWindowOnCurrentVirtualDesktop()`, and `IsIgnoredShellWindow()` to `common/screen_grab_privacy.*`. `TryClassifyWindowFullscreenLike()` now immediately rejects cloaked windows, windows not on the current virtual desktop, and shell/system UI classes/processes (`explorer.exe`, `xamlexplorerhost.exe`, `shellexperiencehost.exe`, `startmenuexperiencehost.exe`, `searchhost.exe`, `textinputhost.exe`, `WorkerW`, `Progman`, etc.).
- Added target continuity tracking (`capturedMonitorWindow_`, `capturedMonitorPid_`) to `ScreenGrabPrivacyRuntime`. In monitor-scope capture, once verified fullscreen focus is established, only windows from that same application/process can satisfy fullscreen focus while that target window remains alive. If the target closes, the lock releases cleanly.
- Added regression tests in `tests/test_screen_grab_privacy.cpp` for cloaked, virtual desktop, and shell class rejection.
- Validation: Complete `--verify --skip-updates --concise` gate passed with zero regressions (content-validated build, all unit tests, Python self-tests, clang-tidy ratchet clean, ASan/UBSan clean).

### 2026-09-10 - RR preset ladder re-ranked by cost with a new `high` level

The 10-15% frame-time delta between `ray_reconstruction_optimal_settings=medium` and `=full` was attributed to the
wrong settings, so the preset was ranked by cost instead of by "maximum quality". The bundle mixed three kinds of
entry and only the third is what the delta is made of: history-only settings (free), settings that are already engine
defaults or reduce work, and paid sampling density. Levels are now `off|light|medium|high|full` and a strict ladder:
`medium` holds every free/default/work-reducing entry (screen-probe history floor, spatial filter passes, the two
`LumenScene` update factors, MegaLights tiers, SRAO apply mode), `high` holds the paid density that is visibly worth it
(VSM ray counts and local LOD bias, the engine-clamped 16 octahedron lattice, radiance-cache `ProbeResolution`, RR
firefly/ghosting tolerances), and `full` holds the two maximum-sampling escalations (`Temporal.MaxRayDirections` floor
16, radiance-cache probe budget 600, 5.6+ full-resolution short-range AO). The one reversed entry was fixed:
`r.Lumen.ScreenProbeGather.StochasticInterpolation` is `1` (AMD measures up to ~30% faster screen probe gather
passes; Epic's High scalability uses 1; a stochastic signal is what an RR denoiser expects) instead of the bilinear
`0`, with `SpatialFilterNumPasses=3` absorbing the extra noise. The reflection-density saving (`DownsampleFactor=2`
with the 5.6 `DownsampleCheckerboard=1` middle ground) is documented as an opt-in `custom_cvar_overrides` example
rather than applied, because `light` deliberately disables the Lumen reflection reconstruction and the non-RR quality
cost is unmeasured. Inserting a level renumbered the shared-memory preset byte (`full` 3 -> 4) and moved
`SHARED_MEMORY_VERSION` 57 -> 58 with the hardcoded mapping literals; `RayReconstructionPresetName()` now prints the
level in `UE5 overrides enabled: ... rrOptimal=%d(%s)`. Validation: the complete `--verify --verify-clean` gate is
green (content-validated product build, full native unit suite, Python tool self-tests, clang-tidy/file-size/Python
lint with no new findings, ASan/UBSan regression), including the new tier-membership and ABI-value ratchet tests. No
in-game A/B or frame-time measurement yet.

### 2026-09-10 - RR preset F AO-boiling mitigations: full-res short-range AO, history floors, honest MegaLights tier

A follow-up review of the `full` RR preset against DLSS 4.5 RR preset F (2nd-gen transformer) splits three
assumptions that predated that model. Preset F preserves detail instead of temporally smoothing it, so residual
half-resolution short-range AO from UE 5.6 and an artificially low screen-probe history survive into the output.
`full` now also writes `r.Lumen.ScreenProbeGather.ShortRangeAO.DownsampleFactor=1` with `ShortRangeAO.Temporal=1`
(5.6+; older engines log them missing and skip), and the two screen-probe history entries became floors: new
`ApplyMode::Floor`/`ResolvedValue::floor` resolve to `max(configured, observed)` through `ResolveEffectiveBits()`, so
Talos's game-tuned `MaxFramesAccumulated=25.0f` is kept while the engine default of 10 is raised to 16, and an
unrelated settings change re-floors instead of downgrading. `r.MegaLights.NumSamplesPerPixel` is corrected from 8 to
4: the engine quantizes to 2/4/16 and 8 executed as 4, which the config template and tests now state.
`r.Shadow.Denoiser` stays out on purpose (would need an RR-state-gated enable with restore; UE 5.6/5.7 actual default
2, help text stale). Open thread: the Streamline 2.12+ `ResponsivityMask` (id 68, one channel [-1,1], R16F/R8 SNORM)
is the real 4.5 temporal control, but it is a per-frame resource tag the game/plugin must submit; CE does not inject
it, and the 68/69 UIAlpha renumbering makes cross-version tagging risky when newer SL DLLs are pinned under older
plugins. Validation: full unit suite + Python self-tests, incremental product build, clang-tidy/file-size/Python lint
all clean. No in-game A/B yet.

### 2026-09-10 - Repeated-start validation closes the FSR rate split; exact Present-queue proof closes DLSS startup blank

Talos session `installed/captureengine/logs/20260910_075304` exercises build `0.1.6518` over eight
starts. Six FSR-FG starts cover both discovery phases (`d3d12=0` and `d3d12=1`) without the former
slow/fast split: settled output windows span 92.8-95.2 fps at 99-100% GPU load in this scene, with
zero uncovered FSR presents, unresolved pacing matches, staging drops, overload rows, device
removals, dumps, or hook failures. The 2.4-fps spread is ordinary GPU-saturated scene variance, not
the previous repeatable roughly eight-percent bimodality. Every process used WARP-only synthetic
DX12 discovery and six grouped hook batches (43 prepared/installed, zero independent/failing), so
the injection-startup isolation is runtime-validated across early and late D3D12 discovery.

The session did expose one separate strict-visibility regression. Both pure DLSS-FG cold starts
(PIDs 7068 and 11792) identically reported 29 uncovered presents over 157 ms, from
`postsl-inactive` through `postsl-reactivation-warmup`. Talos enables through GetState, so the
explicit-SetOptions proof is unavailable. PostSL nevertheless selected the exact original Present
queue and swapchain with no Streamline wrapper and a healthy device. The old same-queue predicate
rejected this because `g_CommandQueue` held a different execution-discovered render/primary queue;
that queue is not the owner of the presented backbuffer and must not outweigh exact Present-route
evidence. The in-process FSR-to-DLSS transition remained fully covered through its existing proof.

The normal overlay path now publishes a non-owning exact-swapchain proof only after a successful ECL
submit on the retained original game queue; RTV cleanup clears it. A pure-DLSS cold start can bypass
the countdown and warmup when the PostSL swapchain queue still equals that original queue and the
successful-normal-overlay, original-queue ownership, and swapchain-queue capture identities all
match the current swapchain and its overlay/sync backend remains live. A separately discovered
command queue no longer vetoes this stronger
proof. FSR history, a different/runtime swapchain queue, an SL wrapper queue, missing exact proof,
or device removal still preserves the conservative startup guards, so the documented GTA
separate-proxy-init hang family remains excluded. Expected Talos diagnostics are
`Proven successful normal-overlay submit on exact original-queue swapchain`, immediate synthetic
startup takeover with `exactNormalOverlayProof=1`, warmup bypass, and zero visibility interruption.
Runtime re-validation of the post-session change is pending.

### 2026-09-09 - Isolate injection bootstrap from D3D12/FSR graphics startup

The controlled build-0.1.6515 pair in `20260909_063715` supersedes the narrower
`20260908_201724` diagnosis. Healthy PID 3416 was discovered with `d3d12=0` and completed CE
startup before the application's D3D12 path. Slow second-run PID 19884 was discovered with
`d3d12=1`; CE's fatal-hook quiescences (34.000-34.517), synthetic hardware DX12 bootstrap
(34.519-35.449), and OpenGL hook quiescences (35.455-35.901) overlapped the application's
`D3D12GetInterface` (34.909), factory creation (35.097), NVAPI startup (35.256), and official FFX
load (35.366). The healthy run settled near 116 output fps; the slow run near 107.5. CE's
steady-state callback/Present work remained tiny, but FSR latched a different pacing state. The
owned defect is injection's phase-dependent process-wide startup disturbance, not an expensive
steady-state overlay draw.

The temporary DX12 Present-hook bootstrap now obtains WARP through `IDXGIFactory4::EnumWarpAdapter`
and never creates a hardware-adapter device. It bypasses an existing foreign factory/device entry
patch or fails closed when a safe bypass cannot be built. A thread-local internal-probe scope keeps
the WARP device, queue, and swapchain out of application evidence, sampler/device hooks, and queue
tracking; the old process-global synthetic-swapchain flag could suppress a real concurrent game
swapchain. Hardware and WARP expose the same tested ECL/Present/Present1 method addresses, so this
removes vendor-UMD startup interference without weakening hook discovery or adding a copy/wait.

Related inline hooks use a two-phase transaction: decode, allocate, seal/register, and atomically
publish every trampoline while peers run, then exact-range/exact-byte validate and patch the group
under one `ThreadQuiescence`. Unsafe members retry independently after peers resume. Fatal hooks,
OpenGL swaps, DXGI Present/Present1, the DLSS registry pair, each Streamline core-module family, and
the NGX core export family use it. NGX aliases publish all callable predecessors before their shared
entry becomes live. This preserves CFG, foreign-entry chaining, fail-closed ownership, and hook
coverage while removing repeated whole-process suspension from startup.

Process discovery is also ordered and race-safe. `InjectionManager` resolves paths in its
constructor, the injector installs the target-config callback, and only then calls
`StartMonitoring`. WMI prefers event-driven `Win32_ProcessStartTrace`; access denial or later
subscription failure transitions once to the existing `__InstanceCreationEvent WITHIN 0.5`
fallback, with an immediate catch-up scan. WMI callbacks only queue fallback work for the owner
thread, as required by the sink callback contract. One atomic subscription state prevents duplicate
fallback activation, and a PID set coalesces duplicate scan/event workers. Logs identify the event
source and process age. The direct suspended-launch helper intentionally creates no monitor.

User run `20260909_170855` exercised build 0.1.6516 and is healthy but not a controlled proof. The
listed Talos profile resolved as requested (`video_capture=inject`, `dll_injection=always`, FIFO,
SR preset M, RR preset F, debug indicator, forced/optimal RR, UE5 post-processing/sharpen/AF/mip/
gamma overrides, and no CE FPS limiter). `Win32_ProcessStartTrace` was denied with `0x80041003`, so
fallback discovery arrived at process age 317.585 ms with `d3d12=0`. The WARP bootstrap completed
about 1.5 seconds before the application's first D3D12 interface call. Fatal and OpenGL hook
families used one grouped quiescence each. Native FFX callback ownership was clean: zero uncovered
overlay transitions, zero unresolved/dropped pacing matches, zero staging drops or overload rows,
roughly 73-89 us average callback-bridge cost, 1-10 us proxy-Present CE cost, and 15-16 us median
steady total CE CPU cost. Stable output windows were about 86-87 fps at 100% GPU load; this different
scene cannot be compared to the prior 117/109 fps pair.

That run also exposed the remaining startup-only quiescence fan-out: two DLSS registry hooks and
roughly 29 Streamline/NGX export installs, including 22 NGX suspensions totaling about 1.2 seconds.
The grouped registry/Streamline/NGX changes and refined WMI state/logging were added after the tested
0.1.6516 binary and still require a fresh identical-scene repeated-launch check. Background-only
session `20260909_074301` is not Talos evidence. Focused injection, D3D12, Streamline, NGX, and
batch-publication regressions pass; the complete `--verify` gate is the closing acceptance criterion.
