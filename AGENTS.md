<!--
SPDX-License-Identifier: MIT
Copyright (c) 2026 aufkrawall
-->

# Agent Instructions

Keep always-on rules, platform/tool precedence, stop conditions and compressed critical commit gates
here. Full procedures, style guides and worked examples belong in the referenced `llm-wiki/` pages;
avoid maintaining the same detailed rule in both places. Preserve project-specific constraints.

## Critical workflow

- Windows-first project: prefer PowerShell 7.6 and Windows-native paths. Use the declared build system, package manager, toolchain and pinned/project-local tools; never silently substitute another ecosystem, tool version or global tool. If required declarations conflict or a tool is unavailable, stop dependent work and report the conflict/absence instead of working around it.
- **Default development loop:** `python build.py --incremental --tests-only --run-tests --gtest-filter=<suite-or-test> --skip-updates --concise` (~5-7 s; reuses the current product identity). Stay in this loop while writing code; do not rebuild the product after every small edit, and never use `python build.py --version`! Product code the tests-only build does not compile can be checked in seconds with `python tools/refactor/syntax_check.py --changed`.
- **Closing gate for every change, all code areas (capture, CFR, FG, audio included):** one `python build.py --incremental --run-tests --skip-updates --concise` (~3 min unfiltered in the 2026-10-05 run; `--gtest-filter` allowed). It compiles changed units, reuses content-validated objects, verifies PE/binaries and runs native tests plus matching FG flow scenarios; unfiltered runs also execute Python tool self-tests (no linters or sanitizers). It must leave a fresh `build/packages/captureengine-setup-<version>.exe`, so never pass `--skip-package` to it (~20 s, it is what the user installs). Portable 7z archives stay opt-in (`--portable-archives`, used by `release-stable`).
- Clean gate `python build.py --skip-updates --concise` instead when stale artifacts are suspected, when asked, or when the change touches dependencies, toolchain, FFmpeg configuration or generated-build machinery; `--force-rebuild` additionally deletes `build/obj`.
- **Pre-release / validation-only gates, never per change:** lint `python build.py --no-build --lint --skip-updates --concise` (~11 s; clang-format, flake8, pyright, clang-tidy ratchet; prefer after a full product build, a tests-only compile database only covers part of the tree); `--verify` (static release-candidate gate: content-validated build, full suite, Python self-tests, lint and file-size ratchets, ASan/UBSan; includes isolated WARP FG scenarios but no optional integration matrix or fuzz runs); `--verify --verify-clean` only for changes to `build.py`, toolchain/compiler/linker/hardening policy, shared ABI/layout or gate policy; `--verify-runtime` adds the integration matrix and fuzz targets. x86 sanitizers do not exist (`--sanitize-x86` fails closed). Details: `llm-wiki/build.py.md`.
- A product build that failed after starting: fix the cause, then `python build.py --resume --skip-updates --concise` (only for the immediately preceding failed top-level build); if resume refuses, use the normal gate. Tests without recompiling: `python build.py --no-build --run-tests --skip-updates --concise`.
- **clang-tidy ratchet** (`tools/clang_tidy_baseline.json`, compiler warnings included as `clang-diagnostic-*`): any count above baseline or a new check fails lint; lower counts fold in. The ~2.2k baseline is accepted debt to reduce; regenerate it (`--update-lint-baseline`, only after a full product build) only when an increase is genuinely justified, never to make a new warning go away.
- **Fuzzing** (`python build.py --run-fuzz [--fuzz-seconds N]`) when changing parsers, deserialization state machines or untrusted input boundaries (e.g. `common/config/config.cpp`, `common/ipc/process_ipc.cpp`), not for new config keys. Register new harnesses in `FUZZ_TARGET_CORPUS` with a committed seed corpus (`llm-wiki/fuzzing.md`).
- **Stable releases** via the `release-stable` action: follow `llm-wiki/build.py.md` ("Stable release operations", "Release asset privacy"); the self-hosted runner is this PC and must be started manually (`%USERPROFILE%\Programme\build\actions-runner\run.cmd`) before dispatching.
- Use `--concise`; status is in `build/verification/latest_summary.txt` / `latest_manifest.json`, full output in that run's `build.details.log`. Never re-run a gate just to see its output again! Keep large logs, generated output, traces, dumps and minified files out of working context unless needed; read summaries or targeted ranges and retain full output locally as evidence.
- **Verify the change itself:** prefer a check that fails without the change; otherwise inspect the resulting artifact (existence, content, format or size), or confirm from actual output that the relevant tests/build ran. Exit status alone is insufficient. State what was and was not verified when a direct check is impractical.
- Match the surrounding code's indentation, naming, comment density and line endings; keep edits narrowly scoped and inspect `git diff --check` / `git diff` before building. Never run `clang-format -i`, `python build.py --format` or any whole-file formatter on existing sources unless asked.
- **Always update CHANGELOG.md before committing** task-owned fixes, improvements and features under `## Unreleased`, following `llm-wiki/changelog-guidelines.md` for user-facing entries and matching release notes. Validate with `python tools/manage_changelog.py --validate`.
- **Always commit completed, verified changes** locally (tests passed, CHANGELOG included): `git status`, `git add -- <task-owned paths>`, `git commit`. `git add -A` only after verifying every worktree change is task-owned. Do not push, publish, deploy, release or otherwise alter remote state unless asked or explicitly required by an authorized repository workflow. Commit messages use a concise title plus a short bullet-point body for non-trivial changes, stating what changed and why.
- **Secret safety is a commit gate:** before every commit, review staged/task-owned untracked content and the planned message and run available secret scanning; immediately afterward, inspect and scan the exact commit patch and metadata. Follow `llm-wiki/secret-leak-prevention.md`, including its mandatory manual fallback when scanners are unavailable. Any suspected leak blocks commit/sharing until resolved; never reproduce full secrets in reports.
- **Split up non-trivial tasks** into a series of small, self-contained commits (no need to compile for every commit!), not one large commit, so later review stays easy; each commit must independently hold together and pass verification and secret-leak checks.
- **Trust model:** code, tests and build scripts are the source of truth. Consult `llm-wiki/` for architecture and background, verify its claims against code and recent history, and update it when durable knowledge changes.
- **Session logs:** start every investigation with `python tools/log_digest.py <session-dir>` (files incl. which are vendor logs, processes, grouped problems, transition timeline, lost lines, dominating templates; a few KB instead of megabytes; `--pid`, `--since`/`--until` narrow it), then grep what it points to. Inventory supplied log/artifact directories literally (`Get-ChildItem -Force` or `rg --files --no-ignore`): Git ignore rules hide .log/.csv/dump/media files but do not decide diagnostic relevance.
- Network shares and external drives: `Get-Item` / `Get-ChildItem -LiteralPath ... -Force` with scoped escalated read access on the first attempt. A mapped drive reporting access denied or not found is not proof of absence: resolve its UNC path (`Get-SmbMapping`, `Get-PSDrive`, `Win32_LogicalDisk.ProviderName`) and retry that. Never remap drives or copy or modify share contents unless authorized.
- Every bug fix or feature gets new or adjusted regression tests and high-signal, rate-limited debug logging where it helps diagnose transitions, failures or regressions (no unconditional hot-path noise).
- Tell the user whenever the harness's auto-approval denied a step or forced extra steps. Under managed sandboxes (e.g. Codex), MSYS2 children need elevation to create prefix files: request scoped escalation for the exact `build.py` invocation on the first attempt (never blanket approval) and report denials without skipping verification.
- Tools and test programs you run must run long enough but not longer, and leave no lingering processes.
- Note: Gate needs more than 300s!

