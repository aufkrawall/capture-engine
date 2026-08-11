# llm-wiki Log

### 2026-08-11 - Fix false FSR_FG ECL-pattern latch on late inject (Strange Brigade DX12)

- Session `installed/captureengine/logs/20260811_211623` (build 0.1.5917):
  late-injected Strange Brigade DX12 (no DLSS FG, no FSR FG, no Streamline)
  rendered the overlay for only a few frames, then
  `DX12: FG detected via ECL count pattern (real=5, interp=12)` latched
  heuristic `FSR_FG` with `scQueue=null` and every later ProcessFrame hit
  `ProcessFrame — FSR FG active but scQueue=null, SKIPPING overlay`.
- Root cause: the ECL-pattern heuristic counted every zero-ECL present as an
  "interpolated" frame. During late injection the game queue's ECL hook is not
  live yet, so the first ~12 presents before the first counted real frame
  looked like interpolation evidence and tripped the 5-real/10-interp
  threshold on a non-FG game.
- Fix: zero-ECL presents now count as interpolation evidence only after a real
  frame has been observed and only once per real frame (interleaved cadence),
  and a latched heuristic deactivates after 120 consecutive real frames without
  interpolation evidence unless direct FFX API confirmation exists.
- Regression tests: `ECLPatternHeuristicDoesNotCountWarmupZeroECLFramesBeforeFirstReal`,
  `ECLPatternHeuristicRequiresCountThresholdsForDetection`,
  `HeuristicECLPatternDeactivatesAfterSustainedRealOnlyRun` in
  `tests/test_dxgi_shared_part5.cpp`.
- Source anchors: `hook/common/dx12_overlay_policy/fg_metrics_and_transitions.h`,
  `hook/apis/dx12_hook_process.cpp`.

### 2026-08-11 - Guard the Steam external-chain trampoline transport (DLSS->FSR switch crash 20260811_195131)

- Session `logs/20260811_195131` (build 0.1.5914): Talos starts fine with
  DLSS FG active, but DLSS FG -> FSR FG crashes on the fresh FSR swapchain.
- Root cause: CE's inline-hook trampoline was prepended over Steam's `E9`
  entry jump at `dxgi!Present`; such a trampoline re-issues the foreign entry
  jump, so `CallOriginalPresent`'s bare trampoline fast-path re-entered
  `gameoverlayrenderer64` with no NULL-callback VEH recovery, and Steam's lazy
  NULL rendering callback faulted on the new swapchain.
- Fix (build 0.1.5917): `TrampolineChainsToExternalOverlay` /
  `IsSteamExternalChainTrampoline` detect that transport (E9/FF25 entry,
  matching the preserved external hook target or any target outside dxgi.dll);
  Present routes it through `TryInvokeGuardedExternalSteamOverlayPresent`,
  Present1 and the shutdown path use the clean bypass.
- Regression tests: `DXGISharedSteamTrampolineChainTest` in
  `tests/test_dxgi_shared_part13.cpp` plus the source-order guard test in
  `tests/test_dxgi_shared_part11.cpp`.
- Invariant and source anchors: `dx12-overlay-third-party-coexistence.md`,
  "Build 0.1.5917" section.

### 2026-08-11 - Fix locked-read AV on the read-only DXGI class vftable (crash fallout 20260811_192706)

- Session `logs/20260811_192706` (build 0.1.5914): every CE crash dump plus
  the UE minidump crashes identically at
  `RepairVTableHooksIfNeeded::<lambda0>` — `lock cmpxchg` on
  `dxgi!CDXGISwapChain`'s class vftable inside the read-only dxgi image
  (0xC0000005 AV-WRITE).
- Root cause: commit e9fa1341's CAS refactor observed vtable slots with
  `InterlockedCompareExchangePointer(slot, nullptr, nullptr)`. A `lock cmpxchg`
  is a write even when used as a read, so it faults on the read-only page
  between VirtualProtect windows. Same latent pattern in
  `DetachOwnedVTableSlot` and the Steam phase-A vtable[8] save in
  `CallOriginalPresent`.
- Fix: vtable slot observation is a plain volatile read again; atomic CAS
  writes remain inside the existing VirtualProtect regions (foreign-slot
  preservation semantics unchanged).
- Regression tests: `tests/test_dxgi_shared_part13.cpp`
  (`DXGISharedVTableRepairTest`) runs repair and detach against a
  VirtualAlloc'd fake vtable locked to PAGE_READONLY; pre-fix the suite exits
  0xC0000005, post-fix both tests pass.
- Invariant and source anchors: `dx12-overlay-third-party-coexistence.md`,
  "Build 0.1.5914" section.

### 2026-08-11 - Cross-tool hook coexistence plus late-inject/resident-deject lifecycle

- Compatibility scope now explicitly includes ReShade, OptiScaler, Special K,
  RTSS custom hooks and Microsoft Detours, alongside the established Steam,
  Rockstar, EOS, Discord, Overwolf, Streamline, and FFX paths. Module identity is
  refreshed off the Present thread; the render path reads an atomic registry.
- Inline hooks prepend CE to an existing `E9` or x64 `FF 25` entry and preserve
  the exact foreign target as CE's predecessor. The x64 prepend rewrites only
  the five-byte entry through a near relay, preserving a Detours/RTSS
  trampoline's `target+5` continuation. Inline/deep patch writes suspend
  peer threads, reject instruction pointers inside the patch range, revalidate
  expected bytes, and fail closed. Deep-hook installation no longer exposes an
  INT3 transition window.
- Inline, deep, vtable, IAT, DXGI, input, and specialized temporary hook removal
  is ownership-based. CE restores only its live bytes/pointer; if a later tool
  followed or replaced CE, the foreign entry and CE chain storage remain valid.
  Proxy DLLs used by common graphics injectors are excluded from broad IAT scans.
- Startup injection behavior is retained. The startup scan also queues already
  running whitelisted DirectX/OpenGL targets. The globally installed Vulkan
  implicit layer stays dormant until its target-specific activation event.
- Host shutdown now signals a global stopping event. DirectX/OpenGL and Vulkan
  runtimes enter dormant pass-through, quiesce host-owned capture resources,
  acknowledge target-specific dormancy, and remain mapped until game exit.
  Remote `FreeLibrary` and hook self-unload are intentionally absent: wrappers,
  callbacks, foreign saved targets, and in-flight detours can retain CE addresses.
  Vulkan retains minimal forwarding/reactivation metadata and pins its image for
  its process-lifetime watcher.
- A new CaptureEngine generation signals retained per-target reactivation events.
  The resident runtime consumes the old wakeup before validating discovery and
  reconnecting, so a newer signal that arrives during the attempt is not lost.
  IPC publishes the new mapping atomically and retains old generations until
  process exit to protect already-entered detours from mapping use-after-free.
- All graphics entry paths gained dormant pass-through guards and host-disconnect
  resource cleanup. OpenGL context-owned deletion remains deferred to its owner
  context; Vulkan proc-address hooks stay stable across dormant/reactivated state.
- Third-party overlay pixels are captured when their natural draw order precedes
  CE's capture point. Inclusion is deliberately best effort: forcing private
  overlay handlers or GPU-work reordering would compromise coexistence.
- Focused regression gate passed for the DXGI behavior/source policies, overlay
  module detection, IAT filtering, lifecycle event/source contracts, NVIDIA LOD
  routing, and DLSS indicator pass-through suites.
  Full verification is pending.
