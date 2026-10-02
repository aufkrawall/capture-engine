# Refactor roadmap

Last cross-checked: 2026-10-02

Goal (user, 2026-10-02): an orderly, readable codebase that an LLM can investigate with few tokens, with
nothing broken or regressed, fewer lines where that costs nothing, and boundaries that let CaptureEngine
become a library other clients use for recording, overlay and 3D overrides.

## Rules for every wave

- A wave is either **mechanical** (moves, renames, re-spelling, regrouping) or **behavioral**, never both in
  one commit. Mechanical waves need a proof that code did not change; behavioral waves need tests.
- Mechanical proof: `tools/refactor/preprocess_fingerprint.py snapshot` before and after, then `compare`
  with the rename mapping. Every product TU must preprocess byte-identically, except TUs whose text was
  meant to change. Objects are LTO bitcode, so object comparison is not used.
- Source-policy tests (about 1,450 of 4,315 tests) assert on source text. A mechanical wave applies the same
  rename to the tests. A behavioral wave replaces the source assertion with a behavior test where the code
  becomes testable, never deletes the protection silently.
- Present, FG-transition and overlay-route code (`hook/d3d12`, `hook/present`, `hook/streamline`,
  `hook/ffx`) gets only mechanical changes until a behavioral step can be validated on hardware by the
  user (GTA/Talos/W3 FG switching matrix). Unit tests alone do not cover those paths.
- One closing gate per wave: `python build.py --incremental --run-tests --skip-updates --concise`.

## Baseline (2026-10-02, before wave 1)