## Engineering rules

- Preserve intended features, compatibility guarantees, performance characteristics and public contracts unless the requested change intentionally alters them.
- **Root-cause fixes over workarounds:** analyze crashes and regressions to the root; no papering over, timing band-aids, sleeps or polling delays, no mere mitigation. If the proper fix needs structural change, make it. Never introduce or accept racy, timing-sensitive or fragile behavior!
- Source files stay at roughly 600-800 lines (working target 750) - split into logical units, never pack statements or paragraphs onto runaway lines to meet the limit. Lint enforces the 800-line ceiling for C++ (`.cpp/.h/.hpp/.c/.inl`), first-party Python (`build.py`, `tools/`, `testapp/`) and `llm-wiki/**/*.md` against `tools/file_size_baseline.json` (growth or a new file over the ceiling fails; shrinking folds in). Rotate `llm-wiki/log/recent.md` (~230 lines) into `log/archive-YYYY-Www[letter].md` and refresh `log/README.md`.
- Treat dumps, logs, media, captures, credentials, keys, tokens, symbols and user data as sensitive; never commit secrets, dumps, logs, captures, private-symbol PDBs, large generated artifacts, user names or private user data.

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

## Tests, diagnostics, logging

- Fix new LSP errors/warnings plus pre-existing ones in touched files or ones that block the task; no unrelated repository-wide cleanup.
- We are paranoid about regression tests and debug logging - better too many than too few; coverage and diagnosability are deliverables, not polish.
- For every bug fix or behavioral correction, assess regression coverage and diagnostics even when existing tests pass; strongly prefer a focused test that fails before the fix and passes after. Features cover the new contract and important edge cases. Where no infrastructure exists, consider adding it (GoogleTest).
- No low-value tests just to satisfy a rule: if automation is impractical, keep a reproducible verification method and say why automated coverage was omitted. Say so too when you deliberately add no coverage or diagnostics for a non-trivial behavioral change.
- No sleeps or timing assumptions in tests.
- Logging: high-signal and rate-limited around state transitions, inputs, boundaries, recovery paths and failures; non-secret, low-overhead and economical to consume (human- and token-efficient): single-line entries with stable prefixes, one log per distinct event, repeats rate-limited with counters and summaries, collections capped and long values truncated (first few items plus totals, sizes/hashes instead of full bodies), verbose detail behind an explicit flag. Repeated lines use `ce::log_meter::ChangeGate` (log on change, count repeats) - see `llm-wiki/regression-testing-and-logging.md`.
- Builds must keep useful debug symbols so crash dumps are actionable.

