## Most important:

- Always compile with `python build.py --skip-updates` after any code changes!
- Always git commit after code changes, so the worktree is not dirty! But before committing, compile must be successful and also all test unit runs must finish successfully!
- DO NOT PUSH TO A REMOTE unless explicitly requested.
- Always consult the llm-wiki for context of code or context of a bug report!
- Always keep the llm-wiki linted and updated after any code changes!
- Always consider increasing debug logging, also when fixing bugs!
- Always consider adding regression test units, also when fixing bugs!
- DO NOT RUN: python `build.py --version`!

## Scope

- Platform: Windows-first; prefer PowerShell 7.6 and Windows paths.
- Windows-First Workflow: Use Windows-native commands and paths unless there is a clear reason not to.
- Canonical derived LLM memory: `llm-wiki/`.

## Core Engineering Rules

- Prefer root-cause fixes over workarounds.
- Source code files must not get bigger than 600 to 800 lines! Split up into multiple files if required!
- Prefer the smallest maintainable change that fully fixes the issue.
- Match local subsystem patterns instead of imposing new architecture.
- Avoid broad rewrites, unrelated cleanup, formatting churn, and opportunistic refactors unless requested.
- Do not hide, ignore, or weaken failures.
- Do not make tests pass by just deleting coverage, weakening assertions, suppressing errors, or changing expected behavior unless explicitly justified.
- Add regression coverage where feasible when fixing bugs.
- Add or improve debug logging when it materially improves future diagnosis.
- Do not introduce racy, timing-sensitive, or frail behavior.
- Do not use sleeps, wait tables, polling delays, or timing bandaids as crash fixes or race workarounds.
- Do not add dependencies without strong justification.
- Prefer existing project utilities and standard-library functionality.
- Treat crash dumps, logs, media files, captures, credentials, private keys, tokens, and user data as sensitive.
- Do not commit secrets, dumps, large generated artifacts, or private user data.
- Crash dump available? Analyze with cdb.exe in C:\Program Files\Windows Kits\10\Debuggers\x64
- When prompt asks to analyze media file: ffmpeg and ffprobe .exe location is C:\Users\TestUser\Programme\build\captureproject\build\msys64\clang64\bin

## Non-Negotiable Project Constraints

- WE DO NOT DISABLE FEATURES TO AVOID FIXING BUGS.
- WE DO NOT WANT GAME-SPECIFIC COMPATIBILITY HACKS.
- WE DO NOT WANT TIMING BANDAIDS FOR RACES OR CRASHES.
- WE DO NOT WANT D3D11ON12 FOR THE DX12 OVERLAY. USE NATIVE DX12.
- HACKS LIKE COMPLETELY DISABLING OUR OVERLAY WITH FSR FG OR DLSS FG TO PREVENT CRASHES ARE UNACCEPTABLE. FIND PROPER FIXES.

Project invariants:

- Overlay, injection, and frame-generation fixes must stay generic.
- Do not disable features as a workaround.
- Switching between FG modes must work gracefully in Talos and GTA validation scenarios.
- Switching must work in all directions and combinations:
  - no crashes
  - no lost overlay rendering
  - correct visible FG status
- The overlay already worked in both GTA V Enhanced and Talos Reawakened with FSR FG.
- The same is expected to be possible with DLSS FG.

## LSP / Diagnostics Workflow

- Keep the relevant language server, formatter, and linter configured and active for the files being changed.
- Before and after code changes, check LSP diagnostics for touched files and any directly affected neighboring files.
- Fix all LSP errors and warnings introduced by the change.
- Also fix pre-existing LSP diagnostics in touched files when the fix is safe, localized, and consistent with existing project patterns.
- Do not perform broad repo-wide diagnostic cleanup unless explicitly requested or required by the change.
- Use LSP quick-fixes/code actions only when they are safe, deterministic, and preserve intended behavior.
- Do not silence, suppress, or downgrade diagnostics unless there is a documented project convention or a clear technical justification.
- If the LSP is unavailable, misconfigured, stale, or not reporting diagnostics correctly, state that clearly, investigate likely setup issues, and fall back to the project’s canonical build/test/lint commands.
- Treat LSP results as advisory, not a substitute for `python build.py`, tests, or project-specific validation.
- When changing LSP, formatter, lint, or editor configuration, update `llm-wiki/` if the setup or workflow becomes durable project knowledge.

