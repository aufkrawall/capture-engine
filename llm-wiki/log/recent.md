# llm-wiki Log

### 2026-10-28 - HotS: the stalled Presents do not wait for the GPU; copied frames reach the screen ~3 ms late

- Session `20261028_131959` (0.1.7365, first run with copy GPU timing; same HotS settings, plus `cpu_prerender_limit=1`
  -> `SetMaximumFrameLatency(1)`, `capture_include_overlay=true` so CE's copy is the frame's last GPU work, media
  `gpu_priority=7` and `gpu_scheduling_priority=auto(high)`). One 11 s inject recording, healthy, stalling throughout.
- `[PRESENT STALL]` windows: `slow=81/1406` (78 after a copy), `115/1406` (all 115 after a copy; 0 of 446 Presents
  without a copy). Copy on GPU `copy=64/66/82us`; slow verdicts `doneBeforeCall/doneInCall/returnedAtCopyEnd/
  doneAfterReturn = 65/13/0/0` and `112/3/0/0`. Per call: the copy ended 0.06-0.13 ms before entry (or 0.01-0.48 ms
  after it) and Present returned 2.1-4.7 ms after entry. So the game's GPU frame and CE's copy are done before or
  just after the call; Present blocks on something outside the frame's GPU work. This disproves the earlier reading
  that the GPU frame finishes 3-4 ms after the call (entry below).
- Timing: stalls start with the first copies (13:21:21.9, 0.6 s after the media's GPU class went high) and end with
  the stop (13:21:33.3); limiter median `p2dUs` 3.0-3.4 ms during recording vs 0.37-0.43 ms before and after,
  back to 0.39 ms at 13:21:34.9 while the media's high GPU class was only restored at 13:21:35.8, so the class alone
  is not it. CSV: 12-13% of frames >2 ms per second, uniform over the recording (no clean stretch this time).
- Display: 144 Hz panel (vblank 6946 us), VRR, game 140 fps; every flip a hardware MPO immediate flip
  (`hsyncDpcMpo` +~140/s before, during and after; `vsyncDpc` flat). The median frame reaches the screen ~3 ms after
  its Present during recording while only ~12% of Presents block, so the flips themselves run late and the blocking
  call is a symptom (with frame latency 1 the call waits for an earlier frame to leave the queue).
- Open: what holds the flip of a frame CE copied. Candidates: the kernel/driver handling a submission that wrote a
  cross-process shared texture or signals a shared fence the media process waits on; the media's priority-7 contexts
  delaying the game context's present processing (HAGS). CE cannot see inside DXGI/DxgKrnl: next evidence is a
  GPUView/WPR trace of a stalling recording, or CE's DisplayTiming kernel present/flip times per slow call. Cheap
  A/Bs: `gpu_priority=0` + `gpu_scheduling_priority=off`; without `cpu_prerender_limit=1`.
- Added the same day: per-frame kernel milestones in the display stream (ABI 72) and `[PRESENT STALL] flip` lines
  (`present_flip_timing.h`; reading guide `cfr-capture-sync.md`, "Flip timing"). Pending a HotS run.

### 2026-10-28 - HotS: the inject-recording Present stall is back with the Signal held (cause open)

- Session `20261028_122156` (0.1.7363, HotS/DXUP on D3D12, same settings: 3840x2160 P010 av1_nvenc 120 fps CFR,
  140 fps limiter, face cam, 2-buffer swapchain). Three inject recordings, all healthy, all stalling: `[PRESENT STALL]`
  windows `slow(>=2ms)=124/1379, 150/1375, 150/1178, 107/1178, 79/1407, 163/1407, 56/1018`, every slow call
  `signal=held inFlightAtEntry=1 retiredDuringCall=0` (with the Signal held, the copy fence cannot see what Present
  waits on). CSV during capture: 5-14% of frames `present_call_us` >2 ms (p99 4.3-7.7 ms), 5-7% intervals >9 ms;
  overlay pacing stddev 271-1120 us vs 131-257 us in the clean `20261028_100931` recording. Same GPU load in both
  (3D ~44%, videoEncode 64%), same media cost (convert 0.02 ms, encode 0.2 ms), presentation stays hardware MPO
  flip (`hsyncDpcMpo` ~140/s, `composed(pid=0)`) in clean and stalled stretches.
- So the 2026-10-28 validation was a clean game run, not the fix: clean runs exist before it (`20261026_234233`)
  and stalled runs after it. The held Signal is kept (harmless) but is not the cure.
- The limiter is a witness, not the cause. Stalled stretches carry `p2dUs` (present to display) 2.4-4.4 ms vs
  ~0.45 ms clean (PC-latency chain `presentToDisplay` 3.3-4.4 ms), and the front-load controller answers that by
  growing its GPU reservation to the whole frame (`frontLoad=0 budget=7142 workCeilingUs` ~5-5.7 ms vs ~1.5 ms
  clean). Order per recording: recording 1 front-loaded (post-present release waits 1-4 ms) and clean until t+61,
  stalls from t+61 with front-load still on, front-load dropped one second later; recording 3 front-loaded and clean
  for 10 s, first stalls at t+136 with front-load on, dropped at t+137; back-edge stretches without stalls exist
  (rec 1 t+57-58, t+69). Clean stretches show a front-load budget of a few ms; the stall regime is sticky across
  limiter modes and ends on its own (rec 3 t+154).
- Signature now (GPU part disproved by `20261028_131959`, entry above): during inject capture only, the game's
  GPU frame finishes 3-4 ms after the Present call even
  though DXUP's ECLs and CE's overlay+copy ECLs precede CE's 2-5 ms pre-present wait (stage order: ProcessFrame ->
  limiter -> Present -> held Signal -> front-load release), and DXGI Present blocks 2-5 ms on ~1 frame in 7-10;
  the game's own CPU frame (`workCeilingUs`) grows by ~3.5 ms, as if it waits for that late GPU completion. Open:
  what the GPU or Present waits on. CE's only queue work is the back-buffer copy (PRESENT->COPY_SOURCE->PRESENT)
  into a cross-process shared ring texture; no CE `Wait` on the game queue. Next evidence: the copy's GPU
  execution time against the Present call - added the same day (`capture_copy_gpu_timing.h`; how to read it:
  `cfr-capture-sync.md`, "Copy GPU timing"). Pending a HotS run.
