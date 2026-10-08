# libcengine: library-first refactor plan

Status: **adopted** 2026-10-08 at implementation baseline `7ebebd43`; design written 2026-10-07.
It covers the library goal (D8/D9/D13) of `llm-wiki/architecture-debt-plan.md`, and reorders that
plan so the library is the critical path. The graphics/media debt waves continue as separate tracks.
This directory is the canonical library plan; the old temporary plan copy is removed.

## Goal

1. Third parties can use every CaptureEngine engine feature through one small, stable,
   versioned C API (`cengine.dll`, `include/cengine/cengine.h`).
2. The shipping CaptureEngine app (tray, hotkeys, CLI) is a client of that same API. It uses no
   private headers, no engine globals and no side channel.
3. Internally, the engine is a handful of deep modules (Ousterhout / Matt Pocock): each one has a
   small interface and hides a large policy and lifetime. Dependency rules are checked by a tool,
   not by convention.

## Reading order

| File | Contents |
| --- | --- |
| [01-assessment.md](01-assessment.md) | Why the current plan is reshaped; what to keep from it |
| [02-target-architecture.md](02-target-architecture.md) | Binaries, processes, module map, dependency rules, threading, process-wide effects |
| [03-public-api.md](03-public-api.md) | Full C ABI specification, header draft, C++ wrapper, semantics, error model |
| [04-runtime-modules.md](04-runtime-modules.md) | Internal deep modules: interfaces, hidden policy, migration map from today's code |
| [05-frontend-and-role-host.md](05-frontend-and-role-host.md) | Turning `ControllerMain`/`WinMain` into a thin client plus a role host |
| [06-milestones.md](06-milestones.md) | Ordered milestones with commit series, acceptance criteria and gates |
| [07-testing-and-verification.md](07-testing-and-verification.md) | Test strategy through interfaces, ABI tests, headless client, depth metrics |
| [08-decisions-and-open-questions.md](08-decisions-and-open-questions.md) | Decision records (with rejected options) and open questions to verify |

## Ground rules for implementing agents

- `CLAUDE.md` wins on gates, commits, changelog, secret scanning and project constraints. This plan
  never authorizes skipping a gate.
- Implement one milestone of [06-milestones.md](06-milestones.md) at a time, as the listed small
  commits. Mechanical moves and policy changes always go in separate commits.
- Facts marked **(verified 2026-10-07)** were read from source at `644219f2`. Facts marked
  **(verify)** must be checked against code before you rely on them. Code wins over this plan.
- Do not reopen a decision in `08-...` without new evidence. If evidence changes a decision, record
  it there (date, evidence, new decision) before writing code.
- Keep the status table below current at the end of every commit series. It is the only status
  record for this plan; don't add narrative status elsewhere in these files.
- Maintain these pages only in `llm-wiki/library/`; index.md routes here. D8/D9/D13 in
  architecture-debt-plan.md are superseded. Keep historical evidence in llm-wiki/log/, not a second plan.

## Status

| Milestone | State | Last commit | Notes |
| --- | --- | --- | --- |
| M0 Adopt plan, move docs into the wiki | complete | `a50d1600` | Canonical plan; old D8/D9/D13 superseded; historical evidence archived; links and changelog validated. |
| M1 Module boundary checker | complete | `4f584251` | 19 exact exceptions; 3645 edges; 23 boundary/gate fixtures (31 with lint tests). Lint and unfiltered close enforce rules; native/Python/ASan/32 FG checks and setup 0.1.7026 pass. Internal reach: DX12 74 (2 outside), media 29 (0 outside); runtime/frontend absent. |
| M2 Public header v2 + ABI tests (unshipped) | in progress | draft commit | Full C draft and wrapper compile as C11/C++20. Fixed x64 packing; wrapper lifetime/error/timeout rules documented. Permanent layout/C build tests remain. No exports or runtime implementation. |
| M3 Runtime core inside the exe | not started | | |
| M4 C ABI implemented; frontend on the API; v1 removed | not started | | |
| M5 Settings API | not started | | |
| M6 Complete events (finalized output, screenshot paths, helpers) | not started | | |
| M7 `cengine.dll` + WinMain split | not started | | |
| M8 SDK package + headless client + outside-repo proof | not started | | |
| M9 Setup API, monitors, client logging | not started | | |
| M10 Optional: role-host split (`cengine_host.exe`) | not started | | |
| M11 Optional: recording statistics | not started | | |
