# llm-wiki Log

### 2026-10-13 - Isolate display-timing delivery from other ETW writer failures

- The HOTS/DXUP latency feed was already empty before the RTSS compatibility change.
  Trace startup succeeds and the service binary matches the product, but meaningful
  present/flip/blank/input counters remain zero.
- Hardware baseline/independent/baseline probes isolate the cause: default mode sees
  only MPO-ID records; one independent-session flag restores native DXGI Presents,
  submission and display-completion events; reverting only that flag loses them again.
- Both local and privileged-service sessions now use independent mode through the
  existing properties helper. Filters, privileges, render/capture/pacing code are unchanged.
  Startup logs query and name the actual mode. The new native property regression fails
  before the change and passes after; the committed bounded diagnostic reproduces the
  real Windows behavior without changing another collector.
- Closing gate `20261013_000021_build_7133` passed: unfiltered native suite, 41 Python
  groups, all 64 WARP FG flows, PE/privacy checks and setup 0.1.7133 (39,778,532 bytes).
  Native display-timing regressions, product syntax and file-scoped Python diagnostics pass.
  Installed HOTS/DXUP session `20261013_000827`: user confirms latency is visible again;
  independent mode is queried successfully, display events recover, and overlay estimates
  settle at 20.8-21.0 ms around 138 fps. All 2408 display samples are associated; early
  no-timestamp warnings precede the first game Present. Frame start/input wait remain modelled.
  Evidence and commands: [display-change timing](../display-change-timing.md).

### 2026-10-12 - Reserve native Present entries before late RTSS injection

- Session `20261012_224006`, 0.1.7129, HOTS + DXUP: all five off-Present probes see
  CE's FF 25 entry after RTSS loads; immediate callers then alternate RTSS/DXUP.
  CE frame work remains about 0.3 ms. This supports shared-entry interference;
  the precise RTSS-internal source of the stalls remains unmeasured.
- Native x64 system-DXGI Present/Present1 now take the existing body hook before
  any overlay loads, with a 14-byte entry reservation. Foreign/x86 targets and
  body refusal keep existing fallbacks. No game-specific routing or GPU copies.
- The new isolated late-entry regression fails before the change and passes after:
  two restore/call/re-patch install/removal cycles, both methods, native/translated
  callers, 16 exactly-once overlay/gamma/capture outputs and no D3D12 errors.
- Full-flow checks exposed three frontend/foreign-predecessor failures from applying
  the reserved-entry vtable exclusion globally. The corrected decision checks both
  methods and preserves independent vtable interception without a loaded overlay.
- The bounded entry probe now decodes a jump even without a CE entry record,
  so the reserved-entry retest can attribute RTSS instead of reporting no jump.
- Commit `4edc1271`; closing gate `20261012_230801_build_7132` passed in 333.4 s:
  unfiltered native suite, 41 Python groups, all 64 WARP FG flows, PE/privacy checks
  and setup 0.1.7132 (39,779,286 bytes). No optional lint/sanitizer/fuzz/runtime matrix.
- Installed `20261012_231419` retest on 0.1.7132: user reports the issue appears fixed;
  all five probes confirm CE owns no entry bytes, and RTSS/restored native entry samples
  remain on the body route. After-load pacing sample is 138.3 fps, 132.5 fps 1% low.
  The pre-existing all-zero ETW latency feed is fixed and retested separately above.
  See [late DXGI overlay injection](../dxgi-late-overlay-injection.md) for boundaries.
- Ordinary resume needs explicit `--run-tests` to repeat the test gate; a successful
  tests-only run invalidates resume. Source/observed evidence is in `build.py.md`.

### 2026-10-12 - Name and instrument the inject-after-CE overlay join state

- Session `20261012_205937` (0.1.7128, HOTS + DXUP, user-reported): RTSS injected ~60 s
  after CE. The game then stutters (frame clusters of 2-4 stretched frames, 17-32 ms,
  every ~190-200 ms; 115 fps average vs a perfectly smooth 138 fps before the injection)
  and the RTSS OSD blinks per frame. CE's own per-frame work is unchanged (0.2-0.4 ms;
  `[PRESENT STAGE COST] own` ~0.4 ms) and the stretched frames are outside CE's detour;
  the corrected admission is confirmed working in-session (`Overlay chain ownership ...
  action=process-output`, coverage ledger clean all run).
