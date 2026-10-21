# Phase 2: the in-process engine

Status: **adopted 2026-10-20** (user decision); M12-M18 implemented on `claude/libcengine-refactor-phase-2-e316d1` (`85e102b2`..`38e6c20b`, DR-37..DR-48): the roles run in the client process by default; M19 (hardware matrix, outside-repo run in both modes) is the user's. Phase 1 (M0-M9, this directory's other
pages) made `cengine.dll` the engine's control layer and the app its client, but the work itself still
runs in helper processes (`captureengine.exe --mode=inject|media|logger|sensors`). The user wants third
parties to integrate CaptureEngine natively, without any CaptureEngine process. Phase 2 does that and
keeps today's helper processes as an opt-in isolation mode. **Since M20 (2026-10-21, [11](11-helper-isolation.md),
DR-49)** the app sets `CE_FEATURE_ISOLATED_HELPERS` and runs helper processes by default again; SDK clients stay
in-process, and `[Runtime] isolated_helpers=auto|on|off` overrides either.

## Target

| | Default: in-process | Opt-in: isolated helpers |
| --- | --- | --- |
| Selected by | SDK clients without the feature bit; any client with `[Runtime] isolated_helpers=off` | `CE_FEATURE_ISOLATED_HELPERS` (the app, since M20); any client with `isolated_helpers=on` |
| Inject, media, logger roles | runtime-owned threads inside the client process | helper processes, exactly as today |
| `mediaengine.dll` + FFmpeg | loaded by `cengine.dll` into the client process | loaded by the media helper |
| Sensors (.NET LibreHardwareMonitor, PawnIO) | stays a helper process (or off): hosting the CLR in a client process is intrusive | helper process |
| A crash in capture/encode | crashes the client (documented; the CE crash handler only with `CE_FEATURE_CRASH_HANDLER`) | kills the helper; the engine reports HELPER LOST and recovers |

Not CaptureEngine client processes in either mode, unchanged: the hook DLLs and Vulkan layer inside
games, and the elevation service (a Windows service).

### What still runs in a separate process after phase 2

| Item | Why | When |
| --- | --- | --- |
| Hook DLLs, Vulkan layer | They run inside the game; inherent to game capture | game capture |
| Elevated setup actions (PawnIO install/remove, elevation service, launch elevated) | UAC elevation needs its own process | one-shot setup only |
| Sensors bridge (.NET LibreHardwareMonitor) | DR-35 | only with hardware sensors enabled |
| Elevation service | a Windows service | only if installed |
| Crash-dump writer (`captureengine.exe` dump helper) | writes the dump from outside the crashing process | only with `CE_FEATURE_CRASH_HANDLER`, at a crash |
| Per-application audio capture (`--process-loopback-worker`, one disposable worker per captured app, `llm-wiki/multi-audio-capture.md`) | AudioSes `CLoopbackMixer` teardown crashes its owning process, so the COM graph is abandoned and reclaimed by the worker's exit (DR-43) | app audio sources only |

Without sensors and per-application audio a client runs 100% in-process; per-application audio keeps
its disposable `captureengine.exe` worker in both modes (DR-43).

