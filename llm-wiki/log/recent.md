# llm-wiki Log

### 2026-10-07 - Queue Signal ownership; separate device trace bootstrap defect

- Signal now uses the private queue registry transaction for pre-patch publication, exact-vtable
  dispatch and cache/reset recovery. Removed its writable map/lock and first-global-original aliases.
- Two production-hook cases check distinct Signal implementations through actual installed slots,
  duplicate install, S_OK/S_FALSE and value preservation, missing receivers, reset recovery, subsequent
  native rendering, COM balance and physical coverage. The original missing receiver resolved to a
  saved non-null predecessor; its regression failed before the unsafe invocation.
- All four deliberate ECL/Signal mutations fail expected assertions; restored production passes.
- Removed unused factory detours/duplicate wrapper IDs and their four compiler warnings. Vulkan and
  ledger source assertions now protect the actual factory paths. A failing pacing fixture still read
  the real cadence clock; explicit ClockSource wiring and a deadline regression reject that defect.
- Enabling CE_DX12_TRACE exposed a separate first-global device trace mismatch before queue assertions.
  Digest/CDB dump inspection with Microsoft and matching CE symbols locates the native
  CDevice::TranslateNodeMask fault below DetourTraceCreateCommandQueue and temp-swapchain bootstrap.
  Device creation trace ownership is the next fix; the end-to-end trace-switch case remains open.
- Final closing 20261007_083619_build_7015 passed x64/x86 products, native units, Python self-tests and
  all 23 FG scenarios. Installer: 38,660,922-byte PE. Sanitizers/runtime/fuzz were not rerun for this slice.

### 2026-10-06 - Talos 20261006_204930 (0.1.7011): latency "changes weirdly" across mode switches

- First run with value-band sample logging. In steady states the marker reading and the estimate cross-check agree
  within 2 ms (DLSS FG 2x 52.8/54.3, no-FG Reflex 22.7/22.7, menu 9.4-10.8 in every mode). Readings that differed
  between visits to the same mode came from different states:
  - **DLSS FG 2x/3x/4x in gameplay:** 53/59/69 ms at bases of 60/46/35 fps.
  - **DLSS MFG 4x in the menu:** sometimes interpolating with the base cut to 34.6 fps (markers 28.9 ms; 69.7 ms),
    sometimes idle at 138 fps (9.4 ms).
  - **After FSR FG off:** Reflex was off and AMD's proxy was still in place, reading 43-46 ms.
  - **Reflex re-enabled with the proxy queue still full:** 61 ms, with the game blocked 4.6 ms per frame in the
    proxy Present. Both paths read 61.0. The proxy block fell to 0.66 ms by 20:52:38, and the reading to 22.7.
- Transients last 1-3 s after each switch (71.7, 77.2, 135.8 with one sample) while the 64-frame marker report mixes
  both cadences. The 14.0 ms DLSS FG samples at 20:50:58 and 20:53:13 (p2d 0.4 ms, anchor 1.5 ms, base interval
  still 16.4 ms) were the move into the menu, where the generator stops. Not changed: no wrong steady state was
  found.
- The user reported two things. First, Reflex read "much too high" after FSR FG was switched off: in gameplay at
  75 fps, 61 ms (20:52:32-41, both paths 61.0) against 32.8 ms before FSR at 70 fps. The marker split was a 34.7 ms
  anchor-to-runtime-Present and 15.6 ms present-to-display, against 10.9/11.0 before. Second, FSR FG read 53-62 ms in
  its first period (modelled anchor; Reflex off) but 79 ms 0.5 s into its second (frameBegin=simulation): Talos left
  Reflex on, 20:52:30-20:53:18, so its markers anchored FSR frames. Whether these are real or mis-paired was not
  decidable from the log, so `anchorToApp=`/`appToRuntime=` were added to the chain line. Hypothesis: Reflex paces
  less effectively through AMD's leftover proxy, and the one-frame modelled anchor understates UE without Reflex.