- Same game + DXUP with RTSS loaded BEFORE CE's install (`20261012_171627`, `_171826`,
  `_193819` run 2) takes the leave-entry/deep-body path and runs smooth (run 2: 138 fps,
  p95 7.4 ms, no clusters) with a stable OSD - user-confirmed, so the inject-after-CE
  order is the variable. `20261012_193819` also shows an RTSS unload/reload mid-run (the
  "second RTSS load" of the coexistence page) and `20261012_205937` shows RTSS's
  `nvapi64.dll` load re-triggering CE's Reflex limiter init (`Ready`, no active limiter -
  not causal). The 5-6.6 ms `ExecuteCommandLists SLOW` values are driver submission
  backpressure at 4K and occur equally in the smooth runs.
- Working theory (UNPROVEN, needs the new evidence): the five-byte entry dance. RTSS
  first saves CE's prepend bytes as its "original" when it joins late (pristine bytes when
  it hooks first), and its restore/call/re-patch cycle can leave CE's state at the entry
  between calls, bypassing RTSS's handler on some frames (per-frame OSD blink) and
  driving re-hook churn (the ~200 ms stalls). The old entry-ownership watch cannot see
  this: it samples inside CE's detour, i.e. inside RTSS's restore window, where the entry
  always reads as CE's.
- Landed: the late-join warning now fires for ANY tracked overlay meeting CE's prepend
  (`>= 1`, it was gated on `>= 2` and missed the single-RTSS case); `Present entry chain
  state` samples the live entry bytes and their owner between presents on the hook thread
  (armed by the overlay load, five samples over ~4 s); `DetourPresent: Present chain
  participant changed` reports caller-class transitions per Present (metered); the
  `[HookThreadStages]` line reports `wakes(event/timeout/notify)` - the hook thread is
  notification-driven at 50-90 passes/s after RTSS loads (up to 385/s during load storms),
  which is churn present in the smooth runs too but worth attributing. Present admission
  also skips its startup-blocking stack walk unless the swapchain is a tagged helper.
- Open: converge the late-join state to the proven below-chain topology (deferred,
  ownership-checked entry handback). Deliberately NOT landed: RTSS's saved-chain state
  makes a retroactive un-prepend fragile (see the coexistence page), and the probe has to
  name the actual dance first. Next step: user retest with the instrumented build.

### 2026-10-12 - FSR no-callback screenshots serviced on the game queue

- Session `20261012_194528` (GTA5 Enhanced, native FSR FG, SDR B8G8R8A8): the screenshot request
  stayed Pending for 15 s (`wait=258` = `WAIT_TIMEOUT`) and the GDI fallback then took ~1 s. The
  presenter ran `kSkipBundleCovers` (runtime-owned no-callback route), so no `ProcessFrame` or PostSL
  serviced it; the stage cost line for `role=runtime_presenter` shows `overlay n=0`.
- Fix: `DX12_ServiceFSRNoCallbackScreenshotIfPending` composites the proxy backbuffer and the game UI
  resource on the game queue in the proxy prework, draws the overlay on a reserved renderer slot
  (`screenshotSlot`), and reads back through the normal path. Details: `screenshots.md`,
  "Native FSR No-Callback Route".
- Validation: targeted gtests and the closing incremental gate. Live GTA FSR FG run pending.
  The FSR presenter fallback (proxy hook not live) is still unserviced.

### 2026-10-12 - Preserve output ownership through late overlay forwarding

- DXUP session `20261012_185851`, 0.1.7124: after RTSS loaded, the same native game
  output was bypassed as a helper because of its immediate Present caller. Both methods
  now share creation-identity admission, with ordinary helper exclusions and startup-tag handoff.
- The isolated native/translated regression processed only 8/16 frames with old admission;
  corrected admission preserves overlay, gamma and capture once per frame. Real installed
  HOTS/DXUP/RTSS visual retest remains pending; the active game and installation were untouched.
- Same session, graphics load: repeated D3D11 runtime loads no longer trigger whole-process import sweeps; late importers get bounded local patches. Isolated regressions pass; installed retest pending.