| Measure | Value |
| --- | --- |
| First-party C++ lines | hook 202k, tests 90k, captureengine 48k, mediaengine 36k, common 29k, testapp 25k |
| Python lines | tools 41k (+ build.py) |
| llm-wiki | 35.9k lines, 178 files; `index.md` paragraphs are ~1 KB each |
| Flat directories | `hook/apis` 252 files, `hook/common` 248 files |
| Hook globals | ~1,300 distinct `g_` identifiers; 223 `dx12_hook_[gs]_*` in `dx12_hook_internal_globals.cpp` |
| Log call sites | `HookLogImportant` 2,301, `HookLog` 1,148, `EarlyLog` 304, `Log{Info,Warn,Error}` 1,220 |
| Unit tests | 4,315, of which ~1,450 read source text (3,239 `find(` assertions in 108 files) |
| Session logs (CE's own) | 0.9-2.2 MB per session (~225k-550k tokens); `hook_debug.log` 50-89% of it |

## Waves

| # | Wave | Kind | Status |
| --- | --- | --- | --- |
| 1 | Subsystem directory layout (repo-map.md), root-relative includes, layout helpers in `build_common.py` | mechanical | done 2026-10-02 |
| 2 | Runtime log volume: the top families below, an ON-CHANGE gate in `log_meter.h`, shorter prefixes | behavioral (logging only) | 2a done 2026-10-02 (0.1.6946): ~25 families + hook/Vulkan prefix; remaining: service `Log()` date prefix, DisplayTiming line, PRESENT STAGE COST legend, `sl.log` verbosity |
| 3 | `tools/log_digest.py`: a per-session digest (files, warnings/errors, transitions, top templates, gaps) so an investigation starts from ~20 KB instead of 2-16 MB | new tool | done 2026-10-02 |
| 4 | Agent docs: compress `index.md` routing paragraphs and AGENTS.md without dropping any rule | docs | planned |
| 5 | Dead code: linker `--gc-sections` report + unreferenced units (e.g. excluded `hook/wrappers/d3d12_*_wrap.cpp`) | behavioral (removal) | planned |
| 6 | State grouping: loose `dx12_hook_g_*` / `streamline_hook_g_*` globals into named state structs per concern | mechanical | planned |
| 7 | Rename size-split units to what they do (`dx12_hook_process_session_phase1..5`, `media_main_encoder_0N_*`, `mediaengine_audio_pull_encode_{a,b,c}`) | mechanical | planned |
| 8 | Policy calls with many positional `bool`s to named input structs | mechanical | planned |
| 9 | Comment density: incident narratives (session ids, dates) out of code into the wiki; code keeps the invariant | text | planned |
| 10 | Library boundary (below) | architectural | planned |
| 11 | DX12 frame/overlay state machine as explicit states | behavioral, hardware-validated per step | later |

## Library boundary (wave 10)

- CE is several processes (controller, inject child, media, logger, sensor bridge) plus injected DLLs. A
  library keeps that topology: a client loads one engine DLL that owns the helper processes.
- The public API must be a C ABI with opaque handles (`ce_engine_create`, recording start/stop, overlay and
  override settings, screenshot, event callback). CE builds with MSYS2 clang64 and libc++; a client built
  with MSVC cannot share C++ types across the boundary.
- Configuration needs a programmatic model. Today `common/config/` parses INI into `Config`; the INI loader
  becomes one producer of that model, and the shared-memory publication (`common/ipc/`) stays internal.
- `captureengine/app/` (tray, hotkeys, pseudo overlay) becomes the first client of that API, so the
  boundary is exercised by the shipping product.

## Log findings (evidence for wave 2)

Measured on sessions `20261002_063748` (Gothic2 DDraw + Strange Brigade DX12), `_062900`/`_060100`
(GTA5 Enhanced FG), `_055724` (Talos UE5), `_051703`/`_055313` (Witcher 3 SL2 bridge, DLSS-G),
`_051557` (dx12_fg_switch_test). CE's own logs: 11.2 MB ≈ 2.8 M tokens over 7 sessions.

- Prefix cost: `hook_debug.log` 30% (54.6 B/line; the process name alone is 10.8%), `inject.log` and
  `captureengine.log` 27-29% (mostly the full date). `[INFO]` marks nothing (Trace is printed as INFO).
- 37% of all CE log bytes repeat the previous line of the same template once counters are stripped.
- Largest families: inline-hook install narrative ~20 lines per hook incl. one-byte-per-line dumps (1.0 MB);
  inject config prewarm re-logging 17 app-audio lines x 28 targets per sweep on every reload (77% of
  `inject.log`); `[ControllerDiag]` 3 lines per cycle (62% of `captureengine.log`); `Post-SL overlay SUBMIT`
  logged every frame for renderNum 1700-1900 (201 of ~235 lines); IAT "Successfully patched" + "Patched" pairs.
- Defects: Steam overlay "Third-party overlay detected" on every hook pass (826x in one session); NVNGX
  version/preset logged before the `nvngx.dll` presence check and repeated per retry; `LogOncePerParam` keyed
  by parameter only, so three message variants alternate; pseudo-overlay "NOT RECORDING" blink logged as a
  state change every 1-2 s; FPS-limiter stats written to two files and `fps_limiter_trace.log` silently
  capped at 200 lines; Vulkan layer writes each message to `vulkan_layer.log` and `vulkan-layer.log`.
- Vendor logs: the SL2 bridge sets `sl.log` verbose whenever `log_level>=trace` (the default): 7.7-7.9 MB per
  W3 session, and it contains the unredacted account name. NGX logs follow `[DLSS] ngx_log`.
- Loss: the shared log ring overflowed 507-1,399 times per session; 0-15 lines per session were dropped.
  Less volume means fewer drops. The old `[S:N]` counted per process across all files (gaps ambiguous);
  since 0.1.6946 the `#N` column counts per file.
- Text consumers that must keep working: `testapp/run_tests_support.py` (`IAT: Patched D3D11CreateDevice`,
  `DX12: ProcessFrame queue=`, `DX12: Overlay frame #`, media-log date prefix), `tools/analysis` media-log
  parsers (full-date prefix), `tools/analyze_sl1_probe.py`, `tests/test_present_stage_cost.cpp`.
- Existing helpers: `ShouldLogCadence` (common/logging/log_meter.h, 33 uses vs ~494 hand-written
  `% N == 0` meters), `SlowIterationLogGate` (captureengine/media/encoder_loop_stage_cost.h, carries the
  suppressed count), `DecisionLogGate` (hook/sharpen/sharpen_pass_log.h). Missing: a generic
  log-on-change gate that reports how many unchanged repeats it suppressed.
- Estimated effect of the family fixes plus a shorter prefix: about -63% (2.8 M -> ~1.0 M tokens).

## Open questions

- Per-process hook log files would remove the process-name column but break `testapp/run_tests_support.py`
  and many wiki citations of `hook_debug.log`; the plan keeps one file and logs a PID legend instead.
- Whether `sl.log` verbose should become an explicit option (privacy and size) - ask before changing.
