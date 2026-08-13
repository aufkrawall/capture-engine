# llm-wiki Log Archive (2026-08-13, shard W33e)

Entries are newest-first. Rotated out of `recent.md` on 2026-08-13 to keep it near
the 230-line rolling-memory ceiling.

### 2026-08-13 - FIXED (refined): suspend only previously loaded tools' threads, not the whole process

- Build 0.1.5988 field results: session `20260813_033707` got the game running but crashed in `nvwgf2umx` on the
  first Present (driver read a garbage command-list state); session `20260813_033912` ran and exited cleanly, but the
  log shows the all-threads suspension GAVE UP ("could not suspend peer threads cleanly ... degraded") — the game
  constantly spawns threads, so the stable-snapshot requirement always fails, and the run succeeded without any
  suspension. Suspending arbitrary game/driver threads is unsafe and unreliable.
- Refined `ToolThreadSuspension` in `hook/main_thirdparty_load.cpp`: only threads whose start address lies inside a
  previously loaded tool module (recorded via `GetModuleInformation` after each successful load) are suspended, and
  enumeration uses two passes instead of a globally stable snapshot. Game and driver threads are never touched; the
  loader-quiescence probe with resume-and-retry remains. Still needs field validation of all three tools.

### 2026-08-13 - FIXED (structural): peer-thread suspension around every tool load after the first

- Session `20260813_031321` proved Special-K-last + quiescence wait is still insufficient: CE's hook thread held the
  loader lock in Special K's DllMain, whose inner LoadLibrary re-entered ReShade/Steam/OptiScaler loader hooks and
  blocked on OptiScaler's mutex, held by an OptiScaler background thread doing NEW loader work. Order and wait alone
  cannot win — both tools have recurring background loader activity.
- Structural fix in `hook/main_thirdparty_load.cpp`: before every tool load after the first, CE waits for loader
  quiescence and then SUSPENDS all other process threads (stable TH32CS_SNAPTHREAD enumeration + bounded
  post-suspension probe with resume-and-retry if a peer was caught inside the loader). Peers are resumed immediately
  after the LoadLibrary returns. Order back to Special K -> ReShade -> OptiScaler: with peers suspended, Special K's
  enumerator cannot hold its thread-hook critical section across a loader call, so OptiScaler's DllMain thread
  creation proceeds. `ShouldSuspendPeerThreadsForToolLoad` added to `hook/common/third_party_load_policy.h`;
  template/README/wiki updated.
