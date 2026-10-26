# llm-wiki Log Archive

### 2026-10-25 - Remaining timestamp warning: reproducible interval-integrator phase bias

- `InputFrameRatePredictor::SmoothMonotonicTimestamp` integrates its interval EMA without phase correction.
  Native probes using the pinned test compiler: steady 11200 <-> 13000 us intervals leave +/-4199 us of
  permanent offset; 7143 <-> 8333 us leave +/-2777 us. This explains a real failure mechanism consistent
  with `20261025_130636` smoother deviation; the raw and predicted times use the same QPC clock.
- An isolated 16-sample phase-fit proposal removes the offset and preserves the tested quantization,
  monotonicity, deviation, real-stall, reset and variable-span thinning contracts. A naive phase/8 correction
  breaks the 140 fps quantization case and was rejected. Details and required follow-up: `wgc-capture.md`.
- No product/analyzer code changed. The proposal is not yet fully regression-tested, gated or hardware-tested.
  Local report and patch are in ignored `build/verification/timestamp_phase_investigation/`; preserve the strict
  raw timing diagnostic and require fresh hardware evidence before claiming the session warning resolved.

### 2026-10-25 - Startup freeze and duplicate converter summary confirmed fixed (20261025_130636)

- 0.1.7328, Talos 4K dxgi_dup 120 fps AV1 10-bit, 35.267 s, GPU ~100%; user sees a smooth opening.
  Startup gathers 325897 us of history, `lowWater=28` versus output 40, no fallback/ingress drops/grid-debt
  holds/playout catch-up. First cadence window: 34 repeats/121 ticks; longest contiguous repeat in the run:
  one tick (8 ms). No excess/policy repeats, overload or mux pressure. Converter release logged once;
  4232 packed unpacks, zero failures. Twenty sources/two AAC tracks: 1692800 samples, all `diff=+0`.
- Post-mux corrected video/audio endpoints differ by 1 us (rounding). No logged live audio loss, drift
  compensation or ingest starvation. Game frames during the first live second: median 11.762 ms,
  maximum 13.301 ms; live maximum 20.448 ms. The startup/prewarm window has no large game-frame hitch.
- One open analyzer flag: raw residual average absolute 5908 us versus predicted 3431 us triggers
  `timestamp_domain_mismatch`; raw-minus-predicted +2557 us average, 5559 us maximum. Cause unverified;
  separate from the fixed startup failure. Details: `wgc-capture.md` startup section. Packed/unpacked
  same-scene performance A/B and native WinRT WGC validation remain pending. No product code changed.

### 2026-10-25 - Packed P010 + precompiled shaders validated; sub-CFR start froze ~0.3 s (20261025_120518)

- Validation (0.1.7327): desktop `20261025_120018` and Talos 4K at 100% GPU, NVENC p5 AV1 10-bit, 205 s
  `20261025_120518`: `P010_PACKED16`, `runtimeCompiled=0` everywhere, `unpackFailures=0`, `excessRepeats=0`,
  `encoderOverload=0x0`, audio `diff=+0`. First ingress conversion 2.4 ms CPU (51.5 ms with runtime compiles). The
  only game-frame bump at start: one 15.6 ms frame (local median 11.4) inside NVENC `avcodec_open2` (83 ms).
- Found there (pre-existing, not packing): the video began with ~0.3 s of one held frame and ~0.45 s of gameplay
  missing. Cause and fix: `wgc-capture.md` "Startup reserve wait below the output rate". Also: the retained
  converter's counts were never reset by `Release()`, so `[RetainedYuv] Ingress converter released` repeated with
  the same numbers (3x in `20261025_120018`); now reset.
- Continuation review: regression checks with the former policy/counter behavior fail nine tests. Extending the
  simulation to exactly 120 fps found another permanent low-water miss (39 newer frames versus 40 required).
  Count-qualified selection now permits one output interval of extra history; the delivery-hitch test prevents
  reaching farther back through a hole to meet the count. Startup trim logs use a session-owned `ChangeGate`.
- Verified in continuation: all 218 focused capture-policy/packed-P010 tests passed. The unfiltered incremental
  closing gate produced 0.1.7328: native suite, all 47 Python tool groups, 182 fresh FG scenarios, PE/PDB checks,
  privacy scan and installer packaging passed. The setup artifact is 44,745,562 bytes. No new hardware recording
  was made; the startup-freeze fix still needs the Talos 4K rerun described in the topic page.
- Not attributed (needs an A/B): game frame time 10.85 -> 11.4 ms when duplication started and -> ~11.9 ms once
  encoding went live (~9% fps at 100% GPU); CE's own measured passes are ~0.2 ms/frame.

### 2026-10-25 - First recording start compiled ~1 s of HLSL; now built by build.py

- User requirement: the first recording after app start must not stutter the game or the video in any mode. Known
  CPU cost on that path: d3dcompiler_47 for every media engine shader, ~1 s per process (overlay patch 0.65 s,
  retained converter 63 ms, encoder conversion 52 ms, camera 23 ms, cursor 7 ms, unpack 6 ms; measured). The
  driver shader cache does not cover it: it caches DXBC -> ISA, not HLSL -> DXBC.
- Fix: build-time bytecode (`build.py.md` "Precompiled media engine shaders"). Not yet measured on hardware: other
  first-start costs (encoder session open, ~1 GB pool allocation, driver ISA translation) and whether any of them
  shows in game frame times; needs a first-recording log per mode.

### 2026-10-25 - Packed P010 retained slots (VRAM, no picture change)

- Goal: cut the 10-bit retained reservoir without changing anything visible or smoothness-relevant. P010 pads each
  10-bit code to 16 bits; packed `R16_UINT` slots (16 bpp) hold the same codes: dxgi_dup/WGC 4K 64 slots 1519 ->
  1012.5 MiB, slot count, delay, admission, selection and NVENC depth unchanged. Design, layout, costs and
  evidence: `wgc-reservoir-vram-design.md` item 11.
- Rejected on the way: a separate pack pass over a full P010 scratch (+~6 GB/s ingress traffic at 150 fps), and a
  runtime packed/planar branch in the overlay patch shader (`PS_PatchChroma` compile 600 -> 970 ms at
  configuration). Lossy options (8-bit retention for 10-bit output, BC4/BC5) were not pursued: visible by design.
- Bit-exactness verified on the RTX 5070 for every SDR/HDR source format and range plus 4K; hardware recording
  (A/B `wgc_retained_packed_p010`) pending.