- Same run confirms the device-loss chain fixes (957302ad, fa79ef72): two hidden-window creates parked, no
  `deviceRemoved=0x887A...`, CSV `api=DX12` throughout, no false freeze.

### 2026-10-28 - HotS: CE removed the D3D12 device at startup; a false freeze dump followed (fixed, pending HW)

- Session `20261028_092712` (0.1.7362, HotS on DXUP's `Support64\d3d11.dll` over D3D12, NVIDIA). One chain of
  three CE defects; the recording itself (r0001, 67.5 s, inject) was healthy with exact audio track lengths.
- 1) Stale parked hidden-window create (address reuse). 09:31:55 T5404: DXUP's 1x1 probe device created swapchain
  `...67FA43A0` on hidden HWND `B092C`, queue `DD1780` -> `Parked create-time queue ownership`. 09:32:02 T46FC: the
  real swapchain for visible HWND `80880` came back at the same address `...67FA43A0` on queue `DCFCA0` (adopted
  correctly). `ForgetParkedCreateSwapchainsForWindow` only matches the window, so the dead probe's record survived.
  09:32:04.832 T1828, first Present: `First visible Present of hidden-window swapchain ...67FA43A0
  (HWND=...B092C) - promote its parked create (queue=...DD1780 presentedQueue=unreadable ...)`; CE re-adopted
  `DD1780`, initialized the overlay on it, sharpen submitted on it (`Passing CE overlay ExecuteCommandLists ...
  queue=...DD1780 reason=post-process`), and 1 ms later `Overlay frame #1 (deviceRemoved=0x887A002B ...)` - the
  same wrong-queue back-buffer write the ledger exists to prevent (Talos `20260927_034946`). `DecidePromotion`
  only discards on a readable, different queue; here `GetDevice(ID3D12CommandQueue)` was unreadable. Evidence of
  identity that was available: the presenting swapchain's window (`80880`) differed from the parked one (`B092C`),
  and a successful create returning the same pointer proves the parked object dead. CE then skipped the game's
  ECLs, DXUP's ResizeBuffers failed `0x887A0005`, and HotS fell back to a legacy D3D11 swapchain (`CreateSwapChain
  swapEffect=0`, CSV `api=DX11`) for the rest of the session.
- 2) False freeze. The removal set `DXGIShared::g_SharedState.deviceRemovedFatal`, which suppresses the heartbeat
  in every Present path (wrapper `Present`/`Present1`, `dxgi_shared_present_routing`/`present1`). It is cleared
  only by a new D3D12 device (queue adoption) or a D3D12 recheck, never by the game's recovery onto another
  device/API. HotS then presented 613 frames per 10 s on T46FC (`[PRESENT ORIGIN] summary ... presents=613`), yet
  the watchdog's elapsed grew 0.6/10.7/20.7 s and fired `Render thread frozen for 30 seconds` at 09:32:35.017
  (`monitoredTid=6184` = T1828, the dead D3D12 present thread). Genuine-freeze evidence: none.
- 3) The external dump helper could not start: `External pre-termination dump helper failed (... wait=0
  exit=3221225785)` = `0xC0000139` STATUS_ENTRYPOINT_NOT_FOUND, 8 ms after launch. HotS sets a DLL directory
  (dump: ntdll's DLL-directory UNICODE_STRING length 0x6A = 53 chars = `C:\Program Files (x86)\Heroes of the
  Storm\Support64\`), CreateProcess hands it to the child, and `captureengine.exe` statically imports `d3d11.dll`
  (`CreateDirect3D11DeviceFromDXGIDevice`, `D3D11CreateDevice`, `D3D11CreateDeviceAndSwapChain`) - DXUP's
  `d3d11.dll` there exports only `D3D11CreateDevice`, `D3D11CreateDeviceAndSwapChain`, `D3D11On12CreateDevice` and
  `DXUP*`. `Support64` also ships old `dbghelp.dll`, `msvcp140.dll`, `vcruntime140.dll`, `ucrtbase.dll`. The
  in-process fallback dump then froze the game 2.8 s (`DetourPresent: heartbeat ... gap=2792ms`).
- Fixed, pending a HotS hardware run: (1) 957302ad - a successful create of swapchain X supersedes any record
  parked at X, and promotion discards a record whose window differs from the presenting swapchain's output window;
  (2) fa79ef72 - `NotePresentDeviceResult` clears the device-loss latch on the first successful Present after it;
  (3) 8e124f08 - `captureengine.exe` links with `--dependent-load-flag=0xA00` (application dir + System32; project
  LLD 22.1.8). (3) reproduced with the installed 0.1.7362 binary: `captureengine.exe --dump-helper` exits 2
  normally and `0xC0000139` once the parent set HotS's `Support64` DLL directory (Windows' box: entry point
  `CreateDirect3D11DeviceFromDXGIDevice` not found). Next HotS run: expect no `deviceRemoved=0x887A002B`, the
  overlay on DXUP's D3D12 queue, and CSV `api=DX12`.
- Not CE bugs here: face cam `No Media Foundation video-capture device is available: HR=0 count=0` (camera
  unplugged); config warnings about bare `preset`/`display_gamma` keys in the user's profiles.

### 2026-10-28 - HotS: the game's Present stalls during inject recording (fix NOT confirmed, see above)

- Hardware result, HotS `20261028_092247` (0.1.7362, 3,850 inject-recorded frames, recording healthy): `DX12:
  SharedCapture completion Signal after-present`, no `queued a held completion Signal at the ...`, and every
  `[PRESENT STALL]` window `slow(>=2ms)=0` (forward captured 224/295/759 us vs uncaptured 201-210/262/610). Per
  recording, stalled runs vs this one: Present p99 4,544/4,158 -> 342 us, intervals >9 ms 5.5%/2.5% -> 0.36%,
  interval p99 10.7/10.1 -> 7.8 ms, mean successive jitter 669/395 -> 249 us, 1% low 87-88 -> 110 fps (the same
  session's unrecorded stretch: 279 us, 87 fps). The Signal between CE's ECL and the game's Present was the cause.
  The user: "greatly improved, or even fully fixed".

- Signature, inject recording only: the game's Present down-call (`present_call_us`; `[PRESENT STAGE COST]
  forward=` mean/p95/max) takes 2-9 ms on 2.5-12.7% of frames (slow ones average 3.5-3.8 ms, one every 40-60 ms).
  The limiter shortens the next wait, yet that interval still lands near 8.9 ms: 74/75, 104/106, 87/87 and 28/30 of
  the >9 ms intervals per recording sit within one frame of a slow Present. Overlay pacing health fell to 1% low
  70.7 (95-97 clean). Idle stretches and DXGI-duplication recordings of the same process stay at forward ~200/250/
  <600 us; CE's capture copy stays 50-80 us, fence waits 0; GPU 3D ~48%, NVENC ~64%.
- Not a regression: 0.1.7352 (the 10-26 session's archived binaries, `20261028_013522`: 10.2% slow) and 0.1.7157
  (`20261028_013720`: 4.4%) stall the same way today. The one clean run, `20261026_234233` (game run from 10-27
  00:19: 114,920 inject-recorded frames, 4 slow Presents, forward 236/295/567 us), had the same build, settings
  (3840x2160 P010 av1_nvenc 120 fps CFR, 140 fps limiter, `gpu_priority=7`, A/V-sync `injectDelayFrames=4`,
  2-buffer swapchain) and GPU load; why it did not stall is unexplained.
- Ruled out by A/B: NVIDIA driver 621.01 vs 621.12, DXUP 22.10 vs 26.10 build, the face camera, Windows/game updates.
  `WaitBackbufferFrameLatency` inside the forward is a no-op here (`no override (count=-1)`), so the stall is the
  real DXGI Present.
- Root-cause candidate: `SharedCaptureD3D12::CaptureFrame` queued `Signal(sharedFence)` on the game queue right
  after its copy ECL and before the game's Present. The overlay has deferred its own fence Signal past Present since
  2026-03 ("avoids NVIDIA driver stall between Signal and Present", `dx12_hook_process_session_draw_submission.cpp`);
  the capture never did, and only inject capture puts that shape on the queue. Fix: `capture_completion_signal.h`
  holds the Signal (`HeldCompletionSignal`) on the plain foreground non-FG route and queues it at the post-Present
  boundary (`FlushDX12DeferredOverlaySignalAfterHookedPresent`, the wrapper's flush, the core's early returns);
  the next copy, Reset and resize queue a Signal nobody released (`queued a held completion Signal at the ...`).
- New `[PRESENT STALL]` diagnostics (`present_stall_attribution.h`) sample the copy fence around every D3D12
  down-call. Reading the next HotS run: no slow captured calls -> the Signal order was it; slow calls with
  `inFlightAtEntry>=2` -> GPU behind; `retiredDuringCall=0` -> a wait the copy fence cannot explain (flip queue,
  display, lock). Present1's detour has no probe yet (its file is over the 800-line ceiling).
- The frame-ring hand-off fix held on hardware: every inject detach logged ingest == write (e.g.
  `ring(ingest=6147 read=6141 write=6147)`, six encoder leases still out) and every following recording started.

### 2026-10-28 - HotS: every recording after the first failed, the detach split the ingest thread across two rings

- Session `20261027_235200` (0.1.7358): r0002 recorded fine; r0003 and r0004 failed at start with `Inject recording
  rejected corrupt shared frame ring (write=14343 ingest=14342 read=14343 ...)`. At r0002's stop the media log has
  `Detached from the shared live state` and, in the same millisecond, `[Inject Thread] Lag detected! Dropping
  4294952952 frames`; the inject thread then took 3.9 s to stop (`[Inject Perf] Input: 4294953058 ... RingR: 0
  RingW: 0`). The hook log shows no ring reset: the zeroed ring was the media session's private copy.
- Root cause and fix in `cfr-capture-sync.md` (frame-ring hand-off bullet): the ingest thread kept running across
  `DetachFromSharedLiveState`; now the session joins it and discards the unread suffix on the live ring first.
  `DetachFrameRingHandoffTest` failed on the old code (thread still joinable, live ingest left at 1) and passes
  300 repeats with the fix. Same retarget hazard family as the CFR repeat-reason counters (`wgc-capture.md`): no
  thread may carry protocol state across the retarget.
- Follow-up in the same area: the ingest cursor became `InjectFrameRingIngestCursor` (one bound ring; ingestion
  published before any acknowledgement, so a consumer dying mid-frame cannot leave `read > ingest`; a cursor fault
  resyncs within the 32-slot window and logs an error instead of walking 2^32 positions).
- Superseded by the Present-stall entry above (this session's CSV already shows present p99 4.1 ms while
  recording). First read, from the logs alone: in the 140 fps phases the
  overlay's display-change stddev was 590/344/373 us while recording vs 187-664 us afterwards, and CE's per-frame
  hook cost stayed ~0.3 ms (capture 50-60 us) with no limiter lateness. The game's own hitches at 00:09:32-36 (up
  to 305 ms with CE idle in its present path) and HotS's ~24-30 fps background cap while unfocused
  (00:09:53-00:11:28, also seen at 00:26 and 00:29 without recording) dominate. GPU 3D 40-49%, NVENC 61-66%: no
  saturation. Face camera ran 1280x720 at ~25 fps GPU-resident in the media process.
