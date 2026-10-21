# 11 - Helper isolation package (phase 3, M20-M26)

Status: **planned** 2026-10-21: scope decided by the user, four mechanism decisions open (see "Decisions"); not
started, no code changed. Base: `main` at `fa5770ff` (phase 2 of [09](09-in-process-engine.md) pushed, in no release
tag; latest tag `v0.1.7154`, so API 2.1 semantics can still change without breaking a shipped SDK client). Facts
below were read from code at `fa5770ff`; line numbers drift, names are the stable anchors.

## Goals

1. The app gets the pre-phase-2 process layout back by default: inject, media, logger and sensors as helper
   processes, one media process per recording. That restores crash isolation (HELPER LOST, the app survives), OS
   cleanup after every recording, killable helpers, priority boosts confined to the media process, and per-process
   attribution in Task Manager, dumps and `log_digest --pid`.
2. libcengine keeps both modes at full strength. Third-party clients stay in-process by default and never need
   `captureengine.exe`: every remaining helper (crash isolation, per-app audio fallback, sensors fallback, dump
   writer) can run from the client's own executable (part 6); per-app audio (part 5) and sensors (part 7) aim for no
   extra process at all.
3. The in-process path no longer gets the app's real-world coverage, so automated coverage and diagnostics replace
   it (part 2).
4. A crash costs as little as possible in both modes (part 3, MKV and MP4, exact track lengths), and an in-process
   client cannot be broken by its own FFmpeg or C++ runtime copies (part 4).

Unchanged: hook DLLs and the Vulkan layer (inside games), the elevation service (an installed Windows service; CPU
sensors without admin still go through it, because Windows only exposes those CPU registers to a driver).
Loading .NET into a third party's process stays rejected (DR-35); part 7 replaces it for the shown metrics instead.

## Order, commits, gates

| # | Milestone | Part | Commits (each self-contained) | Gate |
| --- | --- | --- | --- | --- |
| 1 | M20 | Core switch | config tri-state + transport table; app feature + template; docs/wiki/CHANGELOG | closing gate, SDK run both modes |
| 2 | M21 | In-process coverage | thread labels; residue probe; SDK harness video + residue check; integration matrix in both modes | closing gate, SDK run both modes |
| 3 | M22 | Recording recovery | 1 s clusters; audio-only staging; journal; remux with exact trimming; triggers + event + tray; fragmented MP4 | closing gate, fuzz (new recovery input) |
| 4 | M23 | FFmpeg isolation | static third-party deps; `-ce` suffix; import-name verification; decoy test | clean gate (FFmpeg configuration changes) |
| 5 | M24 | Per-app audio | owner-thread teardown inside the worker; silent reproducer; verdict; thread transport on success | closing gate per step |
| 6 | M25 | Client-image helpers | role dispatcher into `cengine.dll`; `ce_role_host_main` + feature bit; dump writer, audio worker, sensors bridge through it | closing gate, `--verify` (ABI), SDK run in three modes |
| 7 | M26 | Native sensors | GPU providers (NVML/NVAPI, ADLX, IGCL); CPU via PawnIO modules; backend selection with LHM fallback | closing gate; user hardware |

Parts 1 and 2 go before the user's hardware retest: 1 restores the app's layout, 2 makes the retest logs answer leak and
crash-attribution questions. Then 3, 4 and 6 (independent of each other), 5 (its fallback uses 6), 7 last (largest,
hardware-dependent). One build at a time on the PC, small commits, CHANGELOG with each user-facing commit, secret
checks before every commit, all in this session.

---

## Part 1: core switch

### Design

`CE_FEATURE_ISOLATED_HELPERS` becomes the client's *default* instead of a force, and `[Runtime] isolated_helpers`
becomes `auto|on|off` (missing = `auto`). One bit, one tri-state, the runtime decides (DR-45; the frontend never
reads the setting, DR-31).

| Client feature bit | Setting | Transport | Create-line source |
| --- | --- | --- | --- |
| not set (SDK default) | `auto` / missing | threads | `default` |
| not set | `on` / `true` | processes | `setting` |
| not set | `off` / `false` | threads | `setting` |
| set (the app) | `auto` / missing | processes | `feature` |
| set | `on` / `true` | processes | `setting` |
| set | `off` / `false` | threads | `setting` |

Why not a separate "prefer isolated" bit: two bits with overlapping meaning for one decision; API 2.1 is unreleased,
so redefining the existing bit is free. Consequence to accept: a settings file can move a client that asked for
isolation back to threads. The settings file is the client's own (`settings_path`), and the user wants exactly this
switch to run the app in-process for testing.

