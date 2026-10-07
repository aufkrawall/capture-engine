# llm-wiki Log Archive (2026-W41a)

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