One role code base serves both modes: role logic is written against a host interface (commands,
events, shared state, logging sink), never against "this process". The process host is a thin `WinMain`
around the same code (today's role prelude), the thread host is a runtime component.

## What moves (sizes measured 2026-10-20 on `claude/libcengine-refactor-orchestrate-e6c27c`)

| Part | Size | What it takes |
| --- | --- | --- |
| Inject role (`captureengine/injection/`) | ~5.1k lines | The injection manager is already linked into `cengine.dll` (DR-15). The role's main loop, config publication to hooks and the control channel become a component. Self-exclusion must be by PID (never inject into the client). |
| Media role (`captureengine/media/`) | ~28k lines, 65 `media_main_g_*` process globals | Globals become a per-recording `MediaSession`. Teardown that today relies on the process exiting (D3D devices, VRAM, encoder sessions, threads, handles, COM/MF) becomes explicit and verified. The finalization result (today: coded exit code + inherited result slot, DR-23) becomes a direct callback. |
| `mediaengine.dll` | ~44k lines, singleton C API (47 `MediaEngine_*` exports over global state) | Instance (handle-based) API, so two pipelines can live in one process. |
| Logger role | small | A thread. |
| Logging (`common/logging`) | process-global, one file per process | Per-role log sinks bound to threads, writing today's files (`inject.log`, `media_<id>.log`, ...): those names are diagnosis contracts (`tools/log_digest.py`, wiki). |

## Design problems (decide in M12, before code)

1. **Two live media pipelines.** Today recording N finalizes in its retired media process while N+1
   starts (overlap is a feature). In-process this needs instance state in both the media role and
   `mediaengine.dll`. Rejected: serializing recordings (a new recording would wait for the previous
   file; that removes a feature).
2. **Explicit teardown.** Every resource the media and inject roles create needs an owner and a
   verified release. Leak tests per recording cycle: handles, GDI objects, threads, D3D live objects,
   process video memory (`[GpuMemory]` counters), encoder sessions.
3. **Process-wide effects in the roles.** Crash handler per role process, timer resolution, MMCSS,
   process priority and power throttling, COM apartment and MF startup, `SetDllDirectory` for FFmpeg,
   console handlers, `g_SessionDirName`/`g_RecordingId`, the role host's `RuntimeConfiguration()` owner,
   `Log_Init`. Each gets one of: per-thread, per-instance, owned by the runtime (once per process), or
   opt-in feature. Nothing may silently change the client process.
4. **Transport.** Roles talk over authenticated pipes and shared memory (`ProcessIPC`). The thread host
   keeps the same command protocol over an in-memory transport, so role loops stay shared; results
   arrive by callback.
5. **Hooks ↔ engine.** Shared-memory names and owner checks between hooks and the inject role (verify
   whether they are keyed to the inject process id) must work when the owner is a client process.
6. **Recovery.** In-process there is no "helper lost": a stalled role must be stopped and restarted as a
   component (bounded, observable), or the runtime fails cleanly.

## Milestones

| # | Milestone | Content | Gates |
| --- | --- | --- | --- |
| M12 | Audit and design (read-only) | Inventory of process globals and process-wide effects in media, inject, logger, `mediaengine`, `common/logging`; a decision per item; the role-host interface; test plan. Recorded here and in 08. | wiki only |
| M13 | Role-host seam + logger in-process | `RoleHost` interface in `runtime/children`: `ProcessRoleHost` (today's `ChildSupervisor`) and `ThreadRoleHost`; in-memory `ProcessIPC` transport; result callback; `CE_FEATURE_ISOLATED_HELPERS`; the logger role as the pilot. | close |
| M14 | Logging sinks | Thread-bound per-role sinks in one process, same files and names; rate limits and the crash-time flush keep working. | close |
| M15 | Inject role in-process | Main loop as a component; self-exclusion by PID; shared-memory naming verified; both modes tested. | close, verify |
| M16 | `mediaengine` instances | M16a: state into the `MediaEngine` instance behind unchanged exports; M16b: `MediaEngine_Create`/`Destroy`, handle on 40 exports (DR-42); two concurrent instances tested (encode + mux + audio). | close, verify (ASan) |
| M17 | Media role as an instance | 65 globals into `MediaSession`; explicit teardown; leak tests per cycle; finalization by callback. | close, verify, user hardware round |
| M18 | Media in-process, default switch | `ThreadRoleHost` runs media with overlapping recordings; in-process becomes the default; isolated mode unchanged; app option. | verify-clean, user hardware rounds |
| M19 | Validation and SDK | User hardware matrix (WGC and inject capture, CFR smoothness, A/V sync and track lengths for every audio codec, multi-track, encoder overload, FG switching in Talos and GTA, overlay with Steam/RTSS), `tools/run_sdk_outside_repo.py` in both modes with the client outside the package. | verify-runtime, user |

Phase 1's optional M10 (`cengine_host.exe`) is superseded: the isolated mode's process host is the
role-host executable, and package paths resolve from `cengine.dll` in both modes (part of M15).
M11 (recording statistics) stays optional and becomes simpler in-process.

## Effort and risk

Comparable to phase 1 in size (phase 1 on its branch: 119 commits, 392 files, +28k/-5k lines), and
riskier: phase 1 moved control code and added an API, phase 2 changes the capture/encode core under the
project's strictest requirements (CFR smoothness, perfect A/V sync, no audio dropouts, FG switching).
The long pole is the hardware validation rounds of M17-M19, which only the user can run.

## Branching

Phase 1 lands first: the user's installer test of `claude/libcengine-refactor-orchestrate-e6c27c`, the
overlay ASan fix (separate session, from `main`), the phase-1 `--verify --verify-clean` gate, then a
rebase onto `main` (no merge commits on `main`). Phase 2 starts on a fresh branch and worktree from
`main` after that. If it must start earlier, cut it from the phase-1 tip and rebase once phase 1 lands.
