# llm-wiki Log

### 2026-08-15 - UE5 persistent RR-quality and post-processing override bundles

- `[UE5]` is now the canonical home for `force_ray_reconstruction`; `[DLSS]` and `[Graphics]` remain compatibility
  inputs, including legacy section-qualified profile overrides. New live options are
  `ray_reconstruction_optimal_settings`, `disable_post_processing_effects`, and `tonemapper_sharpen=default|0..10`;
  the explicit sharpen value wins over the disable bundle's sharpen=0.
- The existing validated DenoiserMode pointer redirection became a typed multi-CVar scanner. One read-section pass
  finds every requested exact UTF-16 name, one executable pass scores all surviving static CVar objects, and each
  accepted `Ref` points at process-lifetime game/render shadow storage. Config disable, module unload/reload,
  resident-hook dormancy, and shutdown restore safely; no Engine.ini or other game file is written.
- The RR bundle contains the requested 29 DenoiserMode/Lumen/VSM/MegaLights values. The post bundle uses dedicated
  sharpen, film-grain, motion-blur, scene-fringe, and `ShowFlag.*` CVars, including vignette, without lowering
  `r.Tonemapper.Quality`. Arbitrary post-process materials remain enabled because UE exposes no generic semantic
  marker that distinguishes custom sharpening from damage/underwater/accessibility rendering.
- Shared-memory ABI 39 appends the two policy flags and sharpen value. Config/template, profile, exact-policy,
  persistence-source, and mapping-name regression coverage is included.

### 2026-08-15 - Fresh config template: per-profile ThirdParty overrides + wgc_same_device_capture canonical home

- `captureengine/config.ini.template`'s `[Profile.My Game]` example now lists the three per-application
  `ThirdParty.*_dll_path` override keys (file/folder rules as in `[ThirdParty]`; an empty profile value
  inherits the global path and cannot disable a globally configured tool per app). `tests/test_config.cpp`
  template-parity assertions cover the new example lines.
- `wgc_same_device_capture` was sitting under `[Diagnostics]` in the template while the loader reads it from
  `[WGC]` (legacy `[General]`), so the fresh-config key was dead. Moved it to `[WGC]`; parity test now asserts
  it appears inside `[WGC]` and not in the `[Diagnostics]` span.

### 2026-08-14 - Test-app archives ship a default testappconfig.ini in the x64 and x86 folders

- `build/packages/testapps.7z` now contains `testappconfig.ini` both in the `testapps/` root (x64)
  and in `testapps/x86/`, staged fresh from the checked-in default `testapp/testappconfig.ini`
  (4K borderless fullscreen, `gpu_load=120`, `vsync=1`). Staging fails closed when the template is
  missing, and archive verification requires the root config member plus the x86 one whenever x86
  members are staged.
- Packaging regression tests extended: staging covers both-arch and x64-only layouts plus the
  missing-template failure, and an archive-level test lists the config members in the produced 7z.

### 2026-08-14 - CI: CodeQL default setup replaced by manual-only advanced workflows

- GitHub CodeQL default setup (three `Analyze` jobs on every push to `main`, main receives ~20
  pushes/day) was replaced by three manual-only advanced workflows: `codeql-actions.yml`,
  `codeql-python.yml`, `codeql-cpp.yml` — all `workflow_dispatch` only, no push/PR/schedule triggers.
  Rationale: the default-setup `(c-cpp)` job scanned an empty database every push (its Linux autobuild
  cannot build this MSYS2 Windows project), and the other two re-scanned unchanged files on every push.
- Default setup was disabled via the code-scanning API (`state: not-configured`). The `Review main`
  ruleset's required status check was first updated from the dead `CodeQL` context to the new check
  names, then removed when scanning became fully manual (a required check can never pass for a
  manual-only workflow).
- `codeql-cpp.yml` traces the hardening-ci Linux cross-compile (`python build.py --skip-updates` on
  `ubuntu-latest`). Its first validation run exposed a pre-existing GCC incompatibility:
  `inline template <typename T>` in `hook/apis/ffx_hook_internal.h:219/254` (clang tolerates it, GCC
  rejects it); fixed to `template <typename T> inline` in commit `53354b26` (Windows incremental
  build + unit tests + Python self-tests pass). The first validation run then succeeded with real
  coverage (1.2 GB extracted database vs. the empty default-setup database) and uploaded 29 findings
  (`cpp/wrong-type-format-argument`, `cpp/integer-multiplication-cast-to-long`,
  `cpp/redundant-null-check-simple`) for review under Security > Code scanning. codeql-action pinned
  at v4 SHA `988661ebb5e81487b3fb31b2185d2856c0a10679` (repo convention: pin Actions by SHA).

