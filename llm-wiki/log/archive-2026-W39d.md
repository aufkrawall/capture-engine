# llm-wiki Log Archive 2026-W39d

Covers 2026-09-25. Newest first.

### 2026-09-25 - Fat FSR FG frame-time graph at the vsync cap: refresh-bounded graph time (0.1.6817)

- Symptom: `dx12_fg_switch_test` (4K, `gpu_load=120`, vsync, 144 Hz G-Sync) drew a jagged display
  frame-time graph with FSR FG active; off / suspended / DLSS FG and Talos were flat. RTSS agrees (same
  ETW source). Present by months.
- Pacing trace `20260925_183820/pacing_trace_30940_7258079576.csv`: AMD presenter Presents all `sync=1`,
  alternating 6844/7045 us (AMD pacer formula at exactly 72.0 fps), forward ~150 us; kernel HSync MPO
  completions alternating 3.53/10.36 ms, constant per frame type. FIFO + mean interval = one refresh
  proves the scanout was even. Test app refuted as the cause (standard swapchain desc; AMD's sample never
  waits on the FFX waitable; AMD builds the real swapchain itself).
- Change: sensor publishes a second, refresh-bounded `graphTimeUs` per sample (shared-memory v64); only the
  overlay graph uses it. See `display-change-timing.md` (Refresh-bounded graph time) for the rule, the
  safety properties and the new `graphInterval`/`refreshBound` health fields. Hardware run pending: expect
  `refreshBound(bounded=~half the FG-on frames meanShiftUs=~3400)` and a flat graph.
- VALIDATED `20260925_190800` (0.1.6817): published jaggedness 6410 us -> graph 38 us, p1/p50/p99 all
  6900 us, 3-refresh transition hitch (20835 us) kept; `refreshBound(periodUs=6946 eligible=1095
  bounded=1093 noBlank=0 meanShiftUs=2365 maxShiftUs=6721)`. Nearly every frame is bounded, not half:
  the on-time flip type is also reported ~0.6 ms before its VSync blank, so the graph locks onto observed
  blanks. Consequence: a real hitch after such a lock is drawn up to that lead (~0.6 ms) short and the next
  frame as much longer - sub-refresh, never hidden.

### 2026-09-25 - GTA FFX creates were never observed (`20260925_165708`, 0.1.6814)

- User-validated overlay run in Talos and GTA; the remaining gap was structural. Three `amd_fidelityfx_dx12.dll`
  reloads, zero `context CREATED`, all six destroys `Non-FG`; no `GetProcAddress: Intercepted FFX API` line at all.
  The cached-slot rescan routed GTA's pointers ~16 ms (up to 1 s) after the create on every reload.
- Fix: `ffxCreateContext` entry breakpoint armed from the load notification (Rip redirect into the detour, guarded
  forward) plus configure-time adoption of unseen contexts. See `frame-generation/guardrails.md`
  (create-observation invariant). Older GTA run `gtaslowfsrfgtodlssfg` did observe creates and the full teardown, so
  GTA now takes an already exercised path.
- VALIDATED in GTA `20260925_172935` (0.1.6815): 12/12 creates via the breakpoint, every FSR close ran the full
  teardown (menu-only sessions retired the startup latch through the swapchain-context destroy), zero adoptions,
  no crash; user reports everything working.
- Follow-up: the same run showed the `ffxConfigure` breakpoint re-arming a just-unloaded startup image's address;
  FFX unload invalidation plus a cached export proof now guard every configure arm/restore (guardrails,
  image-lifetime invariant). VALIDATED in `20260925_174546`: probe unload disarmed the live configure byte, no stale
  re-arm, 9/9 creates caught, 5/5 FSR closes ran the full teardown.

### 2026-09-25 - Talos menu FSR FG -> off -> DLSS FG hid the overlay for good (`20260925_061003`, 0.1.6813)

- Logs: `installed/captureengine/logs/talosoverlaydisappearedinmenu`, complete sequence coverage (0 missing).
- 06:11:13 FSR FG enabled on the native app-callback route; 06:11:17 disabled; 06:11:23 FFX context destroyed and
  sl.dlss_g created a fresh proxy (same COM address). No prewarm, no `slDLSSGSetOptions`, DLSS-G never generated;
  every Present logged `Inactive-DLSS Present has no exact queue-ownership proof` until exit.
