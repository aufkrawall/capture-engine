# llm-wiki Log

### 2026-10-17 - Intermittent FlowFSR debug-layer error 907 (open)

- Gate on main 3e6889d6 (run `20261017_185241_build_7213`):
  `FlowFSR.TogglesWithoutAPresentCallbackKeepTheOverlayOnAlternatingUiResources` failed once (98/99 passed).
  First message at frame 364 (64 frames after FSR FG on, no present callback, game-alternated UI textures):
  id=907 "ExecuteCommandLists: a command list which writes to a swapchain back buffer may only be executed on
  the command queue associated with that buffer"; 274 ERRORs until FG off at frame 900, then Present returned
  0x887A0005. A latched state, not a single race.
- The executing queue was unnamed and created after the real swapchain and its buffers: by creation order the
  fake's interpolation queue (proxy constructor). CE's queues are named (`CE_OverlayQueue`,
  `CE_FFXUiCompositeQueue`), so neither. Its only submission is the frame-generation list the game thread fills
  through `ffxDispatch(FRAMEGENERATION)`. CE's topmost same-batch append gates on the learned presenter thread
  and queue (`ShouldAppendTopmostOverlayToFinalECLBatch`), and in passing runs it learns the present queue on
  the presenter thread (signature STABLE, GRANTED at frame ~305). Root cause not found from code alone.
- Not reproduced in 12 targeted runs under load or one full-suite round; the failing run's logs were overwritten
  by reruns. Diagnostics added so the next occurrence is decisive: named queues, CE's submission state on every
  logged error, failed-scenario logs kept under `<run_dir>/flow_logs/` (`build.py.md`, FG flow tests).

### 2026-10-17 - Per-frame config copies removed from the hook

- Audit of every `GetActiveGraphicsConfig()` caller in `hook/`: per-frame/per-call ones (prerender limit and
  vsync override on every present, DXGI frame-latency pacing, D3D7 overlay route, NGX Set/Get/evaluate hooks,
  Streamline evaluate/SetOptions/viewport state, Remix SetConfigVariable, root-signature static samplers,
  wglSwapIntervalEXT, SetMaximumFrameLatency wrappers) now read `GetActiveGraphicsConfigCached()`. The DX9
  `dx9_hook_g_FrameConfig` per-present copy was never read and is gone. Creation/resize/reset/install/summary
  callers stay by-value. Contract in `graphics-overrides-and-frame-pacing.md` (Configuration contract);
  pinned by `tests/test_graphics_config_hot_paths.cpp` (failed on the old sources). No game run.

### 2026-10-17 - BioShock: AF fix confirmed; Steam overlay lost to a factory body-hook race

- `20261017_174523` (0.1.7210, AF 16x): no crash; `[D3D11 AF REPLACEMENTS]` created 28k replacements in
  40 s with `entries` 120-123 (peak 152) and no failures; `[D3D11 SAMPLER CREATES]` 60k-130k game
  creations per second, 99.8% `tagKept` - the rate the removed per-create config copy ran at.
- Steam overlay absent: at 17:45:29.974 both factory body hooks (CSFH at `648686d0`, CreateSwapChain at
  `64868000`) were refused on `unstable-thread-snapshot` with a single attempt, CE patched vtable[10]/[15],
  and Steam's `gameoverlay_renderer.txt` logged `DXGIFactory*_CreateSwapChain points to another module,
  skipping hooks` for every factory - no swapchain tracked, no Present hook. The real-swapchain return and
  the deep Present hook below Steam worked. Fix: both installs use `InstallDeepHookPublishedWithRetry`;
  `Dx12FactorySlotPolicyTest.FactoryBodyHooksRetryTheTransientThreadRace` failed before. Run pending.

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

### 2026-10-17 - BioShock Infinite (x86): no Steam overlay = no x86 body hook; DX11 per-frame log; wrapper AF line

- `20261017_000225` (x86, SysWOW64 dxgi 10.0.26100.9711, Steam loaded first): `reserving the Present
  entry for third-party overlays (loaded=1)`, then `DeepHook: Only supported on x64` and `falling back
  to the entry prepend`. Steam hooks DXGI only after the game's swapchain and skips an entry that
  already jumps elsewhere, so it never drew. New `InstallDeepHookImplX86`: the body patch undoes the
  frame-pointer prologue (`pop ebp` at +5, else `mov esp,ebp; pop ebp`) and jumps to the detour;
  Present `8B FF 55 8B EC 83 E4 F8 83 EC 38 A1..` resumes at +8. Non-stack prologue work is refused
  (x86 `SetColorSpace1`: `mov ecx,[ebp+8]` at +5) and keeps the entry. x86 reserved span 6 everywhere.
  Unit tests: `DeepHookX86PolicyTest` (byte rules only; no x86 runtime test). Hardware run pending.
- 92% of that hook log (3.1 of 3.35 MB in 85 s) was `DX11: [frame N] pre-render` + `calling
  RenderOverlay`, two lines per frame; now a ChangeGate on device/context/RTV, every 10000 frames.
- BioShock's AF runs through `CWrapD3D11DeviceContext`, not the raw hooks, so `[D3D11 AF COST]` never
  covered it; `[D3D11 AF COST wrapper]` now does (numbers pending a run).

### 2026-10-16 - HOTS/DXUP rerun: AF memo keyed by sampler; per-frame translation recheck fixed

- `20261016_224237` (0.1.7192): `[D3D11 AF COST]` ~68 ms/s (~0.48 ms/frame at 140 fps), 29% slow
  slots: the per-slot choice reset on each of ~570 sampler rebinds per frame. Choices now live in a
  bounded per-context cache keyed by the referenced sampler object; bind-hook time is sampled too.
- 91% of hook_debug.log (162,867 lines) was one VTableHook self-collision warning, once per frame on
  the game thread: DXUP loaded from the game folder, the game's first D3D11 device+swapchain went
  through CE's export hook and got wrapped, and the wrapper's diagnostic attribution patched DXUP's
  swapchain AddRef/Release for the process. The translation owner could not cover the frontend
  Release, so it re-validated and re-patched every Present. Attribution limited to DXGI's class,
  refused patches remembered per slot value, warning metered. Flow: `FlowTranslationLifetime`.

### 2026-10-16 - HOTS/DXUP overhead audit: memoized raw D3D11 AF, non-pixel binds unhooked

- `20261016_194516` (0.1.7186, limiter 140 active, inject recording): CE's present-path work, limiter,
  overlay and capture all run on DXUP's submission thread, which waited ~5.2 ms/frame in the limiter;
  recording hitches (0.42% over 20 ms) carried normal CE present cost (245 us mean). The game-thread
  raw D3D11 AF hooks were the only CE work on the critical thread and had no cost measurement.
- Per dirty pixel slot the old path took ~5 global-lock round trips, 4-8 runtime calls (GetDevice,
  sampler/view GetDesc, GetPrivateData), AddRef/Release pairs, a config string hash and two linear cache
  scans. Now memoized per tracked sampler and replacement-cache epoch; view classes read back once per
  changed view; VS/GS/HS/DS/CS bind hooks removed (no reader). `[D3D11 AF COST]` added. Details and
  invariants: `api-translation-overrides.md`. Live HOTS cost of the new path pending.
