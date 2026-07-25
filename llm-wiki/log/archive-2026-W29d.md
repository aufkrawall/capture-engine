# llm-wiki Log — Archive 2026-W29d

### 2026-07-14 - Fix healthy-source WGC/DXGI CFR freeze and multi-process app-audio root selection

- **Evidence / root causes:** the supplied desktop session encoded every requested CFR packet but repeated 2,086 of
  2,096 frames while WGC continued accepting roughly 144 fps with no pool, encoder, or mux pressure. Deferred
  first-frame encoder initialization was folded into the permanent content delay, the selected WGC reserve was then
  flushed, and the wall-relative 250 ms stale-debt floor deleted frames before they could age into that deeper delayed
  target. Process-loopback activated successfully but produced only epoch/EOS records because name resolution selected
  an arbitrary Brave child; process-tree loopback from that child excludes its sibling audio-service process.
- **Root fixes / diagnostics:** WGC and DXGI now pre-open their deferred D3D11 device/codec/mux state before frame-zero
  selection, preserve the newer transactional reserve through live handoff, and commit/rebase the immutable selected
  start contract only after the first successful encode. Stale-debt pruning/clamping subtract intentional content
  delay before applying the real excess-debt allowance. Name capture selects the largest same-executable process-tree
  root deterministically. Rate-limited logs expose prewarm/contract generations and lateness, retained reserve and
  active-delay debt drops, process-tree selection evidence, active-with-no-data epochs, and the distinction between
  `app-active-no-data` and `app-never-started`. Inject remains causal/zero-copy and does not inherit the reservoir.
- **Coverage / validation:** focused policy/process-tree tests passed 5/5 after compiling all mediaengine and unit-test
  sources. The required full x64/x86 product build passed at `0.1.4702`; the canonical no-build run passed all 1,426
  native tests across 94 suites plus three Python tool self-tests at metadata `0.1.4703`. Analyzer syntax validation
  and `git diff --check` also passed. No synthetic capture harness, stimulus capture, matrix, or automated soak was run.
- **Source anchors:** `common/capture_pipeline_policy.h`, `captureengine/media_main.cpp`,
  `captureengine/mediaengine_loader.*`, `mediaengine/{mediaengine,video_encoder,app_audio_capture}.*`,
  `mediaengine/process_tree_selection.h`, `tests/test_capture_pipeline_policy.cpp`,
  `tests/test_capture_coordinator_source.cpp`, and `tests/test_process_tree_selection.cpp`.

### 2026-07-14 - Transactional CFR startup and codec-exact audio finalization

- **Root defects / evidence:** the supplied PCM session showed tracks 2 and 3 retaining roughly 160-173 ms of
  startup/backlog delay even though packet-duration metadata looked nominal. Independent bootstrap flags, partial
  video-reservoir capping, activation state crossing route lifetimes, and generic codec finalization could not prove
  exact decoded endpoints under app churn, stalls, long sessions, or codec priming/padding.
- **Root fixes / invariants:** WGC and DXGI duplication now commit one generation-numbered CFR/audio start contract
  only after reset acknowledgement and video prewarm, use a bounded 300 ms timestamped look-ahead reservoir, and
  choose the nearest monotonic source frame for each content slot. Inject remains causal and GPU-native. Audio
  activations are ordered epochs with explicit tails/EOS; expected source absence is silence rather than an underrun;
  process-loopback COM lifetime is bounded by disposable shared-ring workers; and final audio targets come from the
  common CFR lattice with remainder-preserving 64-bit arithmetic.
- **Codec / diagnostic contracts:** fresh per-recording runtime contracts now enforce AAC, ALAC, FLAC, libopus, and
  PCM-specific framing, priming, skip, trailer, layout, and final-block rules. The pinned native AAC encoder uses its
  NMR coder at the slowest quality-search setting, and the bundled FFmpeg build no longer enables fast-math. Completed
  MKV/MKA streams are decoded to canonical float PCM for exact sample/marker verification; FLAC EOF extradata control
  packets are distinguished from invalid durationless media. Rate-limited logs cover startup generations, epochs,
  worker lifecycle, codec contracts, overload/source repeats, and finalization evidence.
- **Coverage / validation:** the final required x64/x86 product build passed at `0.1.4697`; focused touched-area runs
  passed 349 tests, including six production-mux codec boundary cases; and the canonical no-build run passed all 1,422
  native tests plus three Python self-tests at metadata `0.1.4698`. No synthetic capture matrix, stimulus capture, or
  automated real-time soak was run. Real WGC/DXGI/inject, VRR/stall, endpoint-churn, and long-runtime validation remains
  intentionally assigned to the user's manual recordings.
- **Source anchors:** `common/capture_pipeline_policy.h`, `captureengine/media_main.cpp`,
  `mediaengine/{mediaengine,audio_capture,app_audio_capture,audio_encoder,video_encoder,matroska_timing}.*`,
  `mediaengine/process_loopback_*`, `tools/analyze_capture_av.py`, and the matching audio/CFR/mux tests.

### 2026-07-14 - Cursor composition follows captured content time across WGC, DXGI duplication, and inject

- **Superseded composition detail:** timestamp/coordinate/DPI ownership remains current; VP stream 1 and its duplicate cursor cache were replaced by the single-stream RGB precomposition entry at the top of this log.

- **Root defects:** fresh/repeated frames independently queried `GetCursorInfo` inside several encoder branches, so
  reservoir-delayed or cached pixels could be paired with encoder-time pointer state. Inject assumed desktop origin
  `(0,0)`. Cursor caches ignored DPI, direct RGB never DPI-scaled, global `GetSystemMetrics` was not per-monitor-aware,
  and the VP cursor stream inherited ambiguous HDR color semantics. The monochrome extractor also ignored bitmap row
  stride.
- **Fix / invariants:** captureengine now carries one timestamped cursor snapshot with each frame and maintains bounded
  per-backend histories; CFR repeats select the state belonging to the source-content target. DXGI duplication uses its
  QPC-stamped hardware pointer metadata, inject maps the target client rectangle into swap-chain coordinates, and
  embedded duplication cursors suppress composition. Shared signed/outward-rounded geometry drives both GPU paths.
  Cursor caches include monitor-DPI dimensions, prefer native resource sizes, never shrink larger accessibility cursors,
  and only point-scale on a shape/DPI cache miss. VP stream 1 is explicitly SDR/sRGB; direct RGB converts cursor color
  to the HDR destination transfer function. DPI lookup is cached per thread/monitor and no GPU wait/copy was added to
  the game path.
- **Coverage / validation:** unit coverage now includes negative monitor origins, client-to-swap-chain scaling,
  clipping, out-of-order cursor samples, and bounded history eviction. The required full x64/x86 product build passed
  at `0.1.4695`; the canonical no-build run passed all 1,422 native tests plus three Python self-tests at metadata
  `0.1.4696`. Real WGC/DXGI/inject recordings remain required for visual cursor-latency, mixed-DPI, SDR, and HDR proof.
- **Source anchors:** `common/{cursor_capture_state,frame_queue}.h`, `captureengine/{media_main,wgc_capture,dxgi_dup_capture}.*`,
  `mediaengine/{cursor_renderer,cursor_geometry,video_encoder}.*`, and `tests/test_cursor_geometry.cpp`.
