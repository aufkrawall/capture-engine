# 06 - Milestones

Each milestone is a series of small, self-contained local commits (CLAUDE.md). For each commit:
iterate with the tests-only loop; before committing, update CHANGELOG under `## Unreleased`, run
`python tools/manage_changelog.py --validate`, `git diff --check`, the closing gate (fresh installer,
never `--skip-package`), secret review and scan, commit, then scan the exact commit. Extra gates are
listed per milestone.

Gate shorthand:
- **dev**: `python build.py --incremental --tests-only --run-tests --gtest-filter=<suite> --skip-updates --concise`
- **close**: `python build.py --incremental --run-tests --skip-updates --concise`
- **clean**: `python build.py --skip-updates --concise`
- **verify-clean**: `--verify --verify-clean` (build machinery, shared ABI, gate policy)
- **lint**: `python build.py --no-build --lint --skip-updates --concise` (after a full product build)
- **fuzz**: `python build.py --run-fuzz` for parser/untrusted-input changes

Depth metrics (07) are recorded in the status table at the end of each milestone.

---

## M0 - Adopt the plan

Commits:
1. `docs: adopt library-first plan` - move the committed `llm-wiki/libcengine-refactor/*.md` to `llm-wiki/library/`, add index
   rows, mark D8/D9/D13 of `architecture-debt-plan.md` superseded (link), move its "Execution status"
   prose into `llm-wiki/log/` (recent entry routes to an archive), delete `temp/refactor.md` and the
   source plan folder.