### 2026-08-14 - HARDWARE-CONFIRMED pacing/topmost; FIXED locally: stable translucent-route ownership

- Hardware session `20260814_055632` (0.1.6055) disproved the prior complete claim. Sharing the marker renderer did
  eliminate the delayed *deep-route* backend build, but CSV frame 1605 still has a 24.443 ms displayed-output gap
  while CE `ProcessFrame` took only 136 us. The FFX callback thread built the separate callback fallback adapter
  from `05:56:48.840` to `.858` (PSOs, 32 upload buffers, font/descriptor resources) at the exact 6.027-second flip.
- The required callback fallback adapter is now prewarmed during the initial no-callback activation probe. Its
  immutable backend creation therefore stays inside the FSR transition window instead of the later stable cadence;
  the live app callback only reuses it.
- Hardware session `20260814_061442` (0.1.6056) confirms both requested major outcomes: the six-second steady-state
  pacing spike is gone and CE remains topmost over Steam. It also isolates the remaining translucent-box flicker as
  visible-route ownership, not HDR/blend-state mutation. At `06:15:05.972`, cooldown reached zero, the deep topmost
  proof disappeared, and the callback baseline resumed; the trace's first callback takeover also records the route
  boundary through `[OVERLAY DOUBLE-DRAW]` diagnostics.
- Exact queue pinning and placement before Phase4 were necessary but still insufficient. While cooldown was nonzero,
  Phase3 returned `kSkipOverlayInit`, allowing the later independent route to run. At zero, Phase3 intentionally
  returned `kReturn` for the uninitialized normal runtime-owned backend and preempted it. The independent composite
  now runs after Phase2 and before Phase3, retaining the callback/deep marker handshake and per-Present completion
  flag so every stable output has one topmost visible owner without rebuilding or changing queue cadence.
- Focused FSR topmost/deep-route tests and the complete 0.1.6057 `--verify` gate pass (product, native/Python
  suites, lint ratchets, x64 ASan/UBSan, PE/privacy checks, and packages). Fresh translucency coverage remains.

### 2026-08-14 - PARTIAL: delayed FSR callback handoff reused the marker renderer and pinned its queue

- Hardware session `20260814_053934` (0.1.6054, Steam) disproved the prior completion claim. FSR started in
  no-callback mode with a warm final-ECL marker renderer, then changed to app-callback mode exactly 6.028 seconds
  later. The first deep callback-route Present synchronously created a separate full renderer (PSOs, 16 command/
  upload slots, font and RTV resources), making `ProcessFrame` take 20.4 ms. CSV frame 2425 independently records
  a 24.194 ms displayed-output gap. This is the reproducible CE-attributed spike, not transition noise.
- The app-callback deep route now uses the same exact-swapchain inline-marker renderer as the no-callback route.
  Its separate owner-queue ECL ends in the same GPU completion marker and needs neither another renderer nor an
  extra queue `Signal`. The embedded final-batch route remains distinguishable in diagnostics.
- The same trace lost deep-route submits when the 90-frame transition cooldown ended, allowing the callback/UI
  baseline below Steam to resume. Eligibility still depended on `FrameProcessSession::gameQueue`, which deliberately
  changes with normal-backend routing. The independent compositor now AddRef-pins `dx12_hook_g_SwapchainQueue`
  under its queue mutex and submits on that authoritative presented-swapchain queue across cooldown changes.
- Focused FSR topmost, deep-draw, Present-chain, and real-swapchain-identity tests pass. The complete 0.1.6055
  `--verify` gate passes; fresh Steam/ReShade hardware coverage remains pending.

### 2026-08-14 - PARTIAL: Steam-only early-inject FSR replacement denial and app-callback topmost gate

- Session `20260814_051557` did not contain a CPU memory crash. Its first dump is official FFX replacement creation
  returning `E_ACCESSDENIED`; the second is the switch app's controlled `ExitProcess(0xE000EACC)` after retries.
  CE wrapped the initial native DX12 chain while its HWND was hidden. When the app released that wrapper, Steam still
  left three real-chain references, so the same-HWND FFX replacement was denied before any warm FSR renderer existed.
