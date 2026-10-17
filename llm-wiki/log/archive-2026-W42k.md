# llm-wiki Log

### 2026-10-17 - BioShock Infinite AF crash: CE's replacements exhausted the device's samplers

- Session `20261017_135632` (0.1.7209, x86, wrapper path, AF 16x): game dialog "Rendering thread
  exception: Fatal error!" ~40 s after the first Present; no CE crash handler fired. Full dump
  (taken while the dialog was up) is the decisive evidence: the main thread's TaskDialog text and
  the game's own sampler code. `0x89c0d0` builds the descriptor; with a mip bias or MinLOD/MaxLOD
  override it calls `ID3D11Device::CreateSamplerState` (vtable +0x5C) directly per call, else it
  uses a desc-keyed cache at `+0x2E8` with a last-use time. On `0x887C0001`
  (`D3D11_ERROR_TOO_MANY_UNIQUE_STATE_OBJECTS`) it calls `0x89bad0(1)` (evicts every cache entry not
  used this frame) and retries once; the retry's null result is dereferenced at `0x89c2c3`, the
  first frame of the game's stack trace. Caller `0x89cd50` is RHISetSamplerState
  (PSSetShaderResources, get sampler, PSSetSamplers); `0x6ead61` passes the fade bias.
- Cause: CE's process list kept one replacement per descriptor until the config changed. Fixed
  by the registry with sentinels (`dx11-forced-af.md`); flow scenarios with 8192 distinct per-draw
  samplers failed at 3965 before on both paths. CE's linear list scans and a by-value config copy
  per game `CreateSamplerState` were on the same render thread; the overlay showed 30-47 fps at
  2-100% GPU around 13:59:00 with `[D3D11 AF COST wrapper]` estMsPerS 82-145. The slow texture
  streaming the user saw after loading a save is attributed to that CPU work, not measured yet.
- WARP findings: the runtime zeroes `BorderColor`/`MaxAnisotropy` it does not use (a border-color
  stamp on replacements read back as zero), and a released state object keeps counting toward
  4096 until the next flush. Missing `Created replacement sampler` lines in the session were
  hook-log ring drops (`#1245`, `#1257` gaps), not another code path.
- Pending on hardware: no crash through a save load, bounded `entries`, streaming pace,
  `[D3D11 SAMPLER CREATES]` rates in BioShock.

### 2026-10-17 - Review fixes: camera lifetime, retained-YUV resize and sampler bias

- Camera history growth held a vector-entry reference across reallocation, then read its timestamp.
  Snapshotting the selected slot fixes the ASan-confirmed use-after-free (four to eleven frames at
  300 ms / 30 fps). A real-renderer WARP test covers selection and lag while the history grows.
- A retained P010 -> BGRA resize at explicit 10-bit (or NV12 -> RGB10 at explicit 8-bit) falsely
  stopped recording. Frame admission now compares effective output depth and updates input precision
  separately. Production-admission tests failed before the fix and pass, including auto/display
  preference and actual colour-contract stops. See `wgc-reservoir-vram-design.md`.
- AF-only changes invalidated the sampler's creation hash, so the full policy offset an already
  offset descriptor (1 -> 2). Creation metadata now retains the application's descriptor; changed
  policies rebase from it, and final descriptor equality covers removing a setting. Both raw/wrapper
  flow regressions failed before the fix; all 13 AF flow scenarios pass. See `dx11-forced-af.md`.
- No new per-frame GPU work; existing camera diagnostics retained, storage transitions and sampler
  policy rebases logged at their boundaries. Live game/camera recording validation remains pending.

### 2026-10-17 - Raw D3D11 AF: per-context slots instead of one process mutex

- Each raw context's AF state sits in a never-freed, recycled slot found through a thread-local
  cache; a step holds only that slot's spin flag. Incarnation re-check after taking the slot
  covers a drop behind the cached lookup. COM releases inside a step are deferred until the
  slot is free (a final release can re-enter the context).
