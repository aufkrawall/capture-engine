# llm-wiki Log

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

### 2026-10-11 - Close DXUP cold-load and cached-creation gaps after live-game failure

- Follow-up Heroes of the Storm session `20261011_203755`, CE 0.1.7107: native DXGI was absent
  during both factory-install attempts. Present bootstrap loaded it later, but no creation retry
  ran after Present became ready. An initialization queue was adopted before the output's distinct
  queue; its first overlay submit again removed the device with `0x887A002B`. Microsoft symbols
  and the matching archived CE PDB resolve the supplied fatal-exit dump (post-loss game exit).
- Factory discovery now retries immediately after bootstrap factory creation and before every
  existing service early return. Installation is serialized and reentrant calls refused; absent
  native runtime logging is change-gated. The cold-load boundary regression fails on old sources.
- Creation exports cached before injection also bypass frontend scopes. Covered native creation
  now publishes per-object output evidence; forwarded private data plus a DX11/DX10 device proves
  frontend delegation before first Present. Native DX12/FG proxies retain routing. Real-hook tests
  cover unscoped Present/Present1, video publication, cached API queries and rejected ownership.
  CE's private vtable Present adapter is also recognized as native coverage.
- Restoring all three old source units reproduced the exact first-submit access-denied removal
  in the cold multi-queue DXUP probe and failed the new unscoped ownership regression. Sources
  were restored byte-for-byte before final validation. Final installed-DXUP probes pass 12/12
  covered frames and shared texture/fence/frame metadata at 640x360 and 3840x2160, with one
  transport generation and no DX11 overlay initialization. Cold resize remains covered; included
  and excluded screenshots complete, with 3,018 differing payload bytes at 640x360.
- Unfiltered closing gate `--incremental --run-tests --skip-updates --concise` passed in 346.3 s:
  full native suite, 41 Python tool groups, all 56 FG flows (179.0 s), both hook architectures,
  PE/import/privacy checks and fresh setup `0.1.7108` (39,423,092 bytes). Evidence:
  `build/verification/20261011_205738_build_7108`. No optional sanitizer/lint/fuzz matrix was run.
  Full gameplay and older DXUP frontend binaries remain unverified; private diagnostics stay
  ignored under `build/diagnostics`, outside Git.

### 2026-10-11 - Fix DXUP native queue ownership and duplicate frontend work

- Heroes of the Storm session `20261011_194431`, CE 0.1.7102: native Present was intercepted,
  but factory discovery hooked DXUP's proxy vtable. The real native swapchain create escaped;
  CE used the temporary D3D11 probe's DX12 queue and its first overlay submit removed the device
  with `0x887A002B`, followed by `Present=0x887A0005`. A standalone host with the installed DXUP
  DLLs reproduced that exact removal/stall and was stopped at 20 seconds. Matching archived CE
  PDBs plus Microsoft symbols resolved the manual dump; DXUP's worker was idle after device loss.
- Native factory discovery now matches native Present and retries early system-module absence.
  Probe ECL forwarding preserves exact queue dispatch without adopting renderer state; the probe
  scope includes releases, where the translator submits its teardown initialization work.
- The compatibility probe then exposed both DX11 and DX12 overlay/capture work. Frontend create
  scopes now delegate only after one covered native output for the exact HWND is observed.
  Per-object private data carries the frontend identity without retaining native COM references;
  forwarded private data/QI cannot delegate the native output or misidentify a foreign frontend.
  Native DX12/FG inputs never establish this delegation. Details: `dx12-queue-dispatch.md`.
- Final installed-DXUP checks: 12/12 native overlay frames covered, zero duplicate/native uncovered
  frames and no DX11 overlay initialization; 12 shared video textures, completed fences, ordered
  frame metadata and a single transport generation verified. Included/excluded screenshots both
  completed as valid 640x360 RawV2 payloads (921,664 bytes); excluded pixels were uniform scene
  color, included pixels differed in 3,018 bytes. A separate ResizeBuffers run stayed covered on
  all 12 frames. These probes use native output capture and add no compatibility copy/wait.