### Steps

1. `common/config` (`config.h:455`, `config_load_core.cpp:203`): replace `bool isolatedHelpers` with
   `HelperIsolation { Auto, On, Off }`. Parse `auto|on|off` plus the bool spellings; an unknown value is `Auto` and
   reported through the existing config diagnostics. Tests in `tests/test_config_isolated_helpers.cpp`: absent,
   `auto`, `on`/`true`, `off`/`false`, garbage (Auto + diagnostic), unknown-key tracker, misspelled key.
2. `runtime/core/helper_transport.{h,cpp}`: `ChooseHelperTransports(features, HelperIsolation, entries)` implements
   the table; `runtime_create.cpp:73` passes the tri-state. Tests in `tests/test_helper_transport.cpp`: all six rows,
   null entries stay processes, process tuning never isolates, the create line names each transport and source.
3. `include/cengine/cengine.h:74`: the bit's comment says "helpers run as separate processes unless the settings say
   `isolated_helpers=off`". Value, mask (`runtime/api/runtime_desc.h`) and ABI test unchanged; API stays 2.1.
4. `frontend/app/frontend_main.cpp:234`: the app adds `CE_FEATURE_ISOLATED_HELPERS`. It keeps
   `CE_FEATURE_PROCESS_TUNING`, which matters only with `isolated_helpers=off`. The DR-33 comment is updated.
5. `captureengine/config.ini.template:1098`: `isolated_helpers=auto`, explained (app: helper processes; SDK clients:
   threads; `on`/`off` force either).
