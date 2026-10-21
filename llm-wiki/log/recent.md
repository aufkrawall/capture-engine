# llm-wiki Log

### 2026-10-21 - the media role is driven by a unit test; loader table in its own namespace

- `unit_tests.exe` links mediaengine's sources, so the loader's `MediaEngine_*` pointer globals had the exports' names
  and linking the media role into the tests aliased them (crash). The table now lives in `ce::mediaengine_api`.
- `MediaRoleThreadTest` runs `RunMediaRole` on a thread: a missing `mediaengine.dll` and a refused start both exit 1,
  close the handles the role was given (checked by the object's handle count, not a reusable handle value) and log
  to the role's sink only. A successful Init stays undriven (the unit-test build identity refuses a product DLL).

### 2026-10-20 - libcengine phase 2 completed in code (M13-M18); tests isolated from the developer's machine

- M16b handles, M17b media RoleContext, M18 default switch landed (`bc9b50db`, `a69cd01e`, `38e6c20b`); the app and
  every `cengine.dll` client run inject, media and logger in-process; `[Runtime] isolated_helpers=true` opts out.
- `--verify --verify-clean` at `cb542d23` passed everything except one `bugprone-exception-escape` in a test helper
  (fixed, lint re-run green): clean build, unit suite, all FG flow scenarios, ASan/UBSan, boundaries, installer.
- Interference found and removed: desktop-overlay tests read a live CE's discovery and recording state; RTSS's
  `RTSSHooks64.dll` injected into four flow scenarios (gate hook logs; RTSS not running later); the RTSS coexistence
  scenarios load a stand-in of that name from their own folder (the backstop now exempts it); registration tests
  wrote into the real Khronos loader key, the target-list test and the inject role into the live CaptureEngine key,
  the autostart test into the real Run key. All in the test namespace now (DR-48).
- Real defects found by the new tests: a stopped inject role never prewarmed again in the same process
  (`StopPublicationWarmup` left its flag set); the in-process media role kept the client's raised GPU scheduling
  priority after recording (claims released by the last session now restore it).
### 2026-10-20 - libcengine phase 2: audit, contracts and the first implementation round

- M12 audit (six parallel read-only audits) in `library/10-phase2-audit.md`; DR-37..DR-46 in `library/08`.
  Decisive facts: the media role has 146 mutable process-state items (65 `media_main_g_*`), applies
  `SetPriorityClass` (default `process_priority=high`), process GPU scheduling priority and a console handler
  that swallows every event; the per-app audio worker stays a process (AudioSes `CLoopbackMixer` teardown crash,
  `5fdc11df`/`4e1f30ed`/`9ff8b41d`) and was launched as `GetModuleFileNameW(nullptr)`, i.e. the client.
- Landed (`2bd776fe`..`d657dacb`): role channel seam and in-memory channel, `RoleContext`, thread-bound log
  sinks, API 2.1 feature bits and `[Runtime] isolated_helpers`, thread transport in the supervisor, logger and
  inject roles from a RoleContext, media globals in `MediaSessionState`, mediaengine state in one instance.
  Real role entries are wired only in M18; until then every role still runs as a helper process.
- Closing gate at `d657dacb` + fixes: unit suite passed; 4 of 150 FG flow scenarios failed under load while the
  user's CaptureEngine ran (`FlowTranslation*` saw no device queries on presents; GTA switch took 66 s instead of
  21 s) and all 4 passed on an isolated re-run; no hook code changed. No installer from that gate.
- Review fixes: a test without a bound media session aborted the suite (`MediaState()` terminates by design);
  the logger had duplicated the lifetime wait (now `WaitForServiceLifetime` with a stop event); a static
  mediaengine instance would have released D3D11/COM objects at DLL detach (now leaked by design).
### 2026-10-20 - FFmpeg pin check: coverage and evidence for c08a7f45

- A sanitizer tests-only build in a fresh worktree ran `git fetch --unshallow` in main's shared
  `ffmpeg_build/repos/ffmpeg` although HEAD was the pin `86940d45`: `<sha>^{commit}` reached msys git as
  `<sha>^commit` (only quoted argv is spared, `subprocess` quotes only whitespace). c08a7f45 had already switched
  to `<ref>^0`; added tag / off-pin / no-fetch coverage and the argv rule (`build.py.md`, FFmpeg source pin check).
- With the fix, a fresh worktree gate without `installed/captureengine/ffmpeg` logged `ffmpeg already at pinned
  86940d45...` and went straight to the DLL sync, so the empty-directory workaround is no longer needed.

### 2026-10-20 - ASan UAF in OverlayFrameCostTest was a DX11 backend downcast at teardown