### 2026-10-12 - Complete full-scope verification of logging and fuzz-invocation changes

- The clean gate exposed existing warning blockers in recent regressions: cleanup trace allocation, native multilevel-pointer conversions, GoogleTest optional assertions not visible to the analyzer, and runtime-selected ownership in a placement wrapper. Cleanup traces now use bounded enum storage, optional reads have explicit guards, and placement/heap test ownership is selected by type; runtime behavior is unchanged.
- Clean product validation and the resumed final gate on identity 0.1.7124 passed native tests, all 61 WARP FG flows, Python tool groups, x64 ASan/UBSan, PE/privacy checks and installer packaging. Compiler-warning counts automatically tightened 667 -> 601 with no increases; existing C++ formatting differences remain advisory under the narrow-edit rule.
- Six native fuzz targets passed at 10 seconds each; the real standalone CLI guard also passed at one second per target and preserved build-header/installer hashes. Fresh game/capture logs and decoded media were not part of this log/diagnostic change verification.

### 2026-10-12 - Make standalone fuzzing preserve the product build

- An explicit `--run-fuzz` previously selected the default clean product path before running its harnesses. `should_skip_product_build` now defaults standalone fuzzing to the product skip before identity resolution; explicit build/test/verification modes remain composable, and `--no-build` retains precedence.
- The build-policy Python group now includes focused invocation regressions. The real standalone command passed all six targets and preserved the generated build-header and installer hashes. The console labels product work disabled. `AGENTS.md` keeps explicit `--no-build` in the preferred bounded fuzz command; build/fuzz topic pages document the default and combined scope.

### 2026-10-12 - Repair shared logging reservation, restart and record bounds

- The MPSC producer retained an old write cursor across newer consumer snapshots, so unsigned subtraction could falsely report full. Logger attachment skipped to the reservation cursor, losing accepted backlog and retaining stale committed flags. Hook/Vulkan slot formatting also silently truncated overlong records.
- `log_ring.h` now owns both producer paths and the service consumer: coherent snapshots, complete payload fit, commit ordering, released-slot publication before file I/O, and restart at the actual read cursor. Full/oversized Vulkan fallback records now append to the same session file with counted failure recovery. Shutdown drains one bounded committed snapshot; the shared layout is unchanged.
- Six deterministic ring regressions plus production wiring passed; the wiring fails on old sources. Digest regression failed before merged-record recovery and passes after; session `20261012_153926` now correctly reports three merged records recovered and one actual missing record, with duplicate sequence diagnostics and malformed-prefix tests.

### 2026-10-12 - Preserve complete records across hook/service append writers

- `20261012_153926` contained three records glued into neighboring rows and one explicitly counted fallback drop. Both append writers wrote message and CRLF separately; a concurrent writer could land between them.
- Hook and service now submit complete records through `AppendLogRecord`. A contended hook cache lock uses an independent handle rather than dropping; real path/open/write failures remain counted, and the service retries file handles after failures. The process-name initialization publication is atomic.
- Native regression verifies 400 intact, unique records from two concurrent append handles; old production wiring fails the source regression. Logging ring reservation/discovery is a separate follow-up.

### 2026-10-12 - Align app-audio latency diagnostics with the protected CFR drain target

- Build 0.1.7120 sessions `20261012_153926` / `20261012_161027`: all three recordings saved and finalized healthy, both audio tracks matched their sample targets exactly, and post-mux probes differed by only 1 us of timestamp rounding. The DXGI run's 6356 repeats matched its source/delivery lower bound with zero excess or policy-added repeats; zero-frame delivery windows explain the longest 2983 ms hold.
- The DXGI run reported 75 app-latency warnings while the drain stayed `within_slack`: diagnostics compared a roughly 335 ms backlog against raw target dips as low as 60 ms, although the drain already protected a trailing peak. Live excess averaged 4 ms, with no overruns, underruns, trims or resyncs. Warnings and distributions now share the protected target; capture, resampling and playout policy are unchanged.
- Focused regression checks include production wiring (failed before the consumer change), transient target dips, hold release and real excess. Existing logs retain the old warning semantics; fresh recordings are required to observe the corrected live distribution.

