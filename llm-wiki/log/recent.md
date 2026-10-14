# llm-wiki Log

### 2026-10-14 - Streamline takeover lost an init-end race; hook now loads by loader-init APC

- User report (session `20261014_190537`, 0.1.7145): Witcher 3 kept its own `sl.interposer.dll`. Rollback
  logged `slInitGateEntered=0`. All seven retained W3 processes show the outcome follows injection timing
  (remote thread and initial thread released together at init end), not the recent audit commits.
- `InjectImpl` now tries `ce::loader_init_apc::QueueBeforeEntryPoint` first: APC on the suspended initial
  thread when PEB proves init has not reached `NtTestAlert`. PEB semantics verified with scratch probes on
  this machine. Native tests use suspended and debug-held `cmd.exe` children. Details:
  [startup runtime overrides](../startup-runtime-overrides.md#arming-before-the-games-first-slinit-the-loader-init-apc-route-2026-10-14).
  Live retest pending; the next inject.log shows the route per injection.

### 2026-10-14 - Compose OTA preferences with static Streamline takeover

- Audit `c0f63371` required exact takeover gates at activation, while `ngx_ota=off` stacked CE's own
  `Hooked_slInit` over that gate. The new real-hook flow failed before the correction: direct identity
  still answered from the resident copy. OTA filtering now shares the selected 2.x provider's entry gate;
  static/dynamic calls verify the configured provider/core paths and zero resident initialization calls.
- Foreign ownership still declines before loading the override. Worker rollback retains an installed OTA
  gate as a cached resident fallback. A second deliberate failure (resident flags 72 instead of zero after
  worker cleanup) verifies that later resident initialization also keeps the preference policy.
- Session `20261014_183445` armed takeover then observed the core before any intercepted `slInit` call.
  It does not independently prove the reproduced ownership rejection caused that startup. Rollback now
  reports gate entry and OTA installation/observation; live Witcher 3 retest remains pending.
- Unfiltered resumed closing gate `20261014_185257_build_7144` passed: full native suite, 41 Python groups,
  all 78 isolated FG flows, both hook architectures, PE/PDB/privacy checks and setup. After the resident
  fallback correction, final focused gate `20261014_190137_build_7145` passed all affected native suites,
  nine takeover/early-loader flows and fresh setup 0.1.7145 (39,997,470 bytes). No optional lint,
  sanitizer/fuzz/runtime matrix or live game execution. Contract: [startup runtime overrides](../startup-runtime-overrides.md#import-coverage-and-direct-rebinding-2026-10-14).

### 2026-10-14 - Fix three recent-commit review findings

- Borrowed gamma resources in no-callback native FSR batches used their ordinary queue-fence retirement,
  adding 40 CE runtime-queue Signals over the 20-frame WARP reproduction. Embedded recording now appends
  a per-resource-generation GPU marker; observed submission publishes its token without a queue Signal.
  Native tests cover held GPU execution, trim, marker wrap and unsubmitted abandonment back to fence retirement;
  the real-hook queue observer reports zero added Signals while gamma still applies.
- Export coverage did not prove import ownership. A foreign IAT wrapper retained the resident interposer
  while override slInit initialized. Arming/activation now check exact predecessors; direct slots migrate
  transactionally before custom targets publish. Entry callers join binding, while module queries stay
  nonblocking. Native tests exercise rollback/publication; both foreign-owner flow timings keep one resident.
- Screen-grab compensation now resolves the actual captured window PID, matches the ring's source/proven
  renderer and validates the entire estimate's publication generation. Unrelated/unknown targets and resets
  preserve the probe latency/audio anchor. Native policy regressions cover those boundaries.
- Unfiltered closing gate `20261014_181705_build_7142` passed: full native suite, 41 Python tool groups,
  all 74 isolated FG flows, both hook architectures, PE/PDB/privacy verification and fresh setup 0.1.7142.
  Final focused gate `20261014_182650_build_7143` also passed after the typed-PE-slot review adjustment
  and abandonment regression: all affected native suites, 10 FG flows, both hook architectures and fresh
  setup 0.1.7143 (39,994,844 bytes).
  No lint/sanitizer/fuzz/runtime integration or live GTA/Talos/A/V recording validation.
- Current contracts: [Streamline takeover](../startup-runtime-overrides.md#import-coverage-and-direct-rebinding-2026-10-14),
  [FSR retirement](../cfr-capture-sync.md#native-no-callback-fsr-capture-handoffs-2026-10-13),
  [screen-grab compensation](../wgc-capture.md).

### 2026-10-14 - Streamline/DLSS override review: import coverage, UTF-8 paths, DLSS diagnostics

- Review of the static `sl.interposer.dll` takeover (2026-10-09 to 10-13 commits). Installed testapp
  `sl.interposer.dll` 2.11 exports 262 names; the takeover thunked 48, so D3D11 and ~200 Vulkan imports stayed on
  the dormant game-directory copy after activation. Arming now gates on import coverage (override export table
  read from disk; ordinal/nameless/missing imports decline) and activation rebinds every unthunked import
  directly; D3D11 device creation became an entry thunk (12-argument thunks).
- Config values are code-page text but the redirect compared/loaded them as UTF-8: a non-ASCII override folder
  failed the load and made CE's own sl.common look foreign. All four override paths are now immutable UTF-8
  snapshots; module paths are uncapped. Regression: the flow harness wrote the local config directly and lost
  its override path until `CEFlow_Init` published the snapshot (caught by `FlowLoaderBootstrap`).
- DLSS: NGX's own snippets live as `models\dlss|dlssg|dlssd\versions\<v>\files\160_<appid>.bin` PE files
  (this machine) and bypass `dlss_*_dll_path`; CE now logs per snippet whether the configured file is in effect.
  Redirecting the `.bin` loads was not attempted without hardware evidence.
- Not changed: proxy entry exports may still claim the override while a pre-gate resident `slInit` runs (now
  logged as `SPLIT RUNTIME`); restricting claims to `slInit` would split pre-`slInit` DXGI users. The dormant
  creator role was left in place pending the user's decision.
- Verification: native suite, new `StreamlineTakeoverImports`/`RuntimeOverridePath*` tests, flows
  `FlowStreamlineTakeover.*` (logs show `ACTIVE ... 1 thunked + 1 direct` and the coverage decline), fuzz
  `pe_file_exports` 1.76 M runs; closing gate `20261014_173509_build_7141` passed (458 s, both hook
  architectures, 71 flows, setup 0.1.7141). No lint/sanitizer run, no live game run. Sources: [startup-runtime-overrides](../startup-runtime-overrides.md#import-coverage-and-direct-rebinding-2026-10-14).

### 2026-10-13 - Preserve inject recording across DLSS/FSR mode switches

- Installed 0.1.7137 session `20261013_170631`, GTA V Enhanced: producer frame 5297
  is the last copy at 19:04:18.85, before the new FSR output chain. No FSR capture
  is published before stop at 19:05:02.86; encoding/audio continue. The safe
  no-callback Present bypass had no recording producer.
- Capture now joins the learned final ECL batch. Inline copy markers acknowledge
  the shared fence from existing Present/maintenance passes, with no runtime queue
  Signal, separate ECL, CPU wait, new queue or additional pixel-copy pass.
- Overlay-excluded copies follow post-processing and precede CE drawing. Proxy
  passthrough uses its existing owner-queue submit. Outgoing callback frames retain
  their proven copy queue after OFF; the pixel regression exposed a real D3D12
  cross-queue read/write error at this seam and verifies its removal.
- Five WARP flow regressions read shared-texture pixels/leases across both runtime
  directions, callback/no-callback FSR, suspend/resume, inclusion/gamma and saturated
  textures. Native WARP marker/fence tests pass. Removing the producer deliberately
  fails with zero FSR frames/shades; restoring it passes. A wrapped marker token is
  cleared before slot reuse; the deterministic regression rejects premature completion.
- Unfiltered closing gate `20261013_194922_build_7138` passed in 383.4 s: full native
  suite, 41 Python groups, all 68 FG flows, both hook architectures and PE/PDB/privacy
  checks. Final focused gate `20261013_200557_build_7140` passed in 124.8 s after the
  marker-wrap guard and hidden-overlay post-process edge: completion regression,
  all five capture flows, both architectures and fresh setup 0.1.7140 (39,862,244 bytes).
  No optional sanitizer/lint/fuzz/runtime matrix.
- Hardware GTA/Talos recording and decoded A/V comparison remain unverified.
  Current sources/invariants: [CFR capture sync](../cfr-capture-sync.md#native-no-callback-fsr-capture-handoffs-2026-10-13).

### 2026-10-13 - Keep validated hardware readings during delayed polling

- Installed 0.1.7134 session `20261013_111644` loses optional CPU/GPU sensor validity
  at 15:03:08.226, 16:30:13.025 and 16:34:49.339; recovery follows after 2.034,
  1.018 and 3.042 seconds. Usage/VRAM remain valid, the sensor service continues
  updating, and the broker never reports a reconnect/error. The earlier drop has
  100% total CPU; the last two have 95-100% peak core load but 10-11% total CPU.
  The provider/scheduler cause of the delay is not established.
- Bridge and broker both treated acquisition age beyond max(5 seconds, 3 polls) as
  unavailable. `SensorSnapshotCache` separates delay from availability, retains
  every validated reading while the backend is active, and preserves acquisition
  time. Explicit new unreadable samples/errors/exits/disconnects/reset still clear it.
- New deterministic native tests cover delay, invalidation/recovery, stopped fans,
  service serialization and production overlay formatting. Restoring the old age
  expiry deliberately fails three regressions; restoring the fix passes. Metered
  state diagnostics expose delay/readability/recovery without value/sequence noise.
- Unfiltered gate `20261013_165323_build_7136` passed in 337.948 s: full native
  suite, 41 Python tool groups, all 64 WARP FG flows, PE/PDB/privacy checks and setup.
  Final focused gate `20261013_165925_build_7137` passed in 104.481 s after new-file
  formatting and explicit clock-row assertions: sensor/host/protocol regressions,
  x64/x86 PE/PDB/privacy checks and fresh setup 0.1.7137 (39,806,998 bytes).
  Installed-game retest remains pending. Sources/invariants:
  `overlay-rendering.md`, `elevation-and-startup.md` and the new cache/diagnostic tests.

### 2026-10-13 - Close four review gaps in early takeover and FSR screenshots

- Early Streamline override version reads now use an explicit direct-PE-only policy, including missing/unreadable
  resources. Worker queries keep the loader fallback. Native regressions count calls and read a system PE resource.
- FSR screenshots retain generations before submission and preserve them on failed fence signals. No retention
  allocation or GPU wait follows ECL; unknown completion survives until device removal and loader-lock destruction.
- The production composite shader follows registered straight/premultiplied UI alpha. The completed scene/HUD is
  post-processed in the same borrowed list before optional CE overlay drawing, independently of inclusion.
- WARP pixel readback exercises the production shader, gamma/CAS/RCAS, missing/transparent UI and untouched overlay
  pixels. Separate tests cover signal failures and disabled/HDR passthrough. All four deliberate defect restorations
  failed their targeted regressions; restoring the fixes passed the focused suite. Product syntax checks pass.
- Closing gate `20261013_004230_build_7134` passed: unfiltered native suite, 41 Python tool groups, all 64 WARP
  FG flows, x64/x86 hook verification, PE/privacy checks and setup 0.1.7134 (39,793,054 bytes). Live game/HDR
  comparison remains unverified; no new presentation waits or per-frame copying. Sources: `screenshots.md` and
  `startup-runtime-overrides.md`.

### 2026-10-13 - Isolate display-timing delivery from other ETW writer failures

- The HOTS/DXUP latency feed was already empty before the RTSS compatibility change.
  Trace startup succeeds and the service binary matches the product, but meaningful
  present/flip/blank/input counters remain zero.
- Hardware baseline/independent/baseline probes isolate the cause: default mode sees
  only MPO-ID records; one independent-session flag restores native DXGI Presents,
  submission and display-completion events; reverting only that flag loses them again.
- Both local and privileged-service sessions now use independent mode through the
  existing properties helper. Filters, privileges, render/capture/pacing code are unchanged.
  Startup logs query and name the actual mode. The new native property regression fails
  before the change and passes after; the committed bounded diagnostic reproduces the
  real Windows behavior without changing another collector.
- Closing gate `20261013_000021_build_7133` passed: unfiltered native suite, 41 Python
  groups, all 64 WARP FG flows, PE/privacy checks and setup 0.1.7133 (39,778,532 bytes).
  Native display-timing regressions, product syntax and file-scoped Python diagnostics pass.
  Installed HOTS/DXUP session `20261013_000827`: user confirms latency is visible again;
  independent mode is queried successfully, display events recover, and overlay estimates
  settle at 20.8-21.0 ms around 138 fps. All 2408 display samples are associated; early
  no-timestamp warnings precede the first game Present. Frame start/input wait remain modelled.
  Evidence and commands: [display-change timing](../display-change-timing.md).
