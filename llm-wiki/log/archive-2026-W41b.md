# llm-wiki Log Archive: 2026-W41b

### 2026-10-06 - DX12 queue dispatch ownership and startup transport separation

- Extracted exact-vtable ECL original binding, atomic publication and cache invalidation behind
  dx12_queue_dispatch and execute_dispatch_registry.h. Unknown queues resolve their own live ECL slot,
  never another queue's predecessor.
- Two-implementation queue flow scenario (test_flow_queue_dispatch.cpp) and native registry tests
  (test_execute_dispatch_registry.cpp) verify distinct COM queue entry resolution, reset recovery, and
  rejection of missing queue identities.
- Detected three production mutations (cross-queue fallback, stale reset cache, resurrection after
  retirement) with tools/refactor/check_ecl_dispatch_mutations.py.
- Separated startup transport bypass from overlay rendering in DXGI shared present
  (dxgi_shared_present_core.cpp, dxgi_shared_present1.cpp), restoring output accounting and coverage
  during third-party overlay (RTSS) presence.
- Closing gate 20261006_193912_build_7009 passed native units (48.7 s), Python self-tests, all 21 FG
  scenarios (41.5 s), x64/x86 products and installer (38,617,156-byte PE). No test processes lingered.

### 2026-10-06 - NGX publication boundary preserves accepted OFF during creation

- The D1 fake reproduced all FG feature IDs 9/11/18 republishing 3x after accepted Streamline OFF;
  the next 32 outputs were drawn twice. Digest-first inspection confirmed creation reactivated the
  compatibility flag; accepted settings/runtime/publication had disagreed. No real-game claim.
- Private ngx_fg_observation operations now own creation/evaluation gating, defaults, one latched-factor
  read and all NGX compatibility/shared publication. Callers supply successful observations; SDK calls
  still execute. No locks, waits, copies or COM reference operations were added.
- Four real-hook cases and five deliberate production mutants pass, including 3x/4x factors, legacy
  latch, MFG default and missing-factor preservation. Removed the unused predicate/its mirror test and
  two setter-spelling assertions; parameter-key/config-enforcement wiring protection remains.
- Closing 0.1.7008 passed x64/x86 products, native/Python and all 19 FG scenarios; package is a
  38,591,604-byte PE. No unit/flow/fuzz processes lingered. Sanitizers/runtime were not rerun for this
  bounded source slice. D1 foreign/queue/cold-start and D7 settings concurrency/generation, lifetime
  and retirement coverage remain open.

### 2026-10-06 - D1 NGX real-hook lifecycle and handover coverage

- Added a minimal nvngx.dll core fake and RAII flow adapter using actual intercepted D3D12 exports
  and parameter slots; production hook logic publishes all observations. Controlled failures, feature
  address reuse and independent teardown balance checks protect creation/evaluation/release evidence.
- Two FlowNGX scenarios pass. Immediate assertions use PID-owned shared FG publication; visible
  overlay metrics are checked after outputs, since their cache need not update inside the SDK call.
- Initial cold-FG variant exposed 22 physical outputs outside the ledger (2432 vs 2410), no debug-layer
  errors. The handover case proves an established native route via one completed output; cold startup
  remains a separate unfixed D1/D3 investigation, documented in frame-generation/ngx-flow-lifecycle.md.
- All three production lifecycle mutants failed the expected scenario; exact source bytes restored and
  both scenarios passed again. Clean 0.1.7006 and final verification 0.1.7007 passed native, all 17 flows,
  Python, x64 ASan/UBSan and lint ratchets; targeted formatting corrections cleared all format advisories.
  The 712 accepted warnings are unchanged. Installer: 38,591,852-byte PE; no test processes lingered.
  Real-game/foreign-overlay/A/V/performance evidence and cold-start accounting remain pending.

### 2026-10-06 - Architecture D0 inventory and queue provenance characterization

- Inventoried all first-party subsystem paths at 3563155d; inventory is separate from reading every
  lifecycle. Six bounded operation questions, source anchors, coupling expressions and pending audit
  areas are in architecture-inventory.md; architecture-debt-plan.md and temp/refactor.md track progress.
- Confirmed two ECL provenance hazards: unknown vtable inherits the first global original; separate
  cache atomics can publish an identity/target mismatch. No current hardware crash is attributed to it.
- No-build baseline refused stale test-link evidence; clean verification 0.1.7005 then passed native,
  15 FG flows, Python, x64 ASan/UBSan, x64/x86 products and lint ratchets (712 accepted warnings).
  Installer: 38,592,800-byte PE; full lint scope refreshed to 988 TUs with unchanged counts.
- Four parser fuzz targets passed a bounded 10-second-per-target run (IPC: 2,683,158 units); no
  unit/flow/fuzz processes lingered. Current game/A/V/hardware validation remains pending.
- D1 NGX/foreign/two-vtable coverage and D2 must characterize actual production transactions.

### 2026-10-06 - PC latency audit: frame-matched anchors, input retrieval, flip-queue diagnostic

- Audit finding: without markers the estimate modelled input-to-Present as one interval - too low for engines
  whose game thread runs ahead (Unreal: whole frames), too high for single-threaded loops that wait. The newest
  Reflex sleep was paired across threads (UE sleeps on the game thread, presents on RHI) and read frames too low;
  without FG CE's own in-Present wait was added on top of the interval already containing it (~9 ms at 90 fps).
- Fixed: PCL marker pairs (frame ID) feed the frame-begin clock, also under FSR FG; sleeps only pair on the
  presenting thread; Win32k RetrieveInputMessage bursts (ABI 69) anchor same-thread loops; frames without a
  boundary use the 2 s learned span. New `[Overlay] PC latency anchors` line with anchor-kind totals and
  `queuedAhead=` (frames displayed while a frame waited after its PresentStart) for the vsync question.
- Not done, deliberately: injecting Reflex markers adds no information (NVAPI would echo CE's own timestamps).
  Pipelined engines without markers stay modelled; the depth is not observable from timestamps.
- Hardware run pending (Talos: no FG, no FG + Reflex, FSR FG, DLSS FG, DLSS MFG). Read the anchors line first.
