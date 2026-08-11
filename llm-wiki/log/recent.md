# llm-wiki Log

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
