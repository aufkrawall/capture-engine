# llm-wiki Log Archive

### 2026-11-12 - Unit-test temp directories: never recreate a deleted name

- Gate 0.1.7517: `CrashSymbolStoreTest` x2 failed with ACCESS_DENIED in `create_directories(<pid dir>/installed)`.
  libc++'s `std::filesystem::remove_all` deletes directories with the legacy delete-on-close disposition, so while
  any other handle is open (Defender scanning the files just written, a just-exited child) the name stays
  delete-pending and creating it again fails with ERROR_ACCESS_DENIED (plain `RemoveDirectoryW` uses POSIX
  semantics here and does not). Proven in `ScopedTestDirectoryTest.AHandleHeldOnADeletedDirectoryNeverBlocksTheNextOne`.
- Rule: fixtures take a fresh `ce::test_support::ScopedTestDirectory` (`tests/scoped_test_directory.h`) per test,
  never one fixed per-process name deleted in TearDown and recreated in SetUp. Converted: crash symbol store,
  target launcher, runtime override paths. The helper skips a delete-pending leftover of an earlier same-pid run.

### 2026-11-12 - Menu (FSR suspended) overlay graph judder: draws are not displayed frames

- GTA `20261112_030715` (0.1.7516, `source=display-change`): in the menu AMD suspends FSR FG, display timing reports
  144.0 FPS (32 us stddev) while CE drew ~157 overlays/s (1568 and 1624 per 10 s). The graph cursor stepped one slot
  per draw, so displayed frames moved 1 or 2 slots unevenly. The saved pacing trace does not span the menu, so this
  is the reproduced hypothesis, not a hardware-proven cause; stale-image reuse was not observed or excluded on HW.
- Fix: one cursor per metrics stream (`PerformanceMetrics::PrepareGraphHistory`, `performance_metrics_graph.inl`)
  instead of one per `OverlayAdapter`, so renderer handoffs keep the position. Under display timing each draw maps to
  the first refresh slot after it (grid from the last published display time, period = refresh period, or the
  sample period when displays are slower); draws sharing a slot share a position, distinct runtime outputs
  (`NextGraphOutput()` at the FFX callback, no-callback topmost, PostSL-FG and runtime-owned draw sites) reserve
  successive slots, a repeated output id does not advance. Presentation timing keeps one slot per draw. Prediction
  is capped at `kMaxDryStreak` slots; measured sample values are untouched.
- Tests (`GraphScrollCursorTest`): 160 draws on 144 Hz with batched publication (0.75-1.25 slots per displayed
  frame, drift < 2 slots over 480 displays; failed 0.38-1.87 before), uneven draw phases, 2x/3x/4x bursts with
  duplicates, stall hold/re-arm, FSR active/suspended handoffs sharing one history, stall recovery.
- Open risk: under VRR or a display timestamp whose phase sits near the draw phase, prediction jitter could map a
  pair of draws to slots 2,0 instead of 1,1. `[OverlayGraph] draws= outputs= samples= samePosition= resets=` (10 s)
  shows draws vs published samples and same-position draws; samePosition ~ draws - samples is expected in the
  menu, a large excess with draws == samples indicates that jitter. Hardware validation pending.
- Not done: the FSR-suspend renderer still rebuilds when switching the UI-clear (fmt 28) and backbuffer (fmt 87)
  roles each suspension (`dx12_ffx_suspend_overlay.cpp` keys one state per proxy) - transition cost only.