- Open: the measured FG base read 138.4 at 20:51:21 while the markers said 28.9 ms. It agreed again at 20:51:29.
  Not traced.

### 2026-10-06 - Talos 20261006_203030 (0.1.7010): "Reflex 60 instead of 30" after switching

- Reflex was off from 20:32:29 to 20:34:38: Talos switches it off for FSR FG and leaves it off afterwards. The ~46 ms
  "Latency est." after FSR FG was switched off (refresh-pinned menu, GPU 7 %) is AMD's proxy swapchain, which stays in
  place: 32 ms from present to display, against 18 ms before FSR in the same menu. The game was blocked 5.3 ms per
  frame in the proxy Present. At 75 fps gameplay the readings match before and after the switch. No CE regression
  was found; the 15 s cadence missed whatever the user saw.
- Fixed: the sample is now also logged when the value changes band. `idUnmatched` no longer counts the proxy's
  passthrough outputs while FG is off. Tests: `APassthroughProxyWithoutFrameGenerationOffersNoIdentity` (fails
  without the fix), `LogBandSeparatesAVisibleJumpButNotJitter`.
- First FSR FG run by frame identity: `idQueue=1 idMatched=2029 idUnmatched=6`, at 62.6 ms (median 65.1) with a
  71.7 fps base and 142.4 fps output. That is one application frame behind the newest, not the 3-4 that counting
  produced. `scanout=` was logged. `markerOnPresentingThread=` equalled every paired marker
  (`markerOnOtherThread=0`): Talos brackets its Present.

### 2026-10-06 - PC latency: FSR frame identity, scanout, measured FG rates