- Found while designing it (pre-existing): the resize-time clear dropped every state whole from
  any thread, so a sampler bind tracked but not yet applied on another thread was lost, and CE
  forgot which replacements were bound. Now views only (the resize blockers) for contexts used
  since the previous clear, whole drop for idle ones (no device kept alive by dead contexts).
  Negative check: with the old whole drop `RawViewDropBetweenTrackingAndBindingKeepsTheBind`
  binds WRAP instead of CLAMP.
- Not measured on hardware yet: compare `[D3D11 AF COST]` bindUs/passUs in HotS with
  `20261016_234830`-era numbers.

### 2026-10-17 - One D3D11 forced-AF engine for the wrapper and the raw hooks

- Inventory of wrapper/raw duplication: the AF state machine (two copies, only the raw one had
  the sampler-keyed memo), CreateSamplerState policy (two copies; wrapper recorded hash 0 when
  inactive), two replacement caches (wrapper: per-sampler private-data variants; raw: (device,
  desc) cache), two cost formats. Raw-only gaps: no ClearState/FinishCommandList/
  SwapDeviceContextState hooks (after an unseen ClearState CE kept the replacement as physical and
  swallowed the game's rebind of the same sampler), no PSGetSamplers translation, a new deferred
  context inherited state at a reused address, config changes applied only on slot changes.
- Now: `hook/d3d11/d3d11_af_engine*` (state steps + templated Reconcile over Access/Host),
  `d3d11_af_metadata.*` (moved from wrappers, neutral names), `dx11_hook_af_raw.cpp` (raw
  access/host, map of unique_ptr states moved out before release), slots in
  `d3d11_context_slots.h` verified by `D3D11ContextSlotsTest` (probe vtable). Flow tests run each
  scenario on both paths (+ClearState rebind, read-back, live 16x->4x->off->16x without rebinds).
- Not shared on purpose: draw evidence for the DX11 present-interposer classifier
  (`NoteApplicationSubmission`) stays wrapper-only: raw draw hooks also fire for CE's own D3D11
  contexts (shared vtables), which would count as application rendering. Open.
- Raw bind steps still take the process mutex + map lookup per call; the wrapper takes none.

### 2026-10-17 - BioShock rerun (0.1.7197): Steam got CE's swapchain proxy; Present1/resize retries; wrapper AF cost

- `20261017_003122`: `DeepHook(x86): Installed at 60731170+8` on attempt 4, Steam then patched the entry
  (`E9 at 60731170 -> 7600078a`), yet no Steam overlay. Steam's `gameoverlay_renderer.txt`: `IWrapDXGIFactory2::
  CreateSwapChain called`, `VTable for swap chain in unexpected module`, `Tracking new swap chain: 73e3ad0` = CE's
  `CWrapDXGISwapChain` (`returning wrapper 073e3ad0`). D3D11 creates below a foreign chain were always wrapped; the
  DX12 identity rule never covered them. Fix: D3D10/11 rule + resize coverage + DX11 RTV release in reconcile detours.
- Present1 (`606cbd08`), ResizeBuffers (`606ac238`), ResizeBuffers1 (`607327b8`) all `unstable-thread-snapshot` at both
  swapchain events with one strict attempt each; only Present had the 4-attempt rule. Shared rule: deep_hook_retry.h.
  `InstallDeepHookPublished` now clears the last quiesce failure first (a pre-patch refusal reported a stale reason).
- `[D3D11 AF COST wrapper]` second window: srv 714k/s, sampler 692k/s, shader 204k/s binds, 58.8k passes/s,
  workUs 0.07, estMsPerS 121.9 (~0.8 ms/frame at ~150 fps); raw line srv=368/s. Raw bind path takes
  `dx11_hook_g_D3D11ContextStateMutex` + map lookup per bind (`UpdateStageSamplers`). Next: one shared AF engine.
