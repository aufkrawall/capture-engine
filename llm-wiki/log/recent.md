# llm-wiki Log

### 2026-10-30 - Application profiles can select executable image paths

- Discovery ABI 73 publishes an atomic generation identifying an immutable, separately mapped selector list. Readers
  open the named generation and retry only when publication changes concurrently; held views remain valid through reload.
  Startup fails if the complete policy cannot be published, and a failed reload retains the prior active configuration.
- Profile-local audio exclusions now cross the worker boundary in an inherited, length-delimited mapping rather than
  process arguments. V3 validates the policy on both sides; V1/V2 remain compatible. Windows command-line size no
  longer caps the number of exclusions. The transport parser is also covered by the runtime-argument fuzzer.
- Screen-grab selection resolves the actual window owner's most-specific profile before considering the route;
  WGC and DXGI candidates are evaluated together, and a window-title match cannot bypass the selected process path.
  Profile-local audio workers receive the selectors that outrank their own profile, and `SelectProcessTreeRootByName`
  excludes both ancestors and descendants of any excluded node (plus sibling branches under a target-matching ancestor
  with an excluded descendant), so a generic profile never binds the launcher parent or child worker of a process tree
  owned by a more-specific path profile. Unresolved paths and exclusion deferrals are counted and logged rate-limited
  through `ChangeGate`. Exclusions are carried to the monitoring worker and survive worker restarts.