- Talos `20261006_194753` (0.1.7009) review: input retrieval now arrives (26255 retrievals); DLSS-G Base/Display
  froze at `66.6/133.2` and `33.3/133.2` (also in `20261006_150600`) because the `g_FGCompat` frame history is not fed
  on the PostSL route; the first DLSS FG 2x period ran at ~52 fps output with VRAM at 11.49/11.94 GB and 139 W at 98 %
  load (likely a VRAM spill, not CE's); Reflex-on 6 ms readings coincided with GPU 4-10 % (menus/light scenes).
- FSR FG 91-142 ms looked extreme: `appQueue=3-4` came from conservation. AMD's side conserved exactly in the trace
  (1309 IDs x one real + one generated, 2613 displays); the game blocked 6 ms per frame in the proxy Present at a
  145 Hz-pinned output, so the queue may be real. Now measured by identity: ffxConfigure frameID -> application
  Present, callback frameID -> runtime Present (`idQueue=`).
- Both latency paths add half the scanout (ABI 70 `refreshPeriodUs`); FG Base/Display is measured; PresentStart-marker
  thread and marker-to-Present span logged to settle whether Talos's Reflex markers bracket its Present.
- Tests: `test_system_latency_generator_identity.cpp` (9), association token, refresh period publication. Hardware
  run pending: read `idQueue=`, `scanout=`, `markerOnPresentingThread=`, `fps=`/`gpu=` on the sample line.

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

### 2026-10-05 - Experimental background window heartbeat

- Added profile/global `WindowHeartbeat.enabled` (off by default), independent of DesktopOverlay and capture.
- The controller owns a separate joined worker. Eligible visible, non-minimized, uncloaked borderless
  fullscreen windows outside the foreground process receive asynchronous WM_NULL at a 250 ms cadence.
- SendMessageCallback completions bound each window to one in-flight message; tokens reject late
  completions after HWND reuse and remain tracked across disable/re-enable. Delivery uses no input,
  focus activation, hooks, injection or game memory access; UIPI failures remain authoritative.
- Deterministic cadence/filter/reload/backpressure tests and a native cross-thread delivery test cover
  the mechanism. Actual game/MPO hang prevention and anti-cheat acceptance remain unverified.
- Source, diagnostic and validation anchors: `window-heartbeat.md`. Profile types now live in
  `common/config/application_profile.h` to keep `config.h` below its size ceiling.

### 2026-10-05 - FG flow attribution distinguishes reused presenter addresses

- The 0.1.6991 closing gate exposed a false GTA-style failure: the second FFX presenter reused its first
  allocation address but began with frame offset 900 rather than 0; all 1200 later outputs were misclassified.
- Fake presenters now supply lifetime IDs; the pure tracker keeps within-lifetime offset checks strict.
  Address-reuse regression fails under the old comparison and passes after the fix; repeated mismatch logs are metered.
- Source anchors and diagnostics: `debug-tools.md` "FG flow runtime-output attribution".

### 2026-10-05 - Remaining architecture debt plan

- Refreshed temp/refactor.md and added the identical canonical architecture-debt-plan.md. Completed
  core ownership stays documented separately; D0-D12 specify remaining queue/route/resources, media/audio,
  SDK, controller, interface, other-backend, build and behavioral-protection work with exit criteria.
- Source-backed coupling/include measurements and full locality growth remain explicit. Uninspected
  areas require an audit; real-game/A/V/hardware and broader ABI/fuzz evidence remain separate.
- Purged 32 obsolete one-off refactor text helpers after literal inventory; reusable tools stay in
  tools/refactor. No product behavior changed, so no new runtime tests or diagnostics were added.

### 2026-10-05 - Template agent and commit privacy integration

- Merged the secret-leak baseline from `llm-prompt-templates` at `1fcfac5` with verified
  Gitleaks 8.30.1 staged, exact-commit, message-file and metadata commands. Every agent commit now
  requires both pre- and post-commit checks, with explicit manual fallback and sensitive-artifact review.
- `AGENTS.md` carries the compressed gate; `secret-leak-prevention.md` owns the complete procedure.
  `debug-tools.md` links it instead of retaining the older push-only guidance; the wiki index routes it.
- No hooks, scanners, audit prompts or system tools installed. Scanner absence does not waive review;
  scanner failures/timeouts and empty history scans cannot certify a non-empty outgoing range.
- Merged tool/version precedence, verification evidence and debugger-state boundaries; corrected stale gate coverage/timing.
  Detailed examples stay in topic pages; build gates and compatibility constraints stay in `AGENTS.md`.
- Commit privacy checks passed on live staged/message/commit/metadata scopes; staged scans report zero commits
  normally, so use their scanned bytes. No runtime tests/logging added for these documentation-only changes.

### 2026-10-05 - Complete core ownership refactor and behavioral DX12 protection

- Controller recording lifecycle, validated IPC transactions, versioned media submission outcomes,
  source adapters and timing commitment are implemented. Final stop policy distinguishes explicit
  rejection from unknown acknowledgement and owns media-first/fallback/release; toggles delegate directly.
- PostSL owns route activation, confirmation epoch, callback admission, queue roles and deferred
  retirement. Queue references span callback submission; cancellation precedes render drain and
  retained GPU completion evidence gates ordinary release. Native frame admission now spans drawing/
  capture instead of releasing the preparation-local lock; retirement drains outside overlay admission.
- Production draw transaction tests detected all six historical-defect mutations. The two redundant
  source recovery checks are retired; independent hooking, patching and security checks stay. Named
  resource/record/submit/metrics/capture operations replace eleven generated wrappers.
- Every local implementation commit passed native units, all then-existing FG scenarios and fresh
  product/package gates. The final harness has 15 isolated real-hook WARP scenarios, including stale
  epoch rejection/native return/reactivation. Real games, capture/A/V, hardware, broader shared-ABI
  verification and fuzz remain pending under the agreed scope.
- The repeated full implementation context grew; policy ownership improved. Exact investigation sets,
  measurements, contracts, commit sequence and user validation are in `refactor-contracts.md`.
  Section 11 independent DLL/events/preview/reconfiguration/plugin work remains deferred.