- The hidden-window create path already skipped hook refresh and authoritative swapchain side effects, but the outer
  wrapped factory inconsistently added a retaining CE proxy. With a tracked overlay loaded it now returns the real
  DX12 identity. The existing complete-deep-view identity rule is also generalized from two overlays to one: Steam
  alone can retain the old chain, and CE already needs the below-chain view for topmost order.
- Successful 0.1.6053 sessions `20260814_051200` and `20260814_051350` proved the no-callback final-batch route stayed
  topmost, but app-callback intervals only used FFX UI composition below Steam. The dedicated deep owner-queue renderer
  was nested behind normal `overlayInit && syncInit`; callback-route changes intentionally invalidate that backend,
  making the topmost route unreachable. It now runs before the normal backend gate while retaining its exact queue,
  callback, teardown, startup, and device-health policy checks.
- Moving the deep route before the normal backend initialization gate was necessary but insufficient: session
  `20260814_053934` exposed the remaining queue/cooldown coupling and delayed-renderer construction described above.

### 2026-08-14 - PARTIAL: recurring FSR route flips reuse warm overlay renderers

- Session `20260814_041840` is steady at ~144 displayed outputs/s between routing edges, but its internal FSR
  callback/no-callback route alternates every six seconds while FSR remains enabled. Repeated no-callback entries
  align with 17.3/16.1 ms ECL stalls and 20.1/48.8 ms displayed-output gaps recorded by CE's own graph/CSV.
- Routing-only cleanup had retired both suspend/topmost renderer maps. The next Present/ECL rebuilt PSOs, 16
  allocator/list slots, 16 VB/IB upload pools, font resources, and RTV heaps synchronously. Both exact-identity
  renderer families now remain warm across route clears; genuine live replacement and FFX teardown still retire.
- Each cached renderer pins the proxy behind its raw map key across the route gap, preventing dangling/ABA reuse.
  Retirement drops that pin before retaining any still-in-flight GPU state, so the cache cannot delay FFX teardown.
  Session `20260814_053934` later proved that the separate app-callback renderer's delayed first use was itself a
  remaining spike; the shared marker-renderer fix is recorded in the newest entry above.
- The session also contained 3,451 stable UI/deep `[OVERLAY LAYER]` alternation lines and 1,789 identical callback
  HDR-source lines in 31 seconds. Semantic sites and HDR source contracts now log only first observation/change.
- Focused `FFXTopmostBatch*`, FSR owner/deep-route, and FG transition/replay tests pass. The complete 0.1.6053
  `--verify` gate also passes: product builds, full native/Python suites, lint ratchets, and x64 ASan/UBSan.
  Fresh hardware pacing validation remains open.

### 2026-08-14 - FIXED locally: stable FSR topmost completion ownership and one displayed-output timing observer

- Follow-up session `20260814_035452` tested 0.1.6051 and disproved the prior final claim. The two ~288-FPS blocks
  align exactly with the app-callback intervals (`16.173-22.138` and `28.152-34.146`): the FFX callback and CE's
  deep Present interception both advanced `PerformanceMetrics` for every displayed output. The paired timestamps
  produce alternating sub-millisecond/normal deltas, matching the user's spiky frame-time graph observation.
- Frame timing now has an independent exact owner. A deep-body Present observer suppresses the callback sample from
  the first output; the callback remains the fallback for the older runtime-owned topology that never re-enters CE
  through DXGI. FG-state publication and callback overlay rendering remain independent of that sampling decision.
- The same session's only steady no-callback ownership bounce occurred at `03:55:04.922`: one newly submitted marker
  was still in flight, so an all-markers-complete query erased already-established activation proof and temporarily
  restored the darker UI-resource baseline. Completion now latches for the current learned ECL-signature generation.
  The inline renderer rotates through any of 16 completed allocator/upload slots; each marker protects only its own
  slot, while a signature change or missed append resets the proof floor and requires a genuinely new probe.
- Focused policy/source-contract tests and the complete `--verify` gate pass on 0.1.6052 (full native suite, Python
  self-tests, lint ratchets, x64 ASan/UBSan, and package/privacy validation). Fresh hardware validation remains open.

### 2026-08-14 - SUPERSEDED: FSR topmost handoffs use marker-only activation before visible ownership

- User validation session `20260814_024908` (0.1.6049, Steam + RTSS) confirms the learned final-ECL-batch route
  keeps CE topmost under no-callback FSR FG. The log also showed two overlay owners per output: the FFX callback and
  the later topmost route each ran at about 144/s. The translucent background was therefore blended twice, while
  both paths advanced `PerformanceMetrics`, yielding about 288 FPS against an actual ~144/s CSV QPC cadence.