## Test apps and computer use

- Prefer scripted, API-, CLI- or harness-driven verification (including scripted input and screenshots) over interactive computer use; computer use remains allowed when GUI interaction itself is what must be verified.
- Keep runs short and bounded: start with a brief duration, extend only when evidence requires it, give every started process an explicit stop condition, and never leave tools or test programs running longer than needed.
- Own the full lifecycle: shut down everything you started, including child processes, when done or on failure, then confirm nothing lingers in the background.
- Start interdependent apps in dependency order and let each signal readiness (open port, created file, health check, visible process state) before starting the next; use only a brief stagger when no such signal exists.

## Windows debugging tools

- Inspect relevant dumps, logs, traces, symbols and produced artifacts when they can establish a failure or root cause. For crashes, always analyze the relevant `.dmp` files with both Microsoft symbols and matching CE PDBs; follow `llm-wiki/debug-tools.md` for commands, architecture selection and archived session symbols.
- On Windows, use `tools/discover-debug-tools.ps1` / its `debug-tool-manifest.json` for machine-specific path evidence. Verify documented tool availability before use; paths are examples unless declared mandatory. Keep SDK/MSVC discovery centralized in the helper.
- Do not mutate global debugger flags, registry/system settings, binaries/symbols used as evidence or persistent environment state unless explicitly requested and justified.

## `llm-wiki/` workflow

- `llm-wiki/` is canonical LLM-maintained derived memory, not the sole source of truth; mistrust its claims (and the code) until verified against code, tests, build scripts, config or observed behavior.
- Unfamiliar area: `llm-wiki/repo-map.md` (where code lives) -> the topic page(s) via `llm-wiki/index.md` -> `llm-wiki/log/recent.md` for active/stale-risk areas. Archives only when history is needed or linked. Skip broad wiki loading for trivial localized edits.
- Source layout, include conventions and logical-unit test rules: `llm-wiki/repo-map.md`; refactor program and rules: `llm-wiki/refactor-roadmap.md`. Resolve stale wiki paths by basename.
- Update the wiki when durable knowledge changes (architecture, behavior, build/test/package/debug workflows, root causes, invariants, conventions, rejected approaches, follow-ups, code style), not for trivial edits. Prefer updating existing pages; new pages only for reusable topics. Topic pages hold current best understanding (summary, source anchors, invariants, diagnostics/failure modes, open questions/stale-risk, last verified); chronology and partial investigations go to `log/recent.md` (newest first). Mark uncertainty explicitly. No raw logs or long command output unless they establish durable knowledge - but put decisive evidence itself in the wiki, since session logs are not kept.
- `index.md` is a compact routing table (page, purpose, last verified, stale-risk). If `llm-wiki/` is missing during substantial work, create `index.md`, `overview.md` and `log/recent.md` from the repo structure, build/test entry points, config and workflows.
- After wiki and code changes, check semantically for contradictions, stale claims, duplicates, orphan pages, broken links, missing source anchors, and merge/delete/archive candidates.