- `[Profile.*] process` now accepts a Windows path suffix as well as a basename. A selector with `/` or `\` matches a
  complete path-component suffix; `Modded\game.exe` matches any matching installation, while a drive-qualified path
  targets one installation. Longer path selectors take precedence over shorter paths and name-only fallbacks, and equal
  specificity keeps the existing later-profile-wins rule.
- Profile process values are read through a UTF-8 path (`ConfigSource::ReadValueUtf8`) and process identities come
  from `QueryFullProcessImageNameW` with a handleless `NtQuerySystemInformation(SystemProcessIdInformation)` + DOS
  drive-mapping fallback when `OpenProcess` is denied, so CJK/Cyrillic names and folders do not pass through CP_ACP.
  Discovery and Vulkan target lists carry the same selectors plus internal deny entries, allowing a path-specific
  non-injection profile to shadow a broader name selector safely.
- Identity-aware matching now covers config overrides, injection and startup-launch routing, WGC/DXGI selection,
  config publication cache keys, desktop overlay settings, window heartbeat and process-loopback audio. Regression tests
  cover full/partial paths, path boundaries, slash normalization, precedence, drive-colon parsing and Unicode matching.

### 2026-10-30 - Release 0.1.7158: sanitizer stage failed on start-address tests (ASan interposes CreateThread)

- First dispatch `41091557390` died in the product build step when this PC lost DNS mid-run (`ffnvcodec` clone,
  `Could not resolve host`); no tag was created, so the plain re-dispatch was legitimate.
- Second dispatch `41095574352`: build, native tests and flow tests passed, but the isolated sanitizer stage failed
  5 tests (`ToolThreadSuspensionTest` x2, `ThreadQuiescenceRefusalTest.LivePeerThatRefuses...`,
  `ProcessThreadStateTest` x2). Cause: they compare the kernel's Win32 start address with the test's own thread
  function, but ASan on Windows interposes `CreateThread` and starts every thread through its own thunk. Product
  code is unaffected (it queries threads of unsanitized game processes).
- Fix: `tests/exact_start_thread_support.h` creates such threads with `NtCreateThreadEx`, which is not interposed.
  Any new test that identifies a thread by start address must use it. Local check: `python build.py --sanitize
  --run-tests --gtest-filter=<suites> --skip-updates --concise` (about 13 min, compiles the sanitized tree).
- Diagnostics from a failed release live in `runner-work/.../build/verification/<run>/`; the per-assertion text is
  in the sanitized `build/stages/sanitize/tests/unit_tests.exe` output, not in `sanitize_regression.log`.

### 2026-10-29 - WindowHeartbeat: from WM_NULL pings to Present-stall recovery

- Assessment: the 250 ms `WM_NULL` could not address the reported MPO background freeze - a window a resize
  unsticks still pumps messages; the stuck part is presentation. User asked to change the feature without waiting
  for a repro (last occurrence weeks ago, none on demand).
- Now: DXGI Present start/stop (stop ids added to the shared provider set) from the service display trace; a call
  open past `TdrDelay` in a background profiled process gets a 1 px move-and-back, then a 1 px resize-and-back,
  one attempt per stall, every step logged. Details and limits: `window-heartbeat.md`.
- First hardware session `20261029_181926` (0.1.7377, Fortnite with EAC, background-limited to 30 fps): lease taken
  and released with each focus change, start/stop pairing exact (3595/3595 in 2 min), no stall, so no nudge yet.

### 2026-10-29 - vkd3d flip metering: IMMEDIATE swapchain refused too; DLSS-G paces itself again; present timing next

- GTA `20261027_155313` (0.1.7358, `VKD3D_SWAPCHAIN_PRESENT_MODE=IMMEDIATE`): vkd3d created `presentMode=0` without
  a mode list, the driver still answered `presentConfigFeedback=0x1`, so present mode is ruled out. The carried-back
  feedback (3a993195) worked: sl.dlss_g switched to `numFramesPerBatch=0` after the refusal and CPU-paced; pacing is
  back to the pre-bridge level (stddev 1.8-2.0 ms, displayJag 3.1-3.7 ms, presentJag ~0.48 ms at ~122 fps, 2x).
  The user: no longer badly broken, not flat like native D3D12 (metered there).
- Next suspect: vkd3d's swapchain flag `0x200` (`VK_EXT_present_timing`, vkd3d sets its own target times); A/B with
  `VKD3D_DISABLE_EXTENSIONS=VK_EXT_present_timing` (third-party-dll-loading.md). No code change.
- Same day, A/B runs: `122807` (`vsync_mode=fifo`) - DLSS-G never requested metering (CE answers driver vsync as
  force-on); `123525` (vsync untouched, present timing off) - refused `0x1` again. Every probe in both A/B runs was
  a 1-frame warm-up batch; Remix never asks for fewer than 2. Bridge now presents single-frame batches unmetered
  (`kMinMeteredFramesPerBatch`, `FlipMeteringBridge.SingleFrameWarmUpBatchesNeverPutMeteringToTheDriver`, fails on
  the old threshold) and the hook logs SetFlipConfig's V2 fields. Real-batch acceptance still untested.
- `130342` (0.1.7374): the first real 2-frame batches were refused `0x1` too - vkd3d's swapchain itself is refused.
  Under `vsync_mode=fifo` DLSS-G sends `numFramesPerBatch=0` with batch timing from its first call. Next A/B: the
  closest env-var shape to Remix's swapchain (third-party-dll-loading.md).
- `131715`: that A/B (IMMEDIATE, flags 0, no per-present structs), with and without NVCP vsync - refused `0x1`
  both times. Shape, Reflex, the layered presenter, CE's DXGI hooks and the driver ruled out; left are
  process-level conditions (three metering-enabled vkd3d devices alive, vkd3d's present thread). User: NVCP
  vsync caps at 144 correctly, but inside the VRR range the line is not flat.
- Probe (`build/vk-metering-probe`, agent-run with the user's go): NVIDIA's Vulkan driver refuses the first 1-4
  metering requests on every new swapchain, then meters ~99 % of batches, under every GTA-like condition; stopping
  after the first refusal reproduces GTA. Bridge now keeps asking with DLSS-G's presented count while the latest
  answer is a refusal; the hook logs the graphics env vars the game sees. Pending: GTA with vsync off / NVCP vsync.
- `135421` (0.1.7375): the keep-asking worked (882 asks) and every one was refused - the warm-up was not GTA's
  blocker. The probe with `pEngineName` `vkd3d` or `DXVK` is refused 100 % (`vkd3d` lands on the same D3D11-layered
  presenter as GTA); the user recalled NVIDIA withholding DirectFlip from DXVK unless a profile bit allows it:
  `0x20324987` = `0x00080004` (third-party-dll-loading.md). Pending: GTA with that bit on its profile.
- `141337`: the bit (and "Prefer native") left GTA on the D3D11 presenter, all refused. Agent-run DRS writes on
  GTA's profile plus the probe ruled out every flag bit; the driver matches `vkd3d`/`DXVK` exactly. CE's layer
  now shows the driver `vkd3d-proton` for vkd3d instances in NVIDIA + Streamline processes. Pending: GTA run.

### 2026-10-28 - HotS: VALIDATED on 0.1.7372 - mic off is clean with the immediate capture Signal; diagnostics fire

- Session `20261028_165717` (0.1.7372: held Signal reverted, period lock, mic glitch monitor). Recording 1 on the
  face camera's mic (`Endpoint resolved: 'Mikrofon (USB Microphone)' (capture)`): `[PRESENT STALL]` slow 14 / 180 /
  112 per 10 s; `lock=OUTSIDE CLOCK 10.00ms ... Z=43.7` (peaks 10.00/25.01/19.99 ms) then `39.99ms R=0.71 Z=56.5`;
  `WARNING: input device 'Mikrofon (USB Microphone)' keeps losing capture data: 36 ... in 5.0s, typically one every
  40.0 ms` 15 s in, 439 discontinuities in all. Recording 2 on `Microphone (USB2.0 Microphone)`: slow 0 / 0 / 0, one
  discontinuity (stream start). The user saw it smooth.
- The 10 ms lock is a harmonic: offline that window folds at 20 ms R=0.52 and 25 ms 0.45 (40 Hz, the frame-rate alias
  of 100 Hz) but 40 ms only 0.24 - the mic's pattern varies, the lock verdict is what to read; its period hints.
- The end-of-stream line said the mic "stopped losing capture data"; it now says it was still losing data when its
  stream ended (`CaptureGlitchReport::streamEnded`).

### 2026-10-28 - HotS: the Present stall was the face camera's mono USB microphone (resolved by the user)

- The mono 48 kHz mic of the earlier entries is the face camera's built-in microphone, set as the default capture
  device (absent in `092247`/`092712` because the camera was unplugged). The user disabled it; session
  `20261028_161804` (0.1.7371, stereo 48 kHz default mic, no discontinuity streak, priority auto(high)/7): 3/3792
  capture-phase frames >= 2 ms (0.1%) vs 4.6-6.9% before; `[PRESENT STALL]` windows `slow=0`, `0`, `1`; flip
  `scanout` (programmed->screen) mean 77-97 us vs 1.2-2.1 ms; captured `runtime` p95 229-246 us.
- Follow-ups the same day: the held capture completion Signal (`f0c23d01`, `70778bd4`) reverted to the immediate
  Signal (it never changed the stall; costs media latency); `[PRESENT STALL] period` lock line and the
  `[AudioCapture]` device-name / persistent-discontinuity warnings added (`cfr-capture-sync.md`,
  `multi-audio-capture.md`). Pending HW: one HotS recording without that mic on the reverted build.
- Correction: "copied vs uncopied" was confounded with "recording vs not" - every Present was copied while recording
  and the mic was only open then - so the copy itself is not shown to matter.
- Not explained: why the copied frames (not uncopied ones) waited in the kernel present submit on the mic's 25 Hz
  clock. CE only opens that mic while recording, which is why the stall was recording-only. Diagnostic gaps: the
  media log names neither the microphone nor the camera, and WASAPI discontinuities stop after 8 lines, so a mic
  that glitches throughout a recording is invisible.

### 2026-10-28 - HotS: not the face camera either; the 25 Hz clock matches the mono USB microphone

- Session `20261028_161224` (0.1.7371, `[FaceCamera] enabled=false`: no `[FaceCamera]` line; priority back to
  auto(high)/7): stalls unchanged - `slow=179/1411`, `136/1399`, stack again `NtDxgkSubmitPresentToHwQueue`, slow
  calls still lock to 39.98 ms (R=0.47, all frames 0.011). Copy dependence holds: first window 29/283 captured vs
  3/979 uncaptured calls slow.
- The default microphone is the remaining 25 Hz source in these runs. Per HotS session (perf CSV, capture-phase
  frames with `present_call_us >= 2 ms`): the two recordings on a stereo 48 kHz default mic (`092247`, `092712`)
  had 0.0% / 0.1% slow; 8 of 9 on the mono 48 kHz mic stalled at 1.2-6.9% (`100931` 0.1%). The mono mic's
  WASAPI `DATA_DISCONTINUITY` flags step exactly 1920 frames (40 ms) - from the first packets here, from the first
  stall in `122156`-`160327`; the stereo mic shows none. The log stops after 8 occurrences, so whether the mic keeps
  glitching through a whole recording is not visible. This PC has two USB mics (`USB2.0 Microphone`,
  `USB Microphone`); the media log names neither.
- Working model (unproven): something on a 25 Hz clock tied to the mono mic's stream (its USB/driver activity
  delaying DPCs is one candidate: it would also explain the 1.3-2 ms programmed->screen delay, since screen time is
  a DPC timestamp) delays what the kernel present submit of a *copied* frame waits for. Next: A/B
  `[Microphone] enabled=false` (or the stereo mic as default) on the same build.

### 2026-10-28 - HotS: GPU priority is not it; the stalled Presents lock to a 25 Hz clock

- Session `20261028_160327` (0.1.7371, `[Performance] gpu_scheduling_priority=off` + `gpu_priority=0`; media logs
  `GPU scheduling priority class disabled` and `Encoder device GPU priority=0`): stalls unchanged - `slow=127/1413`
  and `139/1405`, every stack sample again in `win32u!NtDxgkSubmitPresentToHwQueue`. The priority A/B is negative.
- Period sweep over the per-frame `present_call_us` (CSV, capture-phase frames, slow >= 2 ms, Rayleigh R vs all
  frames as control): the slow calls lock to exactly 40.00 ms / 20.00 ms - `160327` R=0.40 @ 19.98 ms, 0.36 @ 40.00;
  `155443` 0.53 @ 39.98; `145319` 0.55 @ 20.00; `131959` 0.71 @ 40.00; `122156` 0.25 @ 39.94 (0.34 @ 15.00); all
  frames R <= 0.02. (The earlier "35 Hz, no lock to the 30 fps face camera" test used the nominal camera rate.)
- 25 Hz is the face camera's real rate: `received=509` frames in ~21 s, ~126 `cameraUploads` per 600 composites
  (requested 30 fps; a 50 Hz anti-flicker exposure gives exactly 40 ms). The microphone's WASAPI discontinuities
  also step exactly 1920 frames (40 ms at 48 kHz), and their streak (`occurrence=2..`) starts 3-80 ms from the first
  slow Present in 5/5 stall sessions while its delay after recording start varies 0-5 s.
- Every stall session had the face camera compositing (`FaceCamera] GPU composite`); the one HotS recording with
  stall instrumentation and no face camera (`20261028_092247`, 0.1.7362, priority high/7) had 0 slow calls in 3809
  copied Presents. `100931` (face camera on) stalled only 17 times in 19482 - the trigger is not every camera run.
- The camera reader uses MF hardware transforms with CE's D3D11 device manager
  (`mediaengine/video/face_camera_capture.cpp`); the camera's friendly name is not logged. Next: A/B the face camera
  off on the same build.

### 2026-10-28 - HotS: the stalled Present waits inside the kernel's hardware-queue present submit

- Session `20261028_155443` (0.1.7371, first run with the stack sampler; same HotS/DXUP setup, HAGS on
  (`hagsEnabled=1`), media `gpu_priority=7` and GPU scheduling class `auto(high)`, face camera active). One 30 s
  inject recording 15:56:31.5-15:57:01.6. No slow call in the first ~4 s of copies (562 copies before the first),
  then `slow=120/1407`, `178/1406`, `43/1407` (window holding the stop); `returnedAfter=submit` 118/179/43.
- All 10 stack samples (8 burst + 2 interval, blocked 1.61-2.02 ms, presenting thread tid 25276) show one stack,
  symbolized with public symbols (win32u/dxgi 10.0.26100.9711, D3D12Core 10.0.26100.9549) and the session PDB:
  `win32u!NtDxgkSubmitPresentToHwQueue+0x14` <- `D3D12Core!CallAndLogImpl<D3DKMT_SUBMITPRESENTTOHWQUEUE>` <-
  `D3D12Core!CGraphicsCommandQueue::SubmitPresent+0x1a3` <- `dxgi!CD3D12Device::PresentExtended` <-
  `dxgi!CFlipPresentToDWM::FlipPresentCore` <- `dxgi!CDXGISwapChain::FlipPresentToDWM` <- `PresentImplCore` <-
  `PresentImpl` <- `Present` <- CE `CallOriginalPresent` / `ExecutePresentCore` / `DetourPresent` <- DXUP
  `Support64\d3d11.dll` present thread. So the wait is inside the kernel's HAGS present submission (DxgKrnl or the
  NVIDIA KMD/GSP behind it), not in DXUP, CE, DXGI user mode, D3D12Core or the NVIDIA UMD; the flip milestones
  already showed the packet is queued only when the call is released (submit +2.4..4.4 ms, screen ~0.1-1.5 ms later).
- Open: what DxgKrnl/KMD waits on there. Candidates: HAGS scheduler/firmware latency while the media process runs
  high-class, priority-7 GPU work (the programmed->screen delay during recording points the same way), or the
  shared-fence/shared-texture handling of the copied frame. Kernel stacks need a WPR (GPU+CPU, CSwitch/ReadyThread)
  trace; the cheap A/B is `[Performance] gpu_scheduling_priority=off` + `gpu_priority=0`.

### 2026-10-28 - HotS: the stalled Present blocks before its kernel submit (flip timing); stack sampler added

- Session `20261028_145319` (0.1.7366, first run with flip milestones; game FIFO vsync on, the prerender limit 1 now
  set in DXUP itself, so CE logs no `SetMaximumFrameLatency`; the swapchain has `flags=0x40` (frame-latency
  waitable); DXUP presents from its own thread T659C while the game thread is T0A58). One 12 s inject recording.
  Milestones arrive: `milestones(ready=6959 programmed=6956)`, flip lines `placed=1404-1413 unplaced=0 unmatched=0`.
- 156 slow calls, 155 `returnedAfter=submit` (1 `onScreen`). Per call: DXGI PresentStart at +1-3 us, the kernel
  present-packet submit at +2.1..4.8 ms, return ~10 us after it, packet ready ~100 us after submit; the copy had
  finished 1.9-3.8 ms before the call (CE's limiter waits between copy and Present); the previous frame was on
  screen 3.7-6.7 ms before the call. Normal calls submit at +0.19 ms. So the wait is inside Present *before the
  kernel submit* (DXGI / D3D12 runtime / NVIDIA UMD, or a kernel lock taken before the packet is queued): not the
  GPU, not the display, not frame-latency throttling in Present (waitable swapchain).
- Separately, while recording, frames sit 1.4-2.4 ms (p95 3.4-4.7 ms) between flip programmed and screen vs
  0.13-0.16 ms before/after (all frames were copied in this recording, so copied-vs-not = recording-vs-not); even
  slow calls whose previous frame flipped 9-10 ms earlier wait ~1.5 ms there, so a VRR ceiling does not explain it.
- Stall ends cluster at ~4-frame spacing (Rayleigh R=0.60 at 35 Hz vs 0.02 for all frames); no lock to the
  120 Hz encoder clock or the 30 fps face camera (both active).
- Added the same day: `hook/present/present_stall_stack.h` samples the presenting thread's stack when a D3D12
  down-call is still inside 1.5 ms after entry (reading guide `cfr-capture-sync.md`, "Stack samples"). Pending a
  HotS run; symbolize the module+offset frames locally with public symbols.

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
