<!--
SPDX-License-Identifier: MIT
Copyright (c) 2026 aufkrawall
-->

# Agent Instructions

## Critical workflow

- Windows-first project: prefer PowerShell 7.6, Windows-native paths, and installed project tools unless there is a clear reason not to!
- **Default development loop:** iterate with `python build.py --incremental --tests-only --run-tests --gtest-filter=<suite-or-test> --skip-updates --concise` (about 5-7 seconds). Tests-only builds reuse the current product identity because they emit no product binary. Stay in this loop while writing code: getting to a testable result fast matters more than any intermediate gate. Do not rebuild the product after every small edit, and do not use `python build.py --version`!
- **Then close with exactly ONE gate. The gates are nested, not cumulative.** `--verify` already performs a content-validated product build (strict clean rebuild with `--verify-clean`), the full native test suite, the Python tool self-tests, lint including the clang-tidy ratchet, and ASan/UBSan validation. When the change requires `--verify`, run only that: prefixing it with `--incremental`, `--no-build --run-tests`, or `--no-build --lint` repeats work already covered by `--verify` and wastes quota for no added signal.
    - Ordinary change -> `python build.py --incremental --skip-updates --concise`, then `python build.py --no-build --run-tests --skip-updates --concise` (about 50 s + 12 s).
    - Change in one of the categories listed in the complete-gate bullet below -> `python build.py --verify --skip-updates --concise`, and nothing else. Cold/miss paths still perform every expensive analysis/build; content-proven warm work is reused automatically.
- `python build.py --incremental --skip-updates --concise` reuses an object only when its source, compiler binary, flags, dependency file, and project-header content signatures remain valid; production outputs still relink for the new exact build identity, while validation-only unit-test/test-app links may be reused only when their compiler, linker, flags, object/library inputs, and output hashes remain valid. Packaging and the normal binary/PE verification stages still run (`--skip-package` skips only the automatic 7z archives, for dev iteration).
    - This is a **build** gate: it does not run unit tests, lint, or sanitizers. Any explicit action flag (including `--skip-updates`) suppresses build.py's default quality mode, so tests, lint, sanitizers, and fuzzing only run when you ask for them.
- Use the clean `python build.py --skip-updates --concise` build gate instead of the incremental one when investigating possible stale artifacts, when explicitly requested, or when the task touches dependency/toolchain/FFmpeg configuration or generated-build machinery without also requiring `--verify`. It is likewise build-only, and it is redundant whenever `--verify` is required, because `--verify` builds the same product (content-validated; `--verify-clean` for the strict clean rebuild). `--force-rebuild` remains the stronger recovery that physically removes `build/obj` first.
- The complete gate is `python build.py --verify --skip-updates --concise`: content-validated product build (reuse is keyed on source, compiler, flags, dependency-file, and project-header content signatures), the full native test suite, the nineteen Python tool self-tests, lint including the clang-tidy and file-size ratchets, and ASan/UBSan regression coverage in one run. `--verify --verify-clean` is the same gate with the strict clean product rebuild and is required for changes to `build.py`, toolchain/compiler/linker/hardening policy, shared ABI/layout, analyzer or test-gate policy; the plain `--verify` gate covers the capture/CFR/FG/audio paths and the ordinary complete-gate categories. Do not make it the routine per-change gate.
    - Verification starts with a read-only file-size/static-analysis preflight against the last compatible full compile database, so a known regression fails before the expensive build. The authoritative lint still runs after the new product database is generated.
    - clang-tidy is content-addressed per translation unit. Reuse requires identical analyzer/configuration, compile command, source, and every compiler-recorded dependency by content; changed or unreadable inputs run again. The final ratchet always aggregates the complete database, including cached unchanged units.
    - The sanitizer child uses isolated object, binary, unit-test, temp, compile-database, and SDK-staging roots. On a miss it runs concurrently with the product build under a split worker budget. A prior sanitizer success is reused only when its manifest proves every discovered first-party input, compiler/dependency input, and required output byte-identical; changed/missing/ambiguous state runs the stage again. Explicit `--force-rebuild` bypasses this stage-success reuse.