6. Docs: `README.md:422`, `docs/technical-overview.md:28-31` and the mermaid edge at `:46`, `sdk/README.md:78-84`,
   `samples/headless/main.c:9-13` header comment. Wiki: DR-49 in [08](08-decisions-and-open-questions.md)
   (supersedes DR-33's "the app uses the default"), [09](09-in-process-engine.md) target table, [README](README.md),
   any page stating "the app runs in-process" (`process-ipc.md`, `repo-map.md`, `code-structure.md` to check).
7. CHANGELOG: the unreleased "Changed" entry "CaptureEngine's helpers run inside the app by default" is rewritten,
   not supplemented (the app's behavior returns to helpers; SDK clients get threads; `isolated_helpers=auto|on|off`).
8. Installed configs: a `config.ini` created from a phase-2 template contains `isolated_helpers=false`, which now
   forces threads. No release wrote it, so no migration code; the implementing agent checks the user's installed
   `config.ini` and reports the line if present.

### Verification

- Unit tests above fail without the change (current code isolates only on the bit or `true`).
- `tools/run_sdk_outside_repo.py --helpers both`: the sample's default must log `isolated=0 (default)`, the
  `--isolated-helpers` run `isolated=1 (feature)`; both record.
- Your retest: the runtime log's `[Runtime] helper transports: inject=process media=process logger=process ...
  isolated=1 (feature)` and Task Manager showing the helper processes again.

---

## Part 2: in-process coverage and diagnostics

### 2a. Thread labels and crash attribution

- New `common/platform/thread_label.h`: `ScopedThreadLabel("media 0042 encoder")` sets `SetThreadDescription` and a
  `thread_local` fixed-size char buffer the crash path reads without allocating (the heap may be corrupt at a crash).
- Apply at every thread CE creates: role threads (`runtime/children/role_thread_runs.cpp:236` today names only these),
  and the thread entries in `captureengine/media` (6 files), `mediaengine` (13), `captureengine/injection` (3),
  `runtime` (6), `common` (3). Names carry role and recording id. Thread-pool and WinRT callback threads are not
  ours to name; they get the label only for the scope of our callback.
- `common/crash/crash_handler.cpp:573` (`TraceCrash`): `[process][tid][label]`, so `crash.log` names the role
  that crashed. Check whether CE's dump writer includes the thread-name stream (WinDbg `~` shows descriptions); add
  it if not.
- Tests: label set/restore and truncation; crash trace format through the existing `...ForTesting` seams.
- Helps both modes (isolated helpers have several threads too).

### 2b. Residue probe after each recording (in-process only)

- New deep module `runtime/platform/process_residue.{h,cpp}`: `Snapshot()` = handle count, thread count, GDI and
  USER objects, private bytes, process VRAM (DXGI `QueryVideoMemoryInfo` over the adapters; the runtime may not
  depend on `mediaengine/video/gpu_memory_breakdown.h`, module boundaries). Injected into `HostChildren` as an
  interface so tests use a fake.
- `runtime/children/host_children.cpp` (thread transport only): snapshot when a media role thread starts while none
  is live (`Spawn`, `:234`); when the last live media thread is released (`:283`), one line
  `[MediaTeardown] residue recordings=K handles=+N threads=+N gdi=+N user=+N private=+X MiB vram=+Y MiB`. Overlapping
  recordings (stop, immediate start) log once, at the idle transition. The process transport never probes (the
  process exit cleans).
- Runtime destroy: `[RuntimeTeardown] residue ...` relative to the snapshot at create (covers the inject and logger
  role threads, WMI, the injection manager, mediaengine's process-wide caches).
- Tests: fake probe; 0->1->2->1->0 logs once; process transport never probes; create/destroy logs once.

### 2c. SDK harness exercises video and checks residue

- `tools/run_sdk_outside_repo.py` passes `--video` (the sample already supports it; today the harness records audio
  only, 7 KB `.mka`). The sample gets `--recordings N` (several recordings per runtime) and `--overlap` (stop, then
  immediate start), the case that needs two media sessions in one process.
- The harness parses each round's `[MediaTeardown]`/`[RuntimeTeardown]` lines. First version reports only; after
  measuring, it fails when handle/thread/GDI/USER counts differ between rounds after the first (warm-up absorbs
  one-time driver and cache allocations) or private bytes/VRAM grow every round. No device-specific constants.
- Python self-test for the parser (runs in the closing gate's tool self-tests). The harness itself stays an
  agent-run validation (no test apps), run after each part.

### 2d. Integration matrix in both modes

- `tools/build/build_tests.py:117-160` runs `testapp/run_tests.py` for `smoke_dx9`, `smoke_dx11`, `smoke_modern`
  (and `full_matrix`): real D3D9/11/12/Vulkan test apps with inject capture and a recording. `run_tests.py` gets
  `--helpers threads|processes` (writes `[Runtime] isolated_helpers=off|on` into the config it stages), and the
  runner executes every target in both modes (`smoke_dx9_threads`, `smoke_dx9_processes`, ...), each with its own
  results JSON and verification artifact.
- Cost: the integration step takes twice as long. It belongs to `--verify-runtime`, which launches test apps, so it
  stays the user's to run; agents run everything up to `--verify`.

### 2e. Your hardware matrix

One round with `isolated_helpers=off` in the app keeps in-process mode exercised by real games and FG switching,
which no automated test covers.

---

## Part 3: recovering interrupted recordings

### Facts today

- mediaengine writes video to a staging file `capture_stage_<UTC ms>_p<writerPID>_s<seq>.<container>`
  (`common/capture/reserved_capture_output.cpp:253`, `video_encoder_start.cpp:375-383`) and renames it at finalize
  (`video_encoder_finalize.cpp:689-710`). The writer holds a reservation handle that blocks delete/rename while it
  lives (`reserved_capture_output.h:56-58`). A crash leaves the staging file behind.
- Audio-only (`capture_audio_*.mka`) is reserved under its final name at Init (`mediaengine_config.cpp:57-68`), so a
  crashed audio-only file looks finished.
- Default container `mkv`; mp4/mov write the moov atom only at the trailer, so they cannot be repaired.
- A crashed MKV has header and tracks, unknown segment size, no duration and no cues. Lost at the tail: the open
  cluster (FFmpeg default limits 5 s / 5 MiB, `matroskaenc.c:2753-2756`), up to 32 KiB of AVIO buffer, CE's async
  packet queue and FFmpeg's interleave buffer, so tracks end at different times.
- A crash while live emits no FINALIZED event (`media_finalizations.h:40-54`, pinned by
  `ACrashWhileActiveIsNotAFinalization`); the controller shows RecordingFailed and respawns the media helper
  (`recording_session.cpp:149-150`). Nobody records or repairs the staging path. No on-disk record of an in-flight
  recording exists; with `log_level=none` there are no manifests at all.
- `cengine.dll` already delay-loads avformat/avcodec/avutil for AVIF screenshots and preloads them from
  `<package>\ffmpeg` (`screenshot.cpp:588-596`), so a stream-copy remux can run in the runtime without a new
  mediaengine export.

### Design

1. **Smaller crash loss:** recordings set the Matroska `cluster_time_limit` to 1 s (next to `reserve_index_space`,
   `video_encoder_encode.cpp:333-340`), so a crash loses at most about 1 s plus queued packets instead of up to 5 s.
   Overhead is a few bytes per cluster; seeking granularity improves. Going lower would need per-packet flushes
   (many small writes, bad on network shares) for little gain.
2. **Audio-only uses the staging/publish scheme** like video, so an unfinished `.mka` is recognizable.
3. **In-flight journal** (new deep module, owner of "which recordings are unfinished"): at staging reservation,
   write `<logs root>\inflight\<hash>.path` holding the staging file's absolute path (one UTF-8 line, nothing else to
   parse); delete it at publish or discard. Independent of `log_level` and of later `output_dir`/`container` changes.
   If the folder is not writable, recovery falls back to scanning the current output directory, logged once.
4. **Recovery module** in the runtime: for each journal entry whose staging file can be opened exclusively (its
   writer is gone): empty or header-less file -> delete with the entry; otherwise stream-copy remux to
   `capture_<start>_recovered.<ext>` with index and duration, then delete the staging file and the entry. Runs on a
   worker thread, cancellable at runtime destroy (FFmpeg interrupt callback), never on the engine thread.
5. **Exact track lengths at the cut**, the same contract as a normal stop:
   - Video ends on its last complete frame. The common end E is the earliest track end; if video ends after an audio
     track, trailing video frames are dropped first and E becomes the video end.
   - AAC/Opus (fixed frame size, `PadAndSignalDiscard`): the last kept packet gets an end skip through
     `AV_PKT_DATA_SKIP_SAMPLES` (written as Matroska DiscardPadding), the computation finalize already uses
     (`mediaengine/audio/audio_encoder_flush.cpp:296-316,380-392`), shared rather than copied.
   - FLAC/ALAC (finalize sends a short last frame): frames are independent, so only the last frame is decoded and
     re-encoded at the exact length; lossless.
   - PCM: the last packet is cut to the sample.
   - Result: all tracks end at E to the sample, as in a normally stopped recording.
6. **MP4/MOV crash-safe (fragmented MP4):** with `container=mp4|mov`, recordings use
   `movflags=+frag_keyframe+empty_moov+default_base_moof` with fragments of about 1 s, so a crash leaves every
   complete fragment playable. A normal stop writes the trailer (fragment index). Recovery drops the incomplete
   fragment and writes a regular (non-fragmented) MP4, whose edit list carries the exact end trim. To verify: exact
   end trim in a normally stopped fragmented file (the moov is written up front, so its edit list cannot hold the
   final duration; if it cannot be exact, the template says so and MKV stays the exact-length container), and
   editor compatibility (Premiere Pro, DaVinci Resolve, Vegas, Shotcut, Windows media players, browsers). MKV stays
   the default.
7. **Triggers:** (a) isolated mode: the controller survives a media helper crash, so the "media process exited while
   recording live" path starts recovery once the helper is gone; (b) runtime create, both modes: entries left by a
   crashed previous runtime (in-process crash, or a controller crash in isolated mode).
8. **Reporting:** a FINALIZED event for the recovered file (output path, recovered marker; exact ABI form decided in
   implementation, API 2.1 is unreleased: a new status or a flags field). The app shows a tray balloon
   ("Recovered the interrupted recording: <file>"; the tray has none today, `tray.cpp:82`). Overlay notifications
   stay unchanged (a new kind would bump the shared-memory ABI).
9. **Logging:** one `[RecordingRecovery]` line per file (leaf name, size, streams, cut point, per-stream tail loss,
   outcome) and a summary at create.

### Tests

- Remux module: truncated MKV/MKA and fragmented MP4 fixtures generated in the test (write a file, keep a prefix
  without trailer) for each audio codec (AAC, Opus, FLAC, ALAC, PCM) and multi-track; checks: index and duration
  present, every track's decoded sample count ends exactly at E, every packet demuxes.
- Journal: write/remove, dead-writer detection by exclusive open, unwritable folder fallback.
- Controller with fakes: crash while live triggers recovery once and emits FINALIZED; create with stale entries;
  the active recording's own entry is never touched.
- Fuzz: `fuzz_recording_recovery` over truncated/corrupt MKV seeds (FFmpeg demuxer input from disk is untrusted),
  registered in `FUZZ_TARGET_CORPUS` with a committed seed corpus.

---

## Part 4: FFmpeg and its third-party DLLs under unique names

### Facts today

- FFmpeg is built from source with MSYS2 CLANG64, x64 only, shared, into `external/ffmpeg`
  (`tools/build/build_ffmpeg.py:574-671`); DLLs ship in `<package>\ffmpeg\`. No suffix today.
- `mediaengine.dll` imports the five FFmpeg DLLs statically; `cengine.dll` and `captureengine.exe` delay-load
  avformat/avcodec/avutil for screenshots. The in-process load path (`LoadLibraryFromScopedDirectory`) only changes
  the search path; `PreloadScopedRuntimeDlls` adopts an already loaded module of the same base name by design
  (`secure_dll_loading.cpp:143`). Windows binds an import to an already loaded module with the same base name, so a
  client's own `avcodec-63.dll` wins.
- The same risk applies to the third-party closure in `ffmpeg\`: `libopus-0`, `libaom`, `libSvtAv1Enc-4`, `libva`,
  `libva_win32`, `libvpl-2`, `libc++`, `libunwind`, `libwinpthread-1`, `libiconv-2`
  (`tools/ffmpeg_dependencies.json`). `libc++`, `libwinpthread` and `libopus` are plausible in a client.
- Configure flags are not part of the FFmpeg fingerprint (`build_ffmpeg.py:683-711`); `external/ffmpeg` is never
  wiped (stale outputs would survive).

### Design

1. `--build-suffix=-ce` (FFmpeg DLLs become `avcodec-ce-63.dll` etc.; the leading `-` keeps every `avcodec-*` glob
   working; inter-library imports, import libs and `.pc` names follow automatically).
2. The third-party closure links statically into the FFmpeg DLLs. Checked: static archives for all of them already
   exist in the dependency prefix (`ffmpeg_build/dependencies/prefix/lib`: `libopus.a`, `libaom.a`,
   `libSvtAv1Enc.a`, `libva.a`, `libva_win32.a`, `libvpl.a`, `libiconv.a`, `libc++.a`, `libunwind.a`; winpthread
   from MSYS2 CLANG64). CE's own binaries (`cengine.dll`, `mediaengine.dll`, `captureengine.exe`) already import only
   Windows DLLs plus FFmpeg, so after this `ffmpeg\` holds only the five suffixed FFmpeg DLLs. To check: the
   licenses (iconv is LGPL, so it stays inside the LGPL FFmpeg DLLs, which remain dynamically linked) and the
   license/notice copying (`copy_bundled_runtime_licenses`, `build_ffmpeg.py:11-164`). libva still loads its VA
   drivers at run time by design (system-provided, not shipped).
3. A build verification that fails if any DLL in the media closure (`mediaengine.dll`, `cengine.dll`, `ffmpeg\*`)
   imports a non-system DLL whose name is not CE-unique. Extends the name-agnostic `verify_pe_import_closure`
   (`tools/ffmpeg_dependencies.py:662-686`).
4. Build sites: `build_ffmpeg.py` (flag, static deps, version 12 -> 13, prefixes at :198-208),
   `build_linux_msys2.py:278-284,361-383`, `build_project.py:454-474,616-623,688-697` (and the stale comment at
   :476-480), `build_compile_db.py:446-455`, `build_toolchain.py:56`, `screenshot.cpp:591`. Wipe `external/ffmpeg`
   once (or clean it in the build). Linux/WSL FG SDK path (`build_fg_sdk.py`) stays unsuffixed; not part of the
   package.
5. Tests: Python suffix resolution; a native decoy test: load a decoy `avcodec-63.dll` first, then mediaengine, and
   assert it bound to the CE copy.
6. Gate: clean gate (FFmpeg configuration change), then the SDK run in both modes.

---

## Part 5: per-app audio without a CaptureEngine process

### What the history shows

- **Feb 2026 (`5fdc11df`):** `IAudioClient::Stop()` on *endpoint* (system audio) loopback crashed in
  `AudioSes!CLoopbackMixer::Cleanup` (AudioLimiterAPO double free on the mixer's own thread). Fix: skip Stop,
  Release only. Endpoint loopback has run in-process this way since, with same-thread release since `86fe278c`.
- **Jun 2026 (`c8145e1d`):** process loopback crashed in the same function when *duplicate/late* captures of one
  process were released (Release without Stop). Fix: abandon the interfaces (never Release), then `9ff8b41d`/`a7a80e2e`
  moved each capture into a disposable worker so abandoned graphs die with the process.
- Duplicate clients per process were removed in July (route dedup). Never tested since: one client per process with
  Stop -> Release on the single MTA thread that activated, initialized and captured. That is what OBS and Microsoft's
  ApplicationLoopback sample do in-process (external comparison from web summaries, to verify).
- Not recorded: Windows build, AudioSes version, exception code, frequency, a symbolized stack. The original dumps
  and logs are gone. So nothing is proven either way.
- Today's teardown (`mediaengine/audio/app_audio_capture*.cpp`): activation on a transient `std::async` MTA thread
  whose COM init ends after activation, capture on another thread, no Stop/Reset/Release anywhere
  (`AbandonClientInterfaces`), event handles closed while an abandoned client may still signal them, process exit
  reclaims the live graph.

### Candidate CE-side causes (speculative, ranked)

1. Release of a running client without Stop, racing the mixer's own cleanup after the target's session ended
   (fits "late" and "duplicate").
2. Concurrent teardown of several clients on one process (the June trigger; removed by dedup, never re-tested).
3. COM apartment/thread lifetime: activation apartment ended before use; release on a different or uninitialized
   thread.

### Steps

1. Record the user's Windows build and `AudioSes.dll` version (read-only).
2. Owner-thread teardown: one MTA thread per capture owns activation, Initialize, capture, Stop, final drain, Release
   (capture client, then audio client), activation handler release, CoUninitialize; event handles closed only after
   Release; the session monitor's apartment lifetime aligned. It still runs **inside the worker**, so a crash stays
   contained and leaves a dump (worker crash.log) instead of hurting a recording.
3. Reproducer (needs the user's OK: a runtime experiment, though no game and no audible sound): a silent render target
   (renders zeros through WASAPI) plus a bounded test program that cycles process-loopback capture N times with
   variants (plain stop, target exits mid-capture, target recreates its session, late join, two clients on one
   process, event vs polling) for the old sequences (no Stop; abandon) and the new one. Goal: reproduce the old crash
   with the old sequence and show the new one clean.
4. Your hardware rounds with app audio (late joins, target exit, many recordings) on the new teardown inside the
   worker: zero worker crashes and reclaimed resources.
5. Positive verdict and the user's go: process loopback runs on a thread for in-process runtimes (SDK clients need no
   extra process), the isolated mode keeps the worker. Negative verdict: DR-43 stays and the worker runs from the
   client's own executable (part 6), so SDK clients still need no `captureengine.exe`; the evidence goes into the
   wiki and `sdk/README.md` states the extra process plainly.
6. Either way: verdict and evidence in [multi-audio-capture.md](../multi-audio-capture.md) and a new DR.

---

## Part 6: client-image helpers (no `captureengine.exe` for third parties)

### Facts today

- The role host is fixed to `<package>\captureengine.exe` (`runtime/api/runtime_package.cpp:50-83`), passed to
  `SpawnChildProcess` (`common/ipc/process_ipc_client.cpp:324`).
- `captureengine.exe`'s `WinMain` (`captureengine/app/main_entry.cpp:23-200`) dispatches, in order: setup actions,
  the per-app audio worker, the sensors bridge, PawnIO setup, the dump helper, elevation fixtures, then the
  `--mode=` roles through the role prelude (config, logging, crash handler, then `InjectProcessMain` /
  `MediaProcessMain` / `LoggerProcessMain`).
- The inject, media and logger role code is already in `cengine.dll` (it runs there in-process); the worker entry is
  `MediaEngine_RunProcessLoopbackWorker` in `mediaengine.dll`. The sensors bridge (`sensor_bridge_host.cpp`,
  `clr_interop.cpp`) and the dump helper (`captureengine/diagnostics/dump_helper.cpp`) are only in the executable.
- Crash dumps are already written in-process (`common/crash/crash_dump_writer.cpp`); the external helper is preferred
  only when a foreign overlay is loaded, because dbghelp's module walk through overlay hooks took up to a minute
  (`crash_dump_writer.cpp:99-104`, session `20260817_052857`).

### Design (the CEF model)

1. One role dispatcher in `cengine.dll`: the whole `main_entry.cpp` dispatch list and the role prelude move behind
   `ce::role_host::Run(commandLine)`; `captureengine.exe`'s `WinMain` becomes a thin caller (behavior unchanged for
   the app).
2. Public API (C ABI, `include/cengine/cengine.h`, `cengine.def`): `int ce_role_host_main(void)` reads the process
   command line and returns `-1` when this process is not a helper invocation, otherwise runs the helper and returns
   its exit code. A client calls it first thing in `main`/`WinMain`. New feature bit `CE_FEATURE_SELF_ROLE_HOST`:
   helpers launch from the client's own executable instead of `captureengine.exe`.
3. Everything that spawns `captureengine.exe` takes the runtime's role host: role processes, the per-app audio worker
   (`mediaengine/audio/process_loopback_capture.cpp` launches `GetModuleFileNameW(nullptr)` today), the sensors
   bridge, the dump helper, elevated setup actions.
4. The sensors bridge and dump helper move into the dispatcher in `cengine.dll` (the CLR is still loaded only inside
   the bridge helper process). `tools/check_module_boundaries.py` rules updated accordingly.
5. Checks while implementing: package paths come from `cengine.dll`, never from the helper's exe
   (`RuntimePackagePaths::FromModule`, DR-38); injection self-exclusion is identity-based and must register the helper
   identities; the remaining name-based rules (`common/capture/screen_grab_privacy.cpp`, `media_main_window.cpp`,
   hook dormancy in `hook/**/main_dllmain.cpp`) get the client's image name or move to identity; helpers stay in the
   kill-on-close job.
6. Dumps without any helper: with neither `captureengine.exe` nor `CE_FEATURE_SELF_ROLE_HOST`, the runtime never
   spawns a dump helper and writes in-process. The overlay-hook hang applies to game processes; the test is that an
   SDK client without foreign overlays always dumps in-process.
7. Documentation of the contract in `sdk/README.md`: call `ce_role_host_main` before anything else; static
   initializers in the client exe must be harmless; the helper processes show the client's name in Task Manager.

### Implemented (M25, 2026-10-21, not run on hardware yet)

- Dispatcher `ce::role_host::TryRun` (`captureengine/app/role_host.cpp`); `main_entry.cpp` keeps only the
  elevation fixtures and the app. `TryRunForClient` adds the client rule (`role_host_command_line.h`: the first
  argument must be `--mode=inject|media|logger|sensors`, `--ce-setup`, `--process-loopback-worker`,
  `--sensor-bridge`, `--install-pawnio` or `--uninstall-pawnio`; the installer commands and `--dump-helper` are not
  served) and makes the helper per-monitor DPI aware as the executable's manifest does.
- `cengine.dll` links the dispatcher with every process host plus its closure (role configuration, host metrics,
  dump helper, logger/inject/media process hosts, per-app audio worker host, sensors service and bridge, display
  timing): `CENGINE_ROLE_HOST_SOURCES` in `tools/build/build_cengine.py`, found with an `llvm-nm` closure walk.
- Package paths: `common/platform/package_module.h`. Latent phase-2 bugs for clients fixed on the way: the
  first-run `config.ini` template is now embedded in `cengine.dll` too (`runtime/api/cengine.rc`, checked by the
  DLL load check with `FindResourceW`), and the Vulkan layer residency, the logger process's logs root and the
  inject publication fallback use the package folder.
- Helper logs carry `[RoleHost] helper executable=<image> package=<module>`; the runtime log `[RoleHost] helpers
  run from <image> (package|client executable)`.
- Harness: `tools/run_sdk_outside_repo.py --helpers all|both|default|isolated|self` (default `all`); `self`
  deploys without `captureengine.exe` and requires every `[RoleHost]` line to name the client.
- Limitation: the hook's external dump helper stays `<package>\captureengine.exe` (DR-51).

### Tests

- Dispatcher rule tests (`tests/test_role_host_command_line.cpp`), package resolution with the bit
  (`test_cengine_api_runtime.cpp`), window ownership of a self-hosting client (`test_runtime_process_identity.cpp`).
- Dispatcher table tests (every helper command reaches its entry; unknown commands fail instead of falling through,
  as the worker does today).
- `samples/headless`: calls `ce_role_host_main`, gets `--self-role-host`; `tools/run_sdk_outside_repo.py` runs three
  modes (in-process, isolated with `captureengine.exe`, isolated with the client image), then a fourth run with
  `captureengine.exe` deleted from the deployment to prove no CE executable is needed.
- ABI: `tests/cengine_abi_layout.h` and the export list. `--verify` because the public ABI changes.

---

## Part 7: native sensors

### Facts today

- Shown metrics (`captureengine/sensors/sensor_selection_policy.h:57-66`): CPU temperature, package power, core clock,
  max core clock; GPU temperature, package power, fan, core clock, memory clock, voltage. No motherboard values.
- All of them come from LibreHardwareMonitor in the `.NET` bridge process; CPU values need PawnIO and admin, which
  CE gets through the elevation service (`sensor_broker.h`, `ce::elevation::Client`). CE's own PawnIO code only
  installs and removes the driver (`pawnio_driver.h`).
- GPU load and VRAM already come from native code (`hook/metrics/system_metrics_gpu.cpp`,
  `captureengine/app/host_metrics.cpp`).

### Design

1. A native sensor backend behind the existing sensor service interface (`sensor_service`, `sensor_plugin`), running
   on a runtime thread, no process:
   - **NVIDIA:** NVML (ships with the driver) for temperature, power, clocks, fan; voltage through NVAPI where
     available, otherwise that one metric comes from the fallback.
   - **AMD:** ADLX for temperature, power, clocks, fan, voltage.
   - **Intel GPUs:** IGCL (Level Zero control library) for the same set.
   - **CPU clocks:** without admin from Windows performance counters; with PawnIO, effective clocks from APERF/MPERF.
   - **CPU temperature and power:** PawnIO modules (Intel MSRs: thermal status, TjMax, RAPL energy; AMD Zen: SMN
     temperature, RAPL energy). Elevated clients read them in-process; unelevated ones through the elevation service,
     which gets a native CPU-sensor command instead of hosting LibreHardwareMonitor for it.
2. `[HardwareSensors] backend=auto|native|lhm`: `auto` uses native for every metric it can read on this machine and
   LibreHardwareMonitor only for the rest (and only if installed/enabled). Per-metric source in the log at start.
3. Loader rules: vendor libraries load by full system path with explicit search flags (no client search order
   changes, DR-39); a missing library is "not available", never an error.
4. Licensing to check first: PawnIO modules (CE must bundle or obtain the signed module blobs) and the vendor SDK
   headers/redistribution terms (NVML/NVAPI, ADLX, IGCL).
5. Validation is hardware-bound: the user's machine covers one CPU/GPU vendor pair; other vendors stay on `auto` with the
   LibreHardwareMonitor fallback until someone runs them. A comparison log (native vs LHM values side by side for
   one session, behind a debug flag) makes that check cheap.

### Implemented (M26, 2026-10-21, not run on hardware yet)

- `captureengine/sensors/native_sensors.{h,cpp}`: `HardwareSensorSources` replaces `LibreHardwareMonitorPlugin` in
  the sensor service; it plans each metric (`PlanSources`, logged once as `[Sensors] sources (backend=...)`), opens
  the native providers and starts the bridge only for what is left, with the native metrics turned off in the
  bridge's selectors. Still inside the sensors role process; the bridge's CLR child is what goes away.
- NVML provider: temperature, board power (mW to W), core and memory clock; only when NVML sees one GPU and DXGI one
  non-software adapter, so multi-GPU machines keep the bridge's busiest-GPU choice. Library from System32 (or the
  NVSMI folder) with explicit search flags.
- CPU clock provider: PDH `Processor Information` per logical processor; P-core average from
  `GetSystemCpuSetInformation` efficiency classes, maximum over all cores.
- `[HardwareSensors] backend=auto|native|lhm` (config, template, loader, service restart on change).
- Not done (each needs hardware or a decision): AMD ADLX and Intel IGCL providers, NVIDIA fan RPM (NVML reports
  percent) and voltage (NVAPI), CPU temperature and power through PawnIO modules (module licensing is the user's
  call), the native-vs-LHM comparison log. Those metrics stay on the bridge under `auto`.

### Tests

- `tests/test_native_sensors.cpp`: source plan per backend, NVML against a fake driver table (scaling, refused
  metric, no driver, several GPUs), processor-instance parsing, hybrid clock reduction, the merge;
  `test_config_hardware_sensors.cpp`: the `backend` key and the service restart on a backend change.
- Provider tests with fake vendor APIs (value scaling, unavailable metrics, library missing, permission denied).
- Backend selection table (`auto|native|lhm` x availability) and the per-metric source log.
- Rate-limited `[Sensors]` lines for source changes and read failures (`ChangeGate`).

---

## Decisions

Decided 2026-10-21: client-image helpers, native sensors, in-process crash dumps, integration matrix in both modes,
fragmented MP4, exact trimming in recovery and static FFmpeg dependencies are in scope.

Open (recommendation in parentheses):
1. Part 1: the feature bit as the client's default with `auto|on|off` (yes) vs a force bit the settings cannot
   override.
2. Part 3: Matroska clusters of 1 s (yes).
3. Part 3: tray balloon for recovered recordings, no overlay ABI change (yes).
4. Part 5: the silent process-loopback reproducer on the user's PC (yes).

Decided later with evidence: whether fragmented MP4 can carry an exact end trim (part 3), and the part 5 verdict.

## Not verified while planning

- External OBS/Microsoft teardown details come from web summaries.
- Exact end trim in normally stopped fragmented MP4; DiscardPadding honored by non-FFmpeg players.
- Whether CE's dump writer includes thread names.
- PawnIO module and vendor SDK licensing; NVIDIA voltage availability without private NVAPI calls.
