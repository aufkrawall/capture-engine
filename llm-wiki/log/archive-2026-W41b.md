# llm-wiki Log Archive: 2026-W41b

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