- Verification: focused source/policy regressions pass, full native suite and 41 Python tool groups
  passed; final 0.1.7107 resumed closing gate passed both hook architectures, focused native/flow
  tests, PE/import/privacy checks and packaged a 39,421,404-byte setup. The same final flow binary
  then passed all 54 scenarios through `build.run_flow_tests` (163.9 s). No optional sanitizer,
  fuzz or integration matrix was run. Full live-game retest and older DXUP frontend binaries remain
  unverified; supplied game directory contained only its D3D11 frontend. Private sources/logs,
  probe source/binaries and screenshots remain under ignored local diagnostic paths, outside Git.

### 2026-10-11 - Attested stable release v0.1.7102 and persistent runner sync fix

- Pre-release `--no-build --lint` caught `tests/test_config.cpp` at 817 lines (above the 800-line ceiling);
  rebalanced WGC/capture tests into `tests/test_config_part3.cpp` (727 lines) and `[Graphics]` sharpen tests
  into `tests/test_config_gamma.cpp` (164 lines), bringing `tests/test_config.cpp` to 656 lines (`05fb4ec6`).
- First `release-stable.yml` dispatch (`38140961568`) failed in `Sync repository (persistent workspace, never cleans)`
  because `external/` in the runner workspace is junctioned to the dev `external/` tree and commit `f3d86683`
  modified tracked `external/fidelityfx/README.md`, making it appear locally modified to the runner's git index.
  Updated `.github/workflows/release-stable.yml` and `tools/tests/test_workflow_security.py` to use
  `git switch --force -C main $env:GITHUB_SHA`, which reconciles tracked files without touching ignored trees or
  junctions (`7dfc5e11`).
- Dispatched `release-stable.yml` run `38141479599` (`version=0.1.7102`, `attest=always` on `7dfc5e11`). Built
  all 8 FFmpeg source dependencies and custom FFmpeg from source, passed `--verify --verify-clean --portable-archives`
  (full native suite, 17 Python tool self-test groups, 52 FG flow scenarios, ASan/UBSan sanitizer regression pass,
  1,052-TU clang-tidy ratchet, PE hardening and PDB privacy scrub), attested all 6 release assets via Sigstore
  (`gh attestation verify` confirmed), published `v0.1.7102`, verified `release-log-cleanup.yml` (`38145153028`)
  purged the self-hosted run log, and promoted `CHANGELOG.md`.

### 2026-10-11 - Keep Streamline takeover ownership coherent and defer resident hook installation

- Review of `e66054e5` exposed two additional startup windows without changing its entry-only
  activation contract. An entry blocked in runtime loading could lose takeover to a concurrent
  post-init call, then publish the custom runtime after resident imports/hooks were restored.
  `TakeoverDecision` now atomically selects ownership before the loader; peers join the selected
  override and only a final publication failure selects resident fallback after a custom claim.
- A resident-core notification previously performed import restoration and installed Streamline
  hooks under the loader lock. That takes the module-install mutex also held by workers across
  loader-dependent lookups. Notifications now select resident routing and queue cleanup only;
  bootstrap/monitor service and `StreamlineHook::Init` drain it outside the loader callback.
  Superseded-module and active-interposer queries recognize resident ownership before cleanup,
  so cached thunks already forward to the resident and detection can inspect that runtime.
  Late-load permission uses the same ownership decision; a pending resident cleanup cannot
  grant the duplicate-interposer exception merely because its old armed flag is still set.
- Controlled production-function reproductions: old callback could not return while the install
  mutex was held; fixed callback returned with zero hook installs, followed by exactly one worker
  install and a resident call with zero custom loads. The concurrent custom-load case reaches the
  override on both calls with no resident hooks or rollback. Focused native policy/publication/
  decision tests and changed-product syntax checks pass. Live commercial-game retest remains pending.
- Validation caveat: one unfiltered 0.1.7097 run failed the GTA-style FSR switching flow
  (2 uncovered presents, 2,226 output-owner violations out of 2,400 outputs), despite same-batch
  draw-success entries. The scenario configures no Streamline override and never arms interposer
  takeover; Streamline initialization/detection succeeded. After rebuilding the harness, its
  isolated run and three concurrent copies passed. Cause and baseline status remain unproven;
  no FSR routing change was made to hide it. The failed trace was preserved locally for follow-up.