- If a final product build fails after it has started, fix the cause and use `python build.py --resume --skip-updates --concise`. Resume is accepted only for the immediately preceding failed top-level build, keeps that failed attempt's build identity, and validates cached objects normally. If resume refuses, the toolchain/dependency boundary changed, cache correctness is in doubt, or the previous run succeeded, use the applicable normal incremental or clean gate instead.
- For test-only runs without recompilation, use `python build.py --no-build --run-tests --skip-updates --concise`. No-build verification reuses the current build identity, validates the unit-test link-cache manifest and output hashes before execution, and must not invalidate version-dependent objects.
- clang-tidy findings are ratcheted against `tools/clang_tidy_baseline.json`, which records accepted counts per check plus the translation units those counts were measured over. Lint fails when any check exceeds its count or a previously unseen check appears; counts below baseline are folded in automatically so a fixed warning cannot silently return.
    - **Run lint explicitly, never routinely.** `--verify` already includes it, so a separate `python build.py --no-build --lint --skip-updates --concise` is worth it only when closing with the incremental gate instead of `--verify`, or when specifically investigating a lint regression. A cold cache still costs roughly the old whole-tree duration; a warm run revalidates fingerprints and analyzes only misses.
    - **Prefer linting after a full product build.** Lint covers whatever `compile_commands.json` currently holds, and a `--tests-only` build regenerates it with a reduced entry count (252 of 552 translation units as of 2026-08-06). Such a run logs `clang-tidy lint scope reduced`, still fails on increases, and never folds its artificially low counts in, so it cannot corrupt the baseline — but it also proves nothing about the unlinted product sources.
    - Regenerate the baseline only deliberately, with `python build.py --no-build --lint --update-lint-baseline --skip-updates --concise`, when an increase is genuinely justified — and only after a full product build, because that flag refuses to rewrite the baseline from a partial database. The raw warning list stays advisory; only a regression is fatal.