## Tests and Regression Coverage

We are paranoid about regressions.

- If there is no existing regression test unit infrastructure, add one and set it up (e.g. GoogleTest / gtest)!
- Add focused tests that would have failed before the fix.
- Do not add sleeps or timing assumptions to make tests pass.
- Do not weaken existing tests unless expected behavior is intentionally changing.
- Good regression coverage verifies behavior, not just code-path execution.
- Always check if the code, including new one, has sufficient test unit coverage!

## Debugging and Debug Logging

- We are paranoid about having enough debug logging diagnostic evidence, so always check if debug logging can be improved and increased in a meaningful way!
- Add logging e.g. where it helps identify root cause, state transitions, failure modes, or unexpected runtime conditions.
- Also: Make sure we compile binaries with optimal debug symbols etc. , so crash dumps contain valuable information!

## `llm-wiki/` Workflow

`llm-wiki/` is the canonical LLM-maintained derived knowledge layer for this repo. It preserves durable project knowledge across context resets while minimizing token waste.

It is not the sole source of truth.

For substantial code, test, build, debugging, config, or behavior changes:

1. Start with `llm-wiki/index.md`.
2. Read only relevant topic pages.
3. Read `llm-wiki/log/recent.md` when touching active, stale-risk, or recently changed areas.
4. Read archives only when historical context is needed or explicitly linked.

For trivial localized edits, skip broad wiki loading unless the area is unfamiliar or stale-risk is likely.

If `llm-wiki/` is missing during substantial work, create:

- `llm-wiki/index.md`
- `llm-wiki/overview.md`
- `llm-wiki/log/recent.md`

Bootstrap by inspecting repo structure, major subsystems, build/test entry points, config, existing docs, and obvious runtime workflows. Do not invent architecture or behavior.

## `llm-wiki/` Trust and Updates

- Mistrust `llm-wiki`, comments, and prior notes until verified.
- Cross-check important claims against code, tests, build scripts, config, or observed behavior.
- Prefer updating existing pages over creating new ones.
- Create new pages only for reusable topics.
- Keep topic pages focused on current best understanding.
- Put chronology, discovery order, partial investigations, and temporary notes in `llm-wiki/log/recent.md`.
- Mark uncertainty explicitly as open question, stale-risk, or unverified claim.
- Do not dump raw logs or long command output unless they establish durable knowledge.

- Update `llm-wiki/` when changes affect durable reusable knowledge, including:
- primary and favored method to build the project (e.g. central build.py build script), including supported command line arguments
- architecture or component responsibilities
- public/user-visible behavior
- build, test, packaging, or deployment workflows
- debugging procedures
- known bugs, root causes, or failure modes
- invariants, constraints, or conventions
- important rejected approaches
- unresolved follow-up work
- information about desired unified code style

Do not update the llm-wiki for trivial edits with no future-useful context.

## `llm-wiki/` Page Rules

- `llm-wiki/index.md` is a compact routing table. Each entry should include page link, purpose, last verified date, and stale-risk: `low`, `medium`, or `high`.

- Durable topic pages should usually include:
- current summary
- source-of-truth anchors
- invariants/constraints
- known failure modes and diagnostics
- open questions/stale-risk
- last verified date and what was checked

- `llm-wiki/log/recent.md` is newest-first rolling memory. Archive older entries to `llm-wiki/log/archive-YYYY-Www.md` when it becomes too long.

- Always perform a semantic quality check of `llm-wiki/` after wiki updates or before code changes: look for contradictions, stale claims, duplicate content, orphan pages, broken links, missing source anchors, and pages that should be merged, deleted, or archived. If an automated checker exists, run it too.