- Final clean 0.1.7098 closing gate passed the full native suite, Python tool self-tests and all
  52 FG flow scenarios. Both hook architectures were rebuilt and verified; PE import closure and
  shipped-artifact privacy checks passed. The fresh setup package has version `0.1.7098` and
  39,413,456 bytes. Optional integration/sanitizer/fuzz runs and live-game validation were not run.

### 2026-10-11 - Fix Streamline interposer takeover race and split-brain duplicate in The Witcher 3

- Session `20261011_113848` (`witcher3.exe` p15696 and p5516, build 0.1.7093) broke Streamline detection:
  every `slGetFeatureFunction`, `slIsFeatureSupported` and `slSetD3DDevice` call returned `result=21`
  (`sl::Result::eErrorNotInitialized`).
- Root cause:
  1. `witcher3.exe`'s main thread (`WinMain`) reads its static `__imp_slInit` IAT slot without touching
     `LdrpLoaderLock`, so it can enter the game-directory `sl.interposer.dll!slInit` while CE's `DllMain`
     is still running. Inside `DllMain`, `InstallKernel32LoaderHooks("DllMain")` spent ~159 ms running
     10 `PatchIATAllModules` passes across ~90 modules, and `TryArmTakeover("DllMain")` then called
     `DllFileMajorVersion(overridePath)` -> `GetFileVersionInfoW` -> `LoadLibraryExW(LOAD_LIBRARY_AS_DATAFILE)`,
     stalling another ~220 ms through Steam's `gameoverlayrenderer64.dll` hook (plus ~21 ms from
     `3a7f92ef`'s `ScanLoadedStreamlineCore`), totaling ~380 ms before `slInit` was patched.
  2. When `resident!slInit` started during that 380 ms window, it spent ~250 ms inside `nvapi64.dll` and
     `WinVerifyTrust` before mapping `sl.common.dll`. `TryArmTakeover("DllMain")` ran in that pre-`sl.common.dll`
     window (seeing `CorePresence::Absent`) and armed takeover *after* `resident!slInit` had already been
     entered. `HookThread` then eagerly called `EnsureCustomInterposerReady()` 15 ms later (still before
     `sl.common.dll` mapped), loading a second `sl.interposer.dll` (`00007FFF2BCC0000`) and rebinding all
     18 `witcher3.exe` IAT slots and `StreamlineHook` to the uninitialized duplicate while `resident`
     (`00007FFF3D190000`) completed `slInit`.
- Fix:
  - `hook/runtime/dll_utils.h`: added `DllReadPeVersionResourceW` to parse the PE `.rsrc` `RT_VERSION` (`16`)
    resource directly via `CreateFileW` + `ReadFile` without calling `GetFileVersionInfoW` /
    `LoadLibraryExW(LOAD_LIBRARY_AS_DATAFILE)`.
  - `hook/runtime/main_injection.cpp` & `hook/streamline/streamline_interposer_takeover.cpp`: moved
    `ResolveEarlyRuntimeOverridePaths()` + `ArmEarlyFromDllMain()` to the very start of
    `InstallKernel32LoaderHooks("DllMain")` before the 10-pass kernel32 IAT scan, and reused the single
    `EnumProcessModules` vector for both `ScanLoadedStreamlineCore` and `CollectInterposerImportingModules`,
    eliminating ~380 ms of `DllMain` delay before `slInit` IAT gating.
  - `hook/streamline/streamline_takeover_core_scan.h`, `hook/streamline/streamline_interposer_takeover.cpp`,
    `hook/runtime/main_overlay_detect.cpp`, `hook/runtime/main_redirect.cpp`: gated `LoadAndPublish` behind
    entry-thunk activation (`CanActivateTakeoverForExport`: only `slInit` index `0` or pre-core proxy
    exports `>= 27` when `CorePresence::Absent`) so `HookThread` never eagerly maps a duplicate interposer,
    and added `AbandonArmedTakeover` rollback (restoring IAT slots to the resident interposer and invoking
    `StreamlineHook::OnModuleLoaded` on it) when a Streamline core maps or a post-`slInit` export (`1..26`)
    fires while takeover is still armed and inactive.