- Fuzzing never runs on its own and is not part of any gate above. Add `--run-fuzz [--fuzz-seconds N]` (default 60 s per target, about 131 seconds total) explicitly for changes to `common/config.cpp`, `common/process_ipc.cpp`, or any other parser or untrusted-input boundary. The stage fails closed on a missing libFuzzer runtime, an unregistered harness, an empty corpus, a crash, or a target that executes zero units. Register every new harness in `FUZZ_TARGET_CORPUS` in `build.py` and commit its seed corpus; see `llm-wiki/fuzzing.md`.
- **GitHub Actions stable releases:** when the user asks to create/replace a stable release via the `release-stable` action, consult `llm-wiki/build.py.md` ("Stable release operations"): the self-hosted runner is this PC and must be started manually (`%USERPROFILE%\Programme\build\actions-runner\run.cmd`) before dispatching, otherwise the run stays queued; the delete-then-trigger procedure and explicit `version=0.1.N` input are documented there.
- Prefer `--concise` for agent runs: `build.log` and the console retain stage summaries, warnings, and bounded failure tails, while complete commands/subprocess output remain in the run's `build.details.log`. Use `build/verification/latest_summary.txt` or `latest_manifest.json` for status and inspect detailed/per-stage artifacts only when diagnosing a failure. If you need more of a gate's output than you captured, read that run's `build.details.log` or stage artifact — never re-run the gate just to see its output again!
- Always git commit after code changes!
- Match the surrounding code's existing indentation, naming, comment density, and line endings; keep edits narrowly scoped and inspect `git diff --check` / `git diff` before building! Never run `clang-format.exe -i`, `python build.py --format`, or any whole-file automatic formatter on existing source files unless explicitly requested.
    - clang-format's own wrap-column preferences are **advisory only**: the lint stage reports them without failing, most of the tree already differs from them, and a few columns of difference on a line you added is never a reason to edit, rebuild, or re-run a gate. Do not diff your changes against clang-format output. Check the file's line endings before any scripted edit (this repo's working copies are LF) rather than assuming CRLF on Windows.
- Before committing, run relevant tests/unit tests and ensure build/test results succeed.
- Commit completed task-owned changes with plain git commands only: `git status`, `git add -- <task-owned paths>`, `git commit -m "<message>"`. Use `git add -A` only after verifying every worktree change is task-owned and safe to commit!
- Do not push to cloud unless explicitly requested, generally just commit locally!
- Always consult `llm-wiki/` for code, bug, build, test, config, debugging, or behavior work; for trivial localized work, read only the directly relevant page(s) when needed!
- Keep `llm-wiki/` linted / quality-checked and update it when durable project knowledge changes. For trivial edits with no future-useful context, perform the semantic check but do not edit the wiki!
- Mistrust code, code annotations and llm-wiki! Each of them might be stale or outdated! Come to your own conclusion and act based on that!
- When inspecting an explicitly supplied logs/session/generated-artifact directory, do not use rg --files as the sole file inventory because Git ignore rules can hide relevant .log, .csv, dump, media, or installed-tree files! Enumerate literally with Get-ChildItem -Force (or use rg --files --no-ignore) before selecting files to inspect. Ignore rules govern discovery/staging defaults, not diagnostic relevance!
- When fixing a bug or implementing a feature, generally always add new regression test units, or adjust existing ones!
- When fixing a bug or implementing a feature, add or improve high-signal, rate-limited debug logging when it materially helps diagnose state transitions, failures, or regressions; do not add unconditional hot-path noise!
- The llm-wiki might not get updated after every change; the git commit history might be more up to date!
- Always keep the user informed when the harness (Codex, Claude Code, OpenCode etc.) auto-approval feature denied some steps or caused additional required steps!
- Under the Codex managed filesystem sandbox, including “Approve for me” mode, do not first attempt project build or test commands with default sandbox permissions. MSYS2 child processes cannot reliably create their prefix files there.
    - Invoke `build.py` build/test commands with escalated permissions on the first attempt, including the canonical incremental, clean, verify, resume, focused-test, no-build, lint, and fuzz commands above.
    - Keep escalation scoped to the exact command; never request blanket approval for `python` or arbitrary scripts.
    - If automatic escalation review denies the command, report the denial immediately; do not skip or weaken verification.
- When inspecting a Windows network share, use Get-Item / Get-ChildItem -LiteralPath ... -Force and request scoped escalated read access on the first attempt when the path is outside the workspace. If a mapped drive reports access denied or path not found, do not conclude that the files are absent: resolve the drive to its UNC path using Get-SmbMapping, Get-PSDrive, or Win32_LogicalDisk.ProviderName, then retry the exact UNC path with scoped escalation. Do not remap drives, copy share contents, or modify the share unless explicitly authorized.
- When accessing files under external drive paths, use direct Windows-native `Get-Item/Get-ChildItem -LiteralPath 'H:\captures\…' -Force` with scoped escalated read access!
- When you run tools and test programs in an agentic manner, make sure they run sufficiently long, but not unnecessarily long, and make also sure no unnecessary lingering processes are left behind!

## Engineering rules

- Prefer root-cause fixes over workarounds; do not hide, ignore, weaken, or paper over failures!
- Perform thorough thinking about actual root causes of crashes and other issues for proper fixes!
- If the result after thorough thinking is that proper fixes require bigger changes, they generally should be implemented!
- Do not just mitigate fallout, take the hard route of proper and solid root cause fixes!
- Do not use sleeps, wait tables, polling delays, or timing bandaids as crash/race fixes!
- Do not introduce nor accept racy, timing-sensitive, or fragile behavior!
- Keep source files roughly 600-800 lines maximum; split up files when needed! This is enforced by the lint stage against `tools/file_size_baseline.json`, which records the files still over the 800-line ceiling. Growing a recorded file or pushing a new file past the ceiling fails lint; shrinking folds in automatically, and a file that drops back under the ceiling is removed from the baseline so the space cannot be silently reclaimed. Split toward the 750-line working target rather than sitting just under the ceiling.
    - The ceiling covers C++ (`.cpp/.h/.hpp/.c/.inl`), first-party Python (`build.py`, `tools/`, `testapp/`), and **`llm-wiki/**/*.md`**. The wiki is included because `llm-wiki/log/recent.md` is rolling memory that its own convention archives at ~230 lines; with nothing enforcing that it reached 6212 lines and became the dominant contributor to git history size. Rotate it into `llm-wiki/log/archive-YYYY-Www[letter].md` and refresh `llm-wiki/log/README.md` instead of letting it grow. Markdown outside `llm-wiki/` is deliberately not governed.
- Treat dumps, logs, media, captures, credentials, private keys, tokens, symbols, and user data as sensitive!
- Do not commit secrets, dumps, logs, captures, private-symbol PDBs, large generated artifacts, user names or private user data!

## Non-negotiable project constraints

- Do not disable features to avoid fixing bugs!
- Do not add game-specific compatibility hacks, we only accept generic solutions for all components!
- Do not use D3D11On12 for the DX12 overlay; use native DX12!
- Do not disable the overlay with FSR FG or DLSS FG to prevent crashes; find proper fixes!
- Switching between FG modes must work gracefully both in Talos and GTA validation scenarios: in all directions/combinations, no crashes, no lost overlay rendering, and correct visible FG status!
- The overlay must not be suspended unnecessarily long, also not during FG switching transitions!
- Ideally, the overlay never gets visibly suspended / does never disappear, not even temporarily!
- Inject, overlay etc. must work optimally and gracefully also when other overlay injects like Steam, Rockstar Social etc. are active at the same time!
- The pseudo-overlay (and other GDI crutches) is NEVER a proper replacement for the inject overlay, also not when encountering hard to solve issues with FSR FG or DLSS FG!
- Our inject overlay is or was already working with both FSR FG and DLSS fg in at least some circumstances, and also other similar programs like RTSS work with FSR FG and DLSS FG too. We do not accept that our inject overlay is lacking compatibility with any FG, we definitely can make it work if we try and think hard enough!
- The inject overlay must not be visibly hidden during swap chain or FG transitions (all fg off to FSR FG, all FG off to DLSS FG, DLSS FG to FSR FG, FSR FG to DLSS FG)!
- The inject overlay must have perfect performance optimization, no slow copy operations etc. allowed to improve compatibility with FG! We must find only highest performance solutions!
- Streamline present / active might not necessarily mean DLSS FG would be on or configured to be on later!
- We accept only smoothest video capture possible with CFR, at the same time no audible audio artifacts like audible pitch change, distortion etc. Audio must never have even tiny cut-outs. We accept only 100% perfect same length of all video and audio tracks! All tracks must have perfect sync! This must be true both with WGC and inject capture, all audio codecs (ALAC, AAC, FLAC, OPUS, PCM), multi-track audio, application audio, system audio and microphone, mixing, resampling etc.!
- Also encoder overload must be handled as gracefully as possible, e.g. by repeating / dropping frames with CFR in the smartest way, leading to the least impacted smoothness of a recording as possible, that still must fulfill above's perfect sync criteria etc.!
- Some minimal pitch change over some time is acceptable, as long as it is minimal / not audible!

## Build, diagnostics, and tests

- Fix newly introduced LSP errors/warnings, plus pre-existing issues in touched files or issues that directly block the task; do not expand into unrelated repository-wide cleanup!
- We are paranoid about having sufficient regression tests, better too many than too few!
- Add focused regression tests where possible, especially tests that would have failed before the fix!
- If no regression-test infrastructure exists for the area, consider adding suitable unit infrastructure such as GoogleTest!
- Do not add sleeps or timing assumptions to tests!
- Check whether touched/new code has sufficient unit coverage, and add new test units accordingly!

## Debugging and logging

- We are paranoid about having sufficient debug logging!
- Add high-signal, rate-limited debug logging when it helps diagnose issue root causes, state transitions, failure modes, unexpected runtime conditions, or future regressions!
- Ensure builds preserve useful debug symbols etc. so crash dumps contain actionable information!

## Windows debugging and binary analysis tools

- For crash/debugging work, always analyze relevant available `.dmp` crash dumps; do not inspect unrelated historical dumps during ordinary feature work!
Use the correct symbol path that includes both the Microsoft symbol server AND the local PDB directory:
```
cdb -z crash.dmp -y "srv*;%USERPROFILE%\Programme\build\captureproject\installed\captureengine" -c ".ecxr; k; q"
```
The `srv*`-only path misses CE's local PDBs and produces incomplete stack traces.

- Common installed Windows tools for `.dmp` files, symbol, PE/COFF:

| Tool | Purpose | Installed/default path |
| --- | --- | --- |
| `cdb.exe` | Command-line `.dmp` debugging and stack inspection for 64 bit dump files| `C:\Program Files\Windows Kits\10\Debuggers\x64\cdb.exe` |
| `cdb.exe` | Command-line `.dmp` debugging and stack inspection for 32 bit dump files | `C:\Program Files\Windows Kits\10\Debuggers\x86\cdb.exe` |
| `windbg.exe` | Interactive `.dmp` debugging | `C:\Program Files\Windows Kits\10\Debuggers\x64\windbg.exe` |
| `ffmpeg.exe` | Media conversion/inspection helper for captures | `%USERPROFILE%\Programme\build\captureproject\build\msys64\clang64\bin\ffmpeg.exe` |
| `ffprobe.exe` | Media metadata/probing helper for captures | `%USERPROFILE%\Programme\build\captureproject\build\msys64\clang64\bin\ffprobe.exe` |

## `llm-wiki/` workflow

- `llm-wiki/` is canonical LLM-maintained derived memory, not the sole source of truth.
- When starting work on an unfamiliar area — whether to understand or change it —
  first orient via `llm-wiki/repo-map.md` (the code map: semantic units per
  subsystem, the Python build pipeline units, test matrix, and important paths),
  then read the relevant topic page(s) via `llm-wiki/index.md`, then
  `llm-wiki/log/recent.md` for active/stale-risk areas. Re-check the code map
  after large refactors: monolithic files may have been split into per-area units.
- File names are semantic: the 2026-08-06 conversion removed all numbered
  `*_2/_3` chunks and `_part_*.py` fragments. A few modules keep a small
  logical-source facade (`dx12_hook.cpp`, `mediaengine.cpp`, `wgc_capture.cpp`,
  `layer_capture.cpp`, the `*.py` entry points) that the source-policy reader
  keys on; the real content lives in content-named sibling units. If an archive
  cites a pre-conversion name, resolve it through `repo-map.md` instead of
  grepping the tree for it.
- For substantial work, start with `llm-wiki/index.md`, read only relevant topic pages, then read `llm-wiki/log/recent.md` for active/stale-risk areas.
- Read archives only when historical context is needed or explicitly linked.
- For trivial localized edits, skip broad wiki loading unless the area is unfamiliar or stale-risk is likely.
- If `llm-wiki/` is missing during substantial work, create `index.md`, `overview.md`, and `log/recent.md` by inspecting repo structure, build/test entry points, config, docs, and workflows.
- Mistrust wiki claims until verified against code (but mistrust code too!), tests, build scripts, config, or observed behavior.
- Prefer updating existing pages over creating new ones; create new pages only for reusable topics.
- Keep topic pages focused on current best understanding; put chronology, partial investigations, and temporary notes in `llm-wiki/log/recent.md`.
- Mark uncertainty explicitly as open question, stale-risk, or unverified claim.
- Do not dump raw logs or long command output unless it establishes durable knowledge.
- Update the wiki when durable knowledge changes: architecture, behavior, build/test/package/deploy/debug workflows, bugs/root causes, invariants, conventions, rejected approaches, follow-ups, or code style.
- Do not update the wiki for trivial edits with no future-useful context.
- `llm-wiki/debug-tools.md` contains additional available debug commands and tool paths.
- `llm-wiki/index.md` is a compact routing table with page link, purpose, last verified date, and stale-risk.
- Durable topic pages should include summary, source anchors, invariants, diagnostics/failure modes, open questions/stale-risk, and last verified details.
- `llm-wiki/log/recent.md` is newest-first rolling memory; archive older entries when it gets too long.
- After both wiki updates and code changes, perform a semantic quality check for contradictions, stale claims, duplicates, orphan pages, broken links, missing source anchors, and merge/delete/archive candidates.
