<!--
SPDX-License-Identifier: MIT
Copyright (c) 2026 aufkrawall
-->

# Agent Instructions

## Critical workflow

- Windows-first project: prefer PowerShell 7.6, Windows-native paths, and installed project tools unless there is a clear reason not to.
- After code changes, run `python build.py --skip-updates`; do not use `python build.py --version`.
- Before committing, run relevant tests/unit tests and ensure build/test results succeed.
- Commit completed code changes with plain git commands only: `git status`, `git add -A`, `git commit -m "<message>"`.
- Do not push unless explicitly requested.
- Always consult `llm-wiki/` for substantial code, bug, build, test, config, debugging, or behavior work.
- Keep `llm-wiki/` linted/quality-checked and updated when durable project knowledge changes. So always update it after code changes!
- Mistrust code, code annotations and llm-wiki. Each of them might be stale our outdated. Come to your on conclusion and act based on that!
- Always consider regression tests and useful debug logging for every bug fix!

## Engineering rules

- Prefer root-cause fixes over workarounds; do not hide, ignore, weaken, or paper over failures.
- Make the smallest maintainable change that fully fixes the issue; match local subsystem patterns.
- Avoid broad rewrites, unrelated cleanup, formatting churn, opportunistic refactors, or new dependencies unless justified.
- Keep source files roughly 600-800 lines maximum; split files when needed.
- Do not make tests pass by deleting coverage, weakening assertions, suppressing errors, or changing expected behavior without justification.
- Do not introduce racy, timing-sensitive, or fragile behavior.
- Do not use sleeps, wait tables, polling delays, or timing bandaids as crash/race fixes.
- Prefer existing project utilities and standard-library functionality.
- Treat dumps, logs, media, captures, credentials, private keys, tokens, symbols, and user data as sensitive.
- Do not commit secrets, dumps, logs, captures, private-symbol PDBs, large generated artifacts, or private user data.

## Non-negotiable project constraints

- Do not disable features to avoid fixing bugs.
- Do not add game-specific compatibility hacks.
- Do not use timing bandaids for races or crashes.
- Do not use D3D11On12 for the DX12 overlay; use native DX12.
- Do not disable the overlay with FSR FG or DLSS FG to prevent crashes; find proper fixes.
- Overlay, injection, and frame-generation fixes must stay generic.
- Switching between FG modes must work gracefully in Talos and GTA validation scenarios.
- Switching must work in all directions/combinations: no crashes, no lost overlay rendering, and correct visible FG status.
- The overlay already worked in GTA V Enhanced and Talos Reawakened with FSR FG; expect the same to be possible with DLSS FG.

## Build, diagnostics, and tests

- Keep relevant LSP, formatter, and linter diagnostics active for touched files when available.
- Fix introduced LSP errors/warnings; also fix safe, localized pre-existing diagnostics in touched files.
- Do not perform broad repo-wide diagnostic cleanup unless required by the change.
- Use LSP quick-fixes only when safe, deterministic, and behavior-preserving.
- If LSP is unavailable, stale, or misconfigured, state that and fall back to canonical build/test/lint commands.
- Treat LSP as advisory; `python build.py` and tests remain authoritative.
- We are paranoid about having sufficient regression tests! Add focused regression tests where feasible, especially tests that would have failed before the fix.
- If no regression-test infrastructure exists for the area, consider adding suitable unit infrastructure such as GoogleTest.
- Do not add sleeps or timing assumptions to tests.
- Good tests verify behavior, not only code-path execution.
- Check whether touched/new code has sufficient unit coverage.

## Debugging and logging

- We are paranoid about having sufficient debug logging! Add additional debug logging when it helps diagnose root cause, state transitions, failure modes, unexpected runtime conditions, or future regressions.
- Ensure builds preserve useful debug symbols etc. so crash dumps contain actionable information.
- For media analysis, `ffmpeg.exe` and `ffprobe.exe` are in `C:\Users\TestUser\Programme\build\captureproject\build\msys64\clang64\bin`.

## Windows debugging and binary analysis tools

- Prefer read-only inspection before invasive instrumentation or binary mutation; prefer already installed tools.
- Dumps: use `cdb.exe` from `C:\Program Files\Windows Kits\10\Debuggers\x64`; consider `dumpchk.exe` for readability and `symchk.exe` for symbols.
- Use WinDbg/WinDbgX only when interactive dump debugging is useful.
- Visual Studio/MSVC: use `dumpbin.exe` for PE/COFF headers, imports, exports, dependencies, sections, symbols, and disassembly. dumpbin.exe location: C:\Program Files\Microsoft Visual Studio\18\Community\VC\Tools\MSVC\14.44.35207\bin\Hostx64\x64
- Use `undname.exe` for MSVC C++ decorated symbols, `link.exe /dump` as a dumpbin-style fallback, and `lib.exe /list` for library contents.
- Do not use `editbin.exe` unless explicitly requested; it mutates binaries.
- Windows Debugging Tools: use `gflags.exe` only with explicit intent because it changes debug/runtime settings; use `umdh.exe` for heap/leak investigations.
- Use `dbh.exe`, `pdbcopy.exe`, `symstore.exe`, and `symchk.exe` for symbol/PDB inspection and symbol-store work.
- Sysinternals if installed: `procdump.exe`, `procmon.exe`, `procexp.exe`, `vmmap.exe`, `handle.exe`, `listdlls.exe`, `sigcheck.exe`, `strings.exe`.
- Treat tool output as sensitive; it can contain paths, private symbols, command lines, process details, credentials, or user data.

## `llm-wiki/` workflow

- `llm-wiki/` is canonical LLM-maintained derived memory, not the sole source of truth.
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
- `llm-wiki/index.md` is a compact routing table with page link, purpose, last verified date, and stale-risk.
- Durable topic pages should include summary, source anchors, invariants, diagnostics/failure modes, open questions/stale-risk, and last verified details.
- `llm-wiki/log/recent.md` is newest-first rolling memory; archive older entries when it gets too long.
- After both wiki updates and code changes, perform a semantic quality check for contradictions, stale claims, duplicates, orphan pages, broken links, missing source anchors, and merge/delete/archive candidates.