Acceptance: one canonical copy; index routes to it; no contradictions with `library-delivery.md`
(merge that page's capability inventory into `library/04` or link it).
Gates: changelog validate (docs-only commits still follow the commit/secret rules); no product build
needed unless a gate policy requires it.

## M1 - Module boundary checker

Commits:
1. `tools: add module boundary checker` - `tools/check_module_boundaries.py` plus
   `tools/module_boundaries.json` (rules from 02, an initially generous exception list matching
   today's tree). Python self-tests in `tools/tests/` (fixtures: allowed edge, forbidden edge,
   same-dir include, exception ratchet: a new exception fails, a removed one folds in).
2. `build: run boundary checker in lint and python self-tests` - wire it into `--lint` and the
   unfiltered close gate's Python self-tests. Report includer counts of `*_internal.h` in the summary.

Acceptance: the checker passes on the current tree with documented exceptions; adding
`#include "captureengine/app/main_internal.h"` to a hook file fails; the exception count is recorded.
Gates: dev (python self-tests), close, lint. Machinery change → verify-clean (CLAUDE.md: gate
policy).

## M2 - Public header v2 and ABI tests (not shipped)

Commits:
1. `api: draft cengine v2 header` - `include/cengine/cengine_draft.h` (full draft from 03) and
   `include/cengine/cengine.hpp` written against it (header-only, compiles).
2. `tests: add C ABI layout and C compilation tests` - a C11 translation unit compiled as **C**
   (proves C compatibility), static layout assertions (offset/size of every struct on x64), and a
   C++ wrapper compile test. No exports, no implementation.

Acceptance: header compiles as C and C++; layout assertions documented as the ABI baseline;
reviewers agree on the semantics in 03 (open questions in 08 resolved or explicitly deferred).
Gates: dev, close.

## M3 - Runtime core inside the exe

Goal: `ControllerMain` loses its engine responsibilities to `runtime/core/Runtime` (C++), still
inside `captureengine.exe`. No public API yet. Behavior identical.

Commits (follow 05 "Migration order" rows 1-6):
0. Detach the existing controller include dependencies identified by Q13 before moving those units
   (DR-19), in separate behavioral commits. Keep the boundary checker enforced throughout.
1. `refactor: move controller sources to runtime module` - mechanical: create `runtime/` and move
   `recording_session.*`, `controller_recording.*`, `host_children.*`, `child_process_lifecycle.h`,
   `runtime_configuration.*`, `configuration_state.h`, `main_vulkan_residency.h`,
   `hotkey_input_hook.*` and `captureengine/pseudo_overlay/**` into their `runtime/<subsystem>/`.
   `captureengine/app/status_overlay_sync.*` is the **media-side** half of the status-dark protocol
   (used by `media_main_start.cpp`), so it stays in the role host; move it to `captureengine/media/`. Update build source lists (`build_common.module_sources`), test
   source lookups (`ce::test_source::FindSource`), and repo-map. Prove the move with
   `tools/refactor/preprocess_fingerprint.py` (byte-identical preprocessed TUs except include paths).
2. `refactor: own controller composition in Runtime` - `Runtime` constructs settings, children,
   recording, heartbeat and residency. `ControllerMain` calls `runtime.ServiceOnce()`. Globals
   `main_g_ConfigPath`, `g_Session` and the free-function child accessors are replaced by members.
3. `refactor: runtime owns startup sequence` (05 row 2).
4. `refactor: runtime owns global hotkeys` (row 3).
5. `refactor: runtime owns desktop overlay` (row 4).
6. `refactor: runtime owns screenshot orchestration` (row 5).
7. `refactor: run engine loop on a runtime thread` (row 6). Commands from tray/hotkeys go through
   `Runtime::Submit`; the tray reacts to recording events.
8. `refactor: remove controller globals` - delete `main_internal.h` globals (table in 04) and shrink
   `main_internal.h` to role-host needs (or delete it).

Acceptance: `ControllerMain` (or its successor) contains no engine policy; `runtime/**` has no
writable globals except documented process singletons; existing recording/children/config tests pass
unchanged in substance; new `test_engine_runtime.cpp` cases from 04 pass; FG flow scenarios
unchanged; manual app smoke test by the user (record, stop, screenshot, overlay toggle, hotkeys,
config reload, quit) listed as pending hardware validation.
Gates: dev per commit, close per commit, **clean** after commit 1 (moved sources, stale objects),
lint at milestone end. Pending user validation: app smoke run.

## M4 - C ABI implemented; frontend on the API; v1 removed

Commits:
1. `api: implement cengine v2 runtime lifecycle, commands, events, status` - `runtime/api/` over
   `Runtime`; rename the draft to `cengine.h` with only the implemented sections (lifecycle, status,
   commands, events). Still statically linked into the exe.
2. `frontend: drive the runtime through cengine.hpp` - `frontend/` with `FrontendMain`, tray and CLI;
   uses only `cengine.hpp` (plus the transitional setup allowance, 05).
3. `api: remove v1 controller facade` - delete `include/libcaptureengine.h`,
   `libcaptureengine.cpp`, `libcaptureengine_controller.*`, `libcaptureengine_internal.h`, the
   `ControllerBackend` binding, and their tests (replace them with v2 API tests).
4. `tools: enforce frontend public-API boundary` - checker rule active; the exceptions list contains
   only the setup allowance.

Acceptance: the frontend source includes only `include/cengine/**` (+ allowance); API tests from 07
pass (lifecycle, exactly-once completion, stale handle, busy, event overflow); every v1 feature is
reachable via v2; tray/hotkey/CLI behavior unchanged.
Gates: dev, close per commit, clean at the end, lint. Pending user validation: app smoke run plus
`--launch` and `--auto-record`.

## M5 - Settings API

Commits:
1. `config: load from document with diagnostics sink` - `LoadConfigFromDocument`, sink, path overload
   wraps it; log output identical (test: same warnings for a fixture INI).
2. `config: report unknown keys by lookup tracking` - reader tracks reads; diagnostics for unread keys;
   tests including profile sections and aliases (`desktopoverlay.*` vs `pseudo-overlay.*`).
3. `config: move desktop overlay profile overrides into the loader` - removes the second parser in
   `ParseProfileDesktopOverlayOverrides`; behavior identical (fixture-based test).
4. `config: edit INI documents in place` - `IniDocument` set/remove/section creation, preserving
   bytes; fuzz harness `fuzz_settings_edit` + seed corpus registered in `FUZZ_TARGET_CORPUS`.
5. `runtime: transactional settings commit` - `SettingsStore::Commit` (validate, conflict check,
   atomic replace, self-write suppression, publish, fan-out, ReloadConfig).
6. `api: settings get/begin/set/commit/diagnostics` - add to `cengine.h`; API tests.

Acceptance: every 04 settings test passes; a programmatic change of e.g. `[Video] codec` reaches the
media helper (verified by the helper's logged config after ReloadConfig in an integration test, or by
a test-only query) without a restart; comments preserved; fuzz runs clean.
Gates: dev, close, **fuzz** (parser), clean at end, lint.

## M6 - Complete events

Commits:
1. `media: report finalization result and output path` - exit codes and the manifest fields (04
   §recording). **(verify)** today's exit code semantics first; tests in the media role layer.
2. `runtime: emit RECORDING_FINALIZED from retired media exits` - supervisor `CollectRetiredMediaExits`,
   mapping to recording ids; covers immediate restart overlap.
3. `media: return screenshot paths` - `TakeScreenshot` returns paths; SCREENSHOT event carries them.
4. `runtime: emit HELPER events` - ready/lost/recovered from supervisor service.
5. `media: skip status-dark wait without a status consumer` - spawn flag/handshake field (04 §desktop).

Acceptance: headless test (fake or real media child, see 07) observes STARTING→LIVE→STOPPING→IDLE
and exactly one FINALIZED with an existing file path; no 300 ms dark wait without the desktop
overlay feature (asserted via the handshake/log field).
Gates: dev, close, clean at end. IPC/handshake change → **verify-clean** (shared ABI) and **fuzz**
if the IPC deserializer changes (`fuzz_ipc_deserialize`). Pending user validation: real recording
with each capture method, A/V unaffected (this touches the media stop path).

## M7 - `cengine.dll` and the WinMain split

Precondition: the transitional allowance is gone (M9 setup API pulled forward if needed) and no
`frontend/**` file includes `common/**`.

Commits:
1. `build: build cengine.dll with explicit exports` - new target: `runtime/**` + required `common/**`
   → `cengine.dll`, `cengine.def` (actually linked; verify the export table equals the .def; memory
   note: a `.def` once was never linked), import libs for clang/mingw (`libcengine.dll.a`) and MSVC
   (`cengine.lib` via `llvm-dlltool`/`--out-implib`), PDB kept (debug symbols rule), PE hardening as
   other binaries. Packaging and installer include the DLL.
2. `runtime: pin module and exit via FreeLibraryAndExitThread` - plus a test that loads the DLL,
   creates, shuts down, destroys, and `FreeLibrary`s with no crash or leaked thread.
3. `app: split WinMain into role dispatch and FrontendMain` - the role prelude only for roles;
   controller mode → `FrontendMain` linking `cengine.dll` (import).
4. `mediaengine: verify build identity at load; drop unused legacy exports` - after confirming no
   internal caller (package-private rule).

Acceptance: `captureengine.exe` in controller mode loads `cengine.dll`; the exe's own `common` statics
are never initialized in controller mode (test: frontend objects reference no `Log_*` symbol, a
link-map/`nm` check in the build); x64 app works end to end; the installer contains the DLL, def,
import libs and headers in an `sdk/` folder; package verification checks them.
Gates: **clean**, **verify-clean** (new build target, hardening, shared ABI), close, lint.
Pending user validation: installer + app smoke on hardware; Steam/RTSS coexistence unaffected
(injection topology unchanged, but confirm).

## M8 - SDK package, headless client, outside-repo proof

Commits:
1. `samples: add headless C client` - `samples/headless/main.c`, built in a **separate** step that
   only sees the staged SDK folder (include + import lib), never the repo include path.
2. `tests: run headless client from an outside-repo deployment` - the integration test copies the
   staged package to a temp dir with a different executable name and CWD, runs the client with a
   bounded timeout (test window as WGC target or audio-only to avoid needing a game), verifies
   READY, a recording finalizes to an existing file, shutdown, then a second create in the same
   process (recreate). Busy test: run while another runtime holds the claim → `CE_E_BUSY`.
3. `docs: SDK readme` - shipped in the package (`sdk/README.md`): API contract summary from 03,
   threading, single-runtime limit, deployment.

Acceptance: the client compiles with only SDK artifacts; the outside-repo run passes; processes are
cleaned up after failure (fixture kills only processes it started, verifies none linger).
Gates: clean, verify-clean (packaging), close. `--verify-runtime` if the integration matrix
includes it.

## M9 - Setup API, monitors, client logging

Commits: `runtime: setup actions` (PawnIO install/uninstall with sensor stop, elevation service,
client autostart; split app autostart from elevation in `captureengine/elevation`); `api: setup,
monitors, log`; `frontend: tray setup items via API; remove transitional allowance`.
Acceptance: the tray uses only the API; exceptions list empty for `frontend/**`.
Gates: dev, close, clean, lint; elevation changes → user validation (requires admin/UAC).

## M10 (optional) - `cengine_host.exe`

Follow the hazard list in 05. Gates: clean, verify-clean, installer validation by the user.

## M11 (optional) - Recording statistics

Requires a media → runtime stats publication (reuse `MuxFlowSnapshotV1` and encoder counters over a
small read-only mapping or IPC poll). API: `ce_recording_get_stats`. Only ship it once each field
is real (frames, duplicates, drops, duration, bytes); otherwise leave the field out.

---

## Parallel tracks (not library prerequisites)

| Track | Scope | Rule |
| --- | --- | --- |
| Graphics | D1-D4, D7, D10 of the old plan | Driven by bugs and the FG/overlay requirements in CLAUDE.md; keep its good rules (no hot-path tax, provenance, retirement), seam tests allowed where no interface exists |
| Media internals | D5-D6 | Media process internals; only M6/M11 touch the library boundary |
| Build | D11 | Independent; the library only adds targets and the checker |
| Hygiene | D12 | Ongoing; delete v1 (M4), legacy package-private exports (M7), obsolete wiki text |

Don't interleave a graphics-track commit into a library milestone's commit series. Don't block a
library milestone on graphics-track work.
