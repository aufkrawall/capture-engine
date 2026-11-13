# llm-wiki Log Archive

### 2026-11-12 - Disabled Streamline replacement lost its overlay after the first Present

- GTA `20261112_030715` (0.1.7516): a prewarmed replacement drew once, then inactive-DLSS routing rejected
  its unproven queue. The one-shot marker now promotes lifetime- and buffer-bound normal-route admission on a
  successful submit; active FG cancels it and keeps its own PostSL proof. Retained startup callbacks no longer
  count as physical outputs. The real-hook regression covers two replacements, including observed same-address reuse, stale tokens,
  resize and output pixels; it failed before and passes after. Details: [FG guardrails](../frame-generation/guardrails.md#disabled-streamline-replacement-after-fsr-2026-11-12).
- Gate regression (0.1.7517): promotion cleared `streamlineStartupHandoffPending`, the only handoff evidence of
  the safe post-FSR bootstrap predicate, so a GetState-only DLSS-G enable on the admitted chain was suppressed for
  good (published 0/x1). A submitted admission now counts as that evidence; scenario fails before, passes after.
- Hardware retest pending. The saved pacing trace ends before the suspended-FSR intervals; animation timing
  and output freshness during those intervals remain separate, unproven questions.

### 2026-11-12 - GTA FSR FG overlay at the rendered rate: Streamline's DLSS-G proxy presents AMD's swapchain

- `20261112_013802` (0.1.7514, ReShade): crash gone, overlay/graph juddering after DLSS FG -> FSR FG. Every
  owner-queue batch lay inside AMD's Present: `sl.dlss_g`'s copy (relayed by ReShade's queue proxy), ReShade's flush
  and DXGI's own work in the native Present (disassembled with `cdb`). `sl.dlss_g` created the native swapchain and
  issued its Present, so it is a nested presenter; AMD composes into its proxy buffers on another queue.
- Fix: the anchor also accepts the nested presenter's own owner-queue submission (originator module = native-create
  caller, not the runtime); layers below it never anchor. New flow scenarios (fake `sl.dlss_g` nested proxy, with and
  without ReShade, plus the DLSS -> FSR switch) fail with `granted=0` before and pass after. Details:
  [FG guardrails](../frame-generation/guardrails.md#nested-presenter-under-fsr-fg-2026-11-12). Hardware run pending.

### 2026-11-12 - GTA overlay ~640 ms after ReShade's at startup: EOS's warm-up held the game's own swapchain

- `gtadlssfgtofsrfgcrash` start (no FG, EOS loaded, no startup window): first Present 01.152, CE's first draw 01.789 -
  493 ms startup warm-up, 100 ms post-resume settle, staged backend/RTV/sync init. Nothing was at risk: the swapchain
  was the game's, drawn on its own create queue (`fgOwned=0`).
- Fix: the warm-up applies only while a runtime owns the live swapchain (the startup handoffs behind the old
  `ERR_GFX_STATE` cases); a visible startup window still pauses. New flow scenario: 90 uncovered Presents before,
  none after the first. Outside Streamline the first Present of a ReShade chain stays unprocessed (zero counted ECLs);
  GTA (Streamline no-FG) processed it. Rule: `dx12-overlay-third-party-coexistence.md`.
### 2026-11-12 - GTA DLSS FG -> FSR FG crash: CE drew the back buffer from a queue that did not own it

- `gtadlssfgtofsrfgcrash` (0.1.7513, ReShade 6.8): `6a56e025`'s every-queue relay recovery anchored the final-batch
  route on AMD's batch on a second DIRECT queue; the first draw onto the real back buffer removed the device in the same
  Present (`ERR_GFX_STATE`). The freeze dump's thread-local trace proved the queues and sites.
- Fix: anchors only on the presented swapchain's own queue (`fg_presenter::OutputQueue`), re-checked before each
  append. New flow scenarios fail before (id 907, `0x887A0005`) and pass after; all 24 `FlowFSR` scenarios pass.
  GTA + ReShade keeps the UI baseline at the rendered rate; the new per-batch `submitter=` diagnostic is the next
  evidence. Details: [FG guardrails](../frame-generation/guardrails.md#startup-resources-and-relayed-fsr-composition-2026-11-11).
### 2026-11-11 - GTA late overlay and remaining FSR graph cadence: source changes, no build/tests requested

- `gtaoverlaylatefsrgraphstuttery` (0.1.7511) shows a 100 ms resource-prime pause before the first cold overlay.
  Removed the separate prime/settle path; the selected renderer prepares resources with its draw.
- The deeper FSR walk now finds AMD's Present site, but no final-batch anchor (4 batches, 3 relayed). Removed the
  create-queue-only relay search and direct-before-relayed selection; added bounded site/queue diagnostics and
  separate-composition-queue, mixed-batch and frozen-virtual-clock startup regressions. No builds/tests executed,
  per the user; both changes still require validation. The logs do not establish the cause of visible ghosting.
- Evidence, pending nested-Streamline question and retest criteria:
  [FG guardrails](../frame-generation/guardrails.md#startup-resources-and-relayed-fsr-composition-2026-11-11).
