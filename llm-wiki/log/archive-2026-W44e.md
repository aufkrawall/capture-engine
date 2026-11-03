# llm-wiki Log Archive

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