- Draw and frame-time ownership were initially transferred together. App-callback FSR keeps its callback baseline until one deep
  topmost submit succeeds, consumes exactly one proof at the next callback, and replenishes it only from the next
  successful deep Present; stopped/refused Presents therefore restore the callback without a timer or stale latch.
  No-callback FSR yields only after the existing inline completion marker proves the final-batch route.
- FG status publication is unchanged and remained correct in the source session (~72.6 base / ~145.2 output).
  Focused policy/source-contract tests cover proof selection, one-shot consumption, and the shared draw/FPS gate;
  the complete `--verify` gate passed on 0.1.6050 (native/Python tests, lint ratchets, x64 ASan/UBSan).
- Follow-up session `20260814_032638` (0.1.6050) visually confirmed topmost order and correct FPS but retained rare
  transition-only translucency flicker. `[OVERLAY DOUBLE-DRAW]` clustered exactly at topmost activation and routing
  edges: the first "proof" submission still rendered visibly over the UI/callback baseline, while callback-routing
  changes left the learned no-callback batch armed until the next Present observation.
- Both topmost families now start with a non-visible submission. The shared renderer records only its completion
  marker/fence when neither clear nor draw is requested—no RTV creation or target transition. No-callback FSR then
  requires marker completion plus successful CE-substitute retirement before proxy prework explicitly grants the
  final batch permission to draw; revocation precedes UI-baseline resumption. App-callback FSR arms its deep route
  with the same marker-only proof, and every callback-routing change synchronously clears both handoff epochs.

### 2026-08-14 - FIXED locally: retained Streamline-hook reconciliation and stateful DX12 hot-path diagnostics

- Clean validation session `20260814_014012` exposed diagnostic volume rather than a functional failure: 1,826
  low-level `already hooked by us` lines were paired with 1,827 Streamline `Failed to inline hook` lines after
  Streamline unload notifications cleared feature-level slots while the identical CE entry detours remained live.
- `InlineHook::TryGetInstalledTrampoline(...)` now validates CE's exact installed bytes with `ReadProcessMemory()`
  and returns the retained callable predecessor only for the same target/detour. Streamline rediscovery uses it to
  restore the original pointer, target slot, and installed flag once. Conflicting detours remain immediate failures;
  identical redundant low-level requests log the first four plus every 300th, and genuine repeated Streamline
  install failures log the first ten plus every 300th.
- Two independent per-frame no-op diagnostics were also reduced without hiding transitions: completed DX12 font
  uploads no longer log `No deferred upload needed`, while FSR proxy owner-queue HDR logs the initial samples, every
  format/color-space/support/HDR change, and a 600-call heartbeat. In the source session these paths produced 1,678
  and 1,197 stable-state lines respectively.
- Regression coverage pins retained-hook reconciliation, safe live-byte validation, failure cadence, removal of the
  font-upload no-op line, and state-change/heartbeat HDR diagnostics.

### 2026-08-14 - FIXED locally: early Steam + RTSS FG switch crashed in proactive Streamline lookup and stranded overlay startup

- Session `20260814_012102` (0.1.6045, Steam overlay + RTSS, FG switched before CE overlay initialization) has a
  genuine CE crash dump: the HookThread executed an unmapped address inside the just-unloaded `sl.reflex.dll` via
  `sl_interposer!slGetFeatureFunction -> TryResolveReflexFeatureHooks`, while the app thread was in `slShutdown`.
- Root cause: `ScopedStreamlineFeatureQueryGuard` correctly pinned every loaded `sl.*` module, but its lexical scope
  ended at the proactive-scan `if` block. Its destructor released every pin before the first feature query. Both
  DLSS-G and Reflex guards now live for the complete resolver function, covering every query and pointer validation.
- Independent visibility root cause in the same session: an early official FFX startup swapchain armed the protected
  pre-configure quiesce latch; the app then successfully enabled DLSS-G without ever issuing enabled `ffxConfigure`.
  The abandoned provisional FFX latch kept overlay sync idle forever. A successful explicit Streamline enable now
  retires the staged FFX queue/startup state only when authoritative FSR is not active, before DLSS ON publication.
- Regression coverage pins the guard scope, successful-result wiring, FSR-active exclusion, and full FFX-startup
  cleanup. Focused Streamline/DXGI policy tests pass; full `--verify` passed on 0.1.6046 (native tests, Python tool
  self-tests, clang-tidy/file-size ratchets, x64 ASan/UBSan, product binary verification, and packaging).