- `--verify --verify-clean` (libcengine branch, same overlay code as main) failed deterministically in
  `OverlayFrameCostTest.TheGraphAloneMovingRedrawsOnlyTheGraph`: heap-use-after-free, WRITE of size 1 at offset 1
  of a freed 32-byte block. cdb on the sanitizer exe (see `debug-tools.md`) resolved it: the write is
  `OverlayAdapter::ApplyShutdownModeLocked` <- `DestroyResourcesLocked` <- `Shutdown` <- `~OverlayHarness`, not the
  graph-only redraw; the block was a `FontAtlas::BuildGlyphSpans` vector buffer freed at `Renderer::Initialize`.
- Cause: `ApplyShutdownModeLocked` did `static_cast<DX11Backend*>(backend)->SetSkipRelease()` whenever
  `backendType == DX11`, and the harness binds a counting backend via `InitCustom(..., DX11)`; the bool landed past
  that small object. Same class as the 2026-10-09 DX12 fix. Shipped paths bind DX11 only with `DX11Backend`.
- Fix: virtual `RendererBackend::SetSkipRelease`; regression `OverlayAdapterBackendIdentityTest.CustomDX11*`
  (sentinel storage overwritten without the fix, in any build).
- Hidden behind it (ASan aborts the process at the first report): an access violation in
  `D3D11DispatchForwarders.X64ForwardersFollowTheRuntimeField` at `cmp dword ptr [r12-8], 0C105CAFEh`, i.e. UBSan
  `-fsanitize=function` reading the signature before a forwarder at the start of a fresh `VirtualAlloc` page. The
  test now calls it through a `no_sanitize("function")` helper. With it `--verify`'s sanitizer pass is green.
- Lint: two 2026-10-19 test regressions fixed here (`bugprone-integer-division` 885c746c, `bugprone-argument-comment`
  218d8c90). Still open on main: `bugprone-throwing-static-initialization` 5 > 4, `performance-faster-string-find`
  (new, `test_streamline_output_site.cpp`), `performance-inefficient-vector-operation` 3 > 1, so `--verify` fails
  at lint only.

### 2026-10-20 - libcengine: cengine.dll, C ABI complete, frontend on the API (rounds 4-6)

- Integrated on `claude/libcengine-refactor-orchestrate-e6c27c` up to `cb9e24ac`: frontend on `cengine.hpp`
  (H8), settings store + `ce_settings_*` (H9), setup API + monitors + tray on `ce_setup_*` (H10), shutdown keeps
  finalizing media (Q8), `settings_path` (DR-28), destroy ordering (DR-29), diagnostics session and session
  claim in `ce_runtime_create` (DR-30), `cengine.dll` with checked exports, module pin, delay-loaded by the
  exe (DR-32), `sdk/` in the installer, headless sample built against the SDK only. Status: `library/README.md`.
- Pitfalls hit: the exe link line passed the Windows command-line limit in a worktree path (response file
  now); Python heredoc edits turned `\n`/`\0` in C++ literals into real newlines and a NUL byte (edit C++ string
  literals with exact-replacement tools, check `git diff` for binary changes); two subagent fixes for destroy
  ordering contradicted each other until both signals were made safe.
- Not run: the outside-repo SDK run (starts the real runtime and helpers; user-driven), hardware smoke of the
  app on `cengine.dll`, Steam/RTSS coexistence. Lint showed one size regression (`video_encoder.h`, fixed).

### 2026-10-20 - libcengine: engine thread, components, v1 removed (orchestrated parallel rounds)

- Rounds 1-3 ran as parallel subagent streams in isolated worktrees, integrated by cherry-pick on
  `claude/libcengine-refactor-orchestrate-e6c27c` (`c08a7f45`..`f0b2b37a`). Decisions DR-20..27 and answered
  Q2/Q3/Q5/Q6/Q8/Q11/Q13 are in `library/08`; status in `library/README.md`.
- Build fixes found on the way: the FFmpeg pin check never matched under MSYS git (`^{commit}` is globbed;
  every build re-fetched and force-checked-out the shared clone, racy across worktrees; `c08a7f45`); the
  runtime/api C ABI needs `CE_STATIC_LIBRARY` while linked into the exe (`0128289c`).
- Test fixes: an event-queue concurrency test could exit holding the drop notice (`842cb099`, 0/40 failures
  after); WARP FG flow scenarios time out under heavy parallel compile load (all 4 passed alone in 74 s) -
  run the gate's flow stage on an idle machine.
- Integration lesson: an automated conflict resolver must stop the cherry-pick loop when it cannot resolve;
  one commit briefly kept conflict markers and was redone from a backup branch.