### 2026-10-12 - Correct reviewed transaction and lifetime boundaries

- Implemented the verified pacing, retirement, audio, gamma, transport, override, PE/crash and reload issues from the seven-day review; qualified unsupported causal claims against production sources.
- Current contracts, source anchors, regressions and remaining hardware validation are in [transaction and retirement invariants](../transaction-and-retirement-invariants.md).
- Closing gate 20261012_144308_build_7117: native unit tests, 61 FG flow scenarios and the Python self-tests passed; installer captureengine-setup-0.1.7117.exe packaged. Earlier bounded fuzzing (six targets, 10 s then extended) passed before the final translation-owner and retirement edits; re-run `--run-fuzz` if parser inputs change again.

### 2026-10-12 - Add `[ThirdParty] custom_dll_paths` for user-supplied third-party DLL lists

- Added `[ThirdParty] custom_dll_paths` (and per-profile `ThirdParty.custom_dll_paths`, with `off`/`none` to clear a global list) in `common/config/config_load_third_party.cpp` and `captureengine/config.ini.template`.
- `ParseThirdPartyCustomDllPaths` preserves spaces inside folder/file names, accepts unquoted or single/double-quoted comma-separated entries (restoring outer quote pairs stripped by Windows INI reading), and deduplicates case/slash variants in order.
- `PreloadConfiguredThirdPartyDlls` (`hook/runtime/main_thirdparty_load.cpp`) loads custom DLLs after `SpecialK -> ReShade -> OptiScaler` through the shared loader-quiescence and tool-thread suspension pipeline, checking duplicates by full path (`AreModulePathsEquivalent`) so custom proxy names such as `dxgi.dll` are not skipped when `System32\dxgi.dll` is mapped.

### 2026-10-12 - Stop failed inject output and bind audio-worker lifetimes

- Session 0.1.7109: first capture finalized normally; the second lost NVENC's device alongside
  NVIDIA GPU-exception events. CE's sustained-failure stop entered an impossible CFR drain,
  leaving an old worker overwriting the next start's shared phase and a 9.97 GB repeated-error log.
- The production iteration runner now prioritizes terminal output failure over retry/debt;
  scheduled and catch-up output all reach failure monitoring. Healthy retries remain supported.
- Removed shared fences never become successful completion or GPU waits; synchronization errors
  propagate, and allocation/wait failure diagnostics carry bounded repeat counts and device evidence.
- Audio workers receive non-inherited kill-on-close jobs atomically at restricted process creation.
  Fifteen abandoned workers with a dead media parent were confirmed and stopped; zero remain.
- Production source safeguards fail against the old sources and pass after byte-exact restoration.
  Real Windows child tests verify atomic job membership, owner-loss cleanup, normal exit and refusal.
- Final unfiltered closing gate passed in 319.8 s: native tests, 41 Python tool groups, all 61 WARP
  FG flows, PE/import/PDB privacy checks and setup 0.1.7111 (39,522,516 bytes). Evidence:
  `build/verification/20261012_020023_build_7111`. Live GPU-fault/game retest remains unavailable;
  the underlying GPU exception's cause is not established. No optional lint/sanitizer/fuzz gate ran.

### 2026-10-12 - Give translated frontend decisions their own observed COM lifetime

- Native private data now carries output evidence only; each frontend decision has an exact
  Release predecessor, in-flight invalidation and final-release retirement without COM retention.
- All four cache regressions fail as assertions with the old owner source and pass after restoration;
  they cover address reuse, reuse before final Release returns and independent frontend decisions.
- Nested Present/Present1 retains two initial device queries per layer and six capture publications;
  actual output has two ownership logs for six frames. Borrowed-IUnknown fallback logs only once.
- Unfiltered closing gate passed in 313.0 s: both hook architectures, the full native suite,
  41 Python tool groups, all 61 WARP FG flows and PE/import/privacy checks. Evidence:
  `build/verification/20261012_001340_build_7109`; setup 0.1.7109 is 39,517,610 bytes.
- Live DXUP retest is unavailable: the recorded game folder no longer has dxgi.dll/d3d11.dll;
  the bounded local probe exited before loading CE. No lint, sanitizer or fuzz gate was run.