- Cause: the prewarm's "retiring overlay live" input read only the normal-route backend, which native FSR's callback
  route leaves uninitialized. The 06:10:50 FSR->DLSS handoff worked because FSR never got past protected startup there.
- Fix: coverage-based liveness plus a skipped-prewarm diagnostic (`frame-generation/guardrails.md`, recovery-latch
  invariant). Talos run pending.

### 2026-09-25 - GTA menu FSR FG -> all off hid the overlay for good (`20260925_054901`, 0.1.6812)

- The b856d249 latch fix held: `Ended post-FSR non-FG recovery on proven normal return inside the FG-transition guard` fired.
- New cause: FSR FG in the menu only receives disabled `ffxConfigure`, so the protected FFX startup latch stays armed.
  On FSR -> off, both FFX context destroys logged `Non-FG Context destroyed` (create missed after the module reload),
  so no exit ran; the game's own original-queue swapchain stayed tracking-only until exit. Earlier FSR menu sessions
  in the same log were rescued only by the next Streamline handoff. Fix: game-created original-queue swapchain in the
  protected window retires the latch (`frame-generation/guardrails.md`). Hardware run pending.

### 2026-09-25 - GTA menu DLSS FG toggles hid the overlay for good (`20260925_052251`, 0.1.6811)

- **Cause** - the post-FSR recovery latch (armed by the 05:25 DLSS OFF after FSR history) survived a proven normal return at
  05:34:36 because that swapchain change took the recent-FG cooldown branch. At 05:34:42 the menu toggle created a fresh
  Streamline swapchain on sl.dlss_g's queue while GTA kept sending `slDLSSGSetOptions(OFF)`; the stale latch held every Present
  GPU-quiet (`Inactive-DLSS Present has no exact queue-ownership proof`, 2700+ times) until exit. The same shape at 05:25:35
  worked only because the latch was not set yet (`First exact prewarmed PostSL handoff Present preserved`).
- **Fix** - end the latch on a proven return in the guarded branch and on the exact prewarmed Streamline handoff; the gate
  passes that exact handoff. See `frame-generation/guardrails.md` (recovery-latch lifetime invariant). Hardware run pending.
- Still visible in the log: CE suppresses GTA's menu OFF calls as startup-protected churn (`suppressCount` in the tens of
  thousands). Harmless here (Streamline already OFF), but it is noise worth revisiting.

### 2026-09-25 - GTA FSR FG -> DLSS FG crash (`20260925_050613`, 0.1.6810): two CE defects

- **Crash** - `ERR_GFX_STATE` after `DXGI: Device removed (hr=0x887A0005)`, one frame after `Clearing stale
  runtime-owned Streamline no-FG swapchain after 120 consecutive real frames on origGame` restored
  `g_SwapchainQueue` to origGame. The presented swapchain was sl.dlss_g's fresh one on its own queue; the game
  renders on origGame regardless, so the heuristic's evidence was meaningless. It now also requires the presented
  swapchain's own queue to be origGame. The dumps only show the game's `int 3` after its error box.
- **Overlay hidden** - `DescFree: slot N still in flight (guard=1 completed=0)` from the 17th present on: the
  deferred overlay fence Signal was never flushed on runtime-owned swapchains (cff7a507 widened an AMD-only skip).
  This answers the OPEN "Streamline queue never retires CE's work" from `gtaslowfsrfgtodlssfg`: CE never signaled.
- Not implicated: the 76eabdd3 explicit-startup takeover (no `slDLSSGSetOptions(ON)` arrived before the crash).
  `FG: Multiplier changed 1 -> 4 (base=3.3, real=1)` right after the handoff is unexplained accounting noise.
- Details: `frame-generation/guardrails.md` (deferred-signal flush + live runtime swapchain invariant).

### 2026-09-25 - Slow FSR -> DLSS switches: one CE stall (fixed), one NVIDIA JIT, one test-app bug (fixed)

- **GTA `gtaslowfsrfgtodlssfg` (0.1.6801)** - 1 FPS for ~10 s after the switch was CE: the upload-slot wait blocked
  every Present for 1 s on a Streamline queue that did not retire CE's work. The wait is gone (see
  `frame-generation/guardrails.md`, upload-slot never-blocks invariant). Open: why that queue never retired.
- **Test app `testappslowswitchtodlssfgstoppedworkingfromfsrfgtodlssfg` (0.1.6803)** - the 13 s first DLSS switch
  was inside NVIDIA's `CreateFeature`: GPU 0.5 %, one core busy, and `%APPDATA%\NVIDIA\ComputeCache` created 82
  entries in exactly that window (cache at 1.02 GB, near the 1 GiB `CUDA_CACHE_MAXSIZE` default). Not CE.
- **Test app DLSS never returned** - `InitDX12` re-called `slSetD3DDevice` after renderer re-creation; Streamline
  accepts one device per slInit (`result=19`, "Plugins already initialized"). `BindStreamlineDevice` now holds the
  accepted device and reuses the binding. Diagnostic: `Streamline device binding reused`.
- **Follow-up `20260925_042117` (0.1.6807, DLSS -> OFF -> DLSS)** - with the binding fixed the switch reached
  Streamline's swapchain create and got `E_ACCESSDENIED` (CE recovery exhausted, `retained=0`). The dump's only
  remaining pointer to the OFF back buffer sits in `sl_dlss_g`-surrounded heap nodes that also hold the other two
  OFF back buffers and the OFF swapchain: the app had tagged the native chain's back buffer as
  `kBufferTypeBackbuffer` every frame, and DLSS-G kept that chain. The app now tags the back buffer only while
  Streamline's own swapchain presents (`Backbuffer tag withheld` otherwise). Not CE.
- **Switch-spam overlay dropouts `20260925_043001` (0.1.6808) - diagnostics first.** Three "uncovered" windows
  (516/734/94 ms) all sat on DLSS-G <-> FSR swapchain handoffs, but the coverage tracker counted one Present
  once per accounting site (PostSL + ProcessFrameExternal inside the same DetourPresent), so a covered present's
  second call read as uncovered. Real no-present gaps of 300-350 ms per switch (`heartbeat gap=319/351/353ms`)
  are the app rebuilding its chain; hypothesis: the departing chain's final image lacked the overlay and stays on
  screen for that gap. Now: `DX12_Begin/EndOverlayPresentScope` in DetourPresent/1 merge accounting per physical
  Present (`mergedCalls=`), and `SwapchainPresentLedger` logs `[OVERLAY SWAPCHAIN HANDOFF] departing=...
  lastPresent=drawn|inherited|MISSING endedWithoutOverlay=N -> arriving=... firstPresent=... noPresentGapMs=`
  plus `overlay first reached swapchain ... after N present(s)`. Open: rerun switch spam and fix the named cases.
- **Rerun `20260925_045043` (0.1.6809):** 22 handoffs, zero uncovered presents, every departing last / arriving first
  present drawn or inherited. The dropouts are the `inherited` ones: post-FSR DLSS-G startup outputs covered only by
  the official UI tag (generated frames only), ending before a 661/888/1012 ms switch pause. Fixed by the explicit
  post-FSR startup takeover (see `frame-generation/guardrails.md`, "official UI tag covers generated frames only").

### 2026-09-25 - Follow-up: pre-creation device release crashed Gothic II; device refs now end in the app's Release

Session `20260924_235830` (0.1.6804, user alt-tab): Gothic II caught an AV in `D3DIM700` and showed its own
`Application Error - Access Violation` box (text now logged by the new dialog recorder); the process later died
unhandled (0xC0000005) and CE adopted the WER dump. `dps` over the stale stack in the dump recovered the first
fault: `DetourDirectDraw7CreateSurface -> ReleaseDirectDrawChainBeforePrimaryCreation ->
ReleaseTrackedLegacyD3D7Device -> DIRECT3DDEVICEI::Release -> ~CDirect3DDevice7 -> ~CDirect3DDeviceIDP2 -> D3DFree`.
CE's tracked ref was the device's last and outlived the chain. Fix (0.1.6806): `IDirect3DDevice7::Release`
interception drops CE's device refs inside the application's last Release; pre-creation release no longer touches the
device; tracking/priming require the interception. Dialog text buffers raised to 2048 (the box lists a stack).
User reports Gothic II alt-tab crashes without CE too - expected to remain, but must no longer involve `capture_hook`.
Open: in-game alt-tab on 0.1.6806 - expect `Application released its last reference to D3D7 device=...`.
