# Graphics overrides through API translation

Last cross-checked: 2026-10-16 (HOTS/DXUP sessions `20261016_154448` and `20261016_194516`, source
audit, memoized raw-context AF and its WARP flow regression; live-game cost of the new path pending).

## Ownership and CPU cost

An application can call D3D11 while a translator submits D3D12 and presents a native DXGI swapchain.
Its D3D11 state hooks still run. A DX12 label in the overlay/performance CSV therefore does not mean
all override work occurs only in the DX12 Present path.

- `hook/present/translated_swapchain_owner.{h,cpp}` establishes one native output owner from observed
  creation or forwarded private-data evidence. Both detoured and wrapped Present/Present1 delegate
  a proven frontend before frame processing, pacing, overlay, capture or post-processing. This is
  API/object evidence, not a translator filename exception. `tests/flow/test_flow_translation_layers.cpp`
  covers nested proxies, cached decisions and exactly one overlay/capture per physical output.
- `hook/overrides/sampler_override/shader_af_plan.h` builds immutable compact dependencies once from
  shader disassembly. The wrapper retains the shader-owned plan; the raw D3D11 path copies it at
  shader bind and copies only one sampler record during reconciliation. Both traverse only sampled
  textures, preserving the existing implicit/bias/gradient versus explicit-LOD/comparison safety rule.
- `hook/d3d11/dx11_hook_sampler_state.cpp` dirties only safe dependent samplers for SRV slots whose
  pointers actually changed. Shader changes include previously overridden slots so a protected,
  absent or unclassified new shader restores the physical sampler. Missing metadata retains the
  conservative resource-invalidation fallback. No time-based validity assumptions are used.
- `HasD3D11SamplerOverride` in `sampler_override/config_and_state.h` publishes a separate
  `g_D3D11SamplerOverridesActive` gate through `hook_common_graphics_config.cpp`. Unrelated VSync,
  queue-depth, MSAA or DLSS settings cannot trigger sampler replacement work. Logical bindings are
  still tracked on the existing active-graphics path, preserving later reconfiguration inputs.
- `sampler_override/translation_scope.h` marks synchronous D3D11 sampler creation and replacement,
  including wrapper and raw-original forwarding. Nested D3D12 dynamic sampler rewriting delegates
  to that frontend policy, preventing a second bias offset and native AF promotion of a sampler the
  frontend kept linear. The scope is nested, thread-local and ends at the exact forwarding boundary;
  native D3D12 descriptors on other threads or after return retain their normal policy.
- `sampler_override/creation_policy.h` tags D3D11 samplers with the policy already applied at creation.
  Under that same policy, raw bind-time reconciliation changes AF only, preserving mip mapping and
  bias (including offset and SGSSAA bias) already applied once. Untagged or differently configured
  samplers retain the existing fallback. Immutable descriptor/config reload limitations still apply.
- Raw-context reconciliation is memoized (`hook/d3d11/dx11_hook_sampler_memo.cpp`). Per logical
  sampler object, `D3D11SamplerChoiceCache` (per context, 128 entries, 4-way probe, LRU eviction)
  keeps its descriptor class and the physical sampler for each AF verdict. Per pixel SRV slot,
  `srvAFClass` keeps the view's resource class. Invariants: each cache entry holds a reference to its
  sampler, so the address cannot be reused while cached (sampler states own no texture memory);
  choices are valid for one `dx11_hook_g_SamplerCacheEpoch11` (bumped by every replacement-cache
  clear); `srvAFClass` is cleared whenever its slot changes, and a changed view's class is read back
  from the view's own private-data cache under the held state lock (one `GetPrivateData`, as the
  shader plan read at PSSetShader already does); only a never-classified view is classified outside
  the lock. Views are never kept referenced, that would retain textures. A queued rebind updates
  `realSamplers` under the deciding lock before `PSSetSamplers` runs outside it (only the context's
  own thread binds), so a pass takes one lock unless it classifies. Untracked shader, a null sampled view
  or an unresolved verdict take the original classification path, so its refresh-from-context
  fallbacks and capped logs are unchanged. A tracked null pixel shader is trusted (depth passes no
  longer re-query `PSGetShader`); state changed behind CE's hooks (`ClearState`,
  `SwapDeviceContextState`) was already untracked for SRVs/samplers. Runtime calls and `PSSetSamplers`
  never run under the state lock. The sampler-config hash is recomputed only when the shared config
  version changes (`AcquireSamplerPolicy11`). The first version kept one choice per slot and lost it on
  every sampler rebind: in `20261016_224237` 29% of slots reclassified (HOTS rebinds ~570 samplers per
  frame).
- Only pixel-stage SRV/sampler binds are hooked. VS/GS/HS/DS/CS tracking had no reader (forced AF
  replaces pixel samplers only) and cost a detour, the global lock and AddRef/Release per change;
  the deferred-AF bootstrap counts those stages' samplers straight from the context.
- Diagnostics: `[D3D11 AF COST]` (hook thread, 10 s windows with activity): binds/s per kind,
  reconcile passes/s, `slots(fast/slow)`, `viewClasses`, `rebinds`, `evictions`, `passUs` and
  `bindUs` (1-in-32 sampled QPC of the passes and of the three bind hooks' tracking work) and
  `estMsPerS(passes binds)`; divide by fps for ms/frame on the game thread. Counters live in
  `D3D11PerContextState::cost`, updated under the lock the hooks already hold.
  Regression: `tests/flow/test_flow_d3d11_sampler_af.cpp` (real WARP D3D11 through the raw hooks).
- Devices created through CE's `D3D11CreateDevice` wrapper (BioShock Infinite x86, `20261017_000225`)
  take the `CWrapD3D11DeviceContext` path (`hook/wrappers/d3d11_context_forced_af.cpp`), which keeps its
  own per-context state and none of the counters above. Its line is `[D3D11 AF COST wrapper]`
  (`hook/wrappers/d3d11_wrapper_af_cost.cpp`, logged in the same 10 s window): thread-local batches
  flushed every 1024 events, 1-in-32 sampled `workUs`, `estMsPerS`. No measurement from a game yet.

## Audit of available override families

| Family | Behavior on D3D11-to-D3D12 translation | Overhead / limits |
| --- | --- | --- |
| AF level and safe/aggressive policy | D3D11 shader/resource-aware reconciliation; synchronous native descriptors delegate | CPU work on state changes plus the selected GPU texture filtering; protected classes retained |
| Mip mapping, bias/mode/min/max/clamp | Creation-time descriptor policy; provenance prevents repeating that policy at bind | Non-idempotent offsets need single ownership at each boundary; existing samplers need re-creation for reliable config changes |
| MSAA and SGSSAA-related bias | Resource/swapchain creation and sampler-bias policies | Changed sample counts can add substantial GPU work; these do not require AF replacement when no sampler setting is active |
| CPU prerender limit | Proven native output uses the DX12 application-queue fence stream | Intentional queue-depth wait; no second D3D11 query stream on the delegated frontend |
| Frame latency and backbuffer count | Native DXGI presentation policy; CE-added waitable-object pacing only | Setter work cached by the existing policy; waits claim the presentation scope once and preserve application-owned waitables |
| General/capture-synced FPS limiter, basic/FG/Reflex/auto | Native output owns cadence; native device publication supports the driver backend | Intended wait/spin/driver sleep when enabled; inactive mode has no pacing wait |
| VSync off/fifo/adaptive/mailbox | Native Present parameters / existing mailbox policy | Vblank or mailbox behavior can change throughput/latency intentionally; no repeated frontend processing |
| CAS/RCAS, SDR gamma/source curve | One native DX12 post-process route before the overlay | Real full-frame GPU pass/source copy and command-recording work, resolution dependent |
| NVIDIA LOD-spread fix | GL/Vulkan driver path | No D3D11/D3D12 sampler or draw rewrite from this option |
| UE5 CVars: RR, post effects, sharpen, internal FPS/AF/mip bias, gamma, DoF, DLSS SR/quality, HDR and custom values | Requires validated UE engine CVars | Configured discovery can consume background CPU even in an unsupported process; no proven UE CVar in this HOTS session |
| DLSS SR/RR presets, exposure, sharpening, FG factor/mode/count/target/preset and indicator | Matching NGX/Streamline/NvAPI/registry contract only | No such feature is added to a translator by these settings; no active FG in this session |
| DLSS/Streamline DLL paths, upgrade, NGX OTA/log | Loader/startup/runtime integration policies | May change loaded code or diagnostic volume; not a repeated D3D11 sampler policy |
| Third-party DLL injection and window heartbeat | Separate module-loading / opt-in background-window mechanisms | Loaded projects have their own cost; heartbeat is not part of the render override pipeline |

Source anchors: `hook/d3d11/dx11_hook_prerender.cpp`, `hook/d3d12/dx12_hook_prerender.cpp`,
`hook/d3d12/dx12_hook_process_session_stage1_prepare_frame.cpp`, `hook/present/dxgi_shared_present_pacing.cpp`,
`hook/pacing/fps_limiter_detail/apply.h`, `hook/wrappers/dxgi_swapchain_wrap_present.cpp`,
`hook/runtime/main_hookthread.cpp`, `hook/runtime/main_ue5*`, `hook/sharpen/`, `hook/ngx/`,
and `hook/streamline/`. Settings are enumerated in `common/config/config.h` and the config template.

## Evidence and verification boundaries

The supplied CE 0.1.7179 session records DXUP frontend DLLs, D3D11 runtime sampler reconciliation,
and a proven native DXGI output owner. AF was 16x/safe, mip controls default. The native sampler
summary at 15:45:41 observed 22 dynamic samplers and modified one; those creation counts are not
measurements of the D3D11 state-change cost over the game. RCAS plus gamma ran at 3840x2160.

The FPS limiter explicitly logged inactive. All 243,376 CSV rows have zero recorded prerender and
FPS-limiter waits. Present stage cost averaged about 356 us of CE-owned wall time per call across
logged windows. It excludes D3D11 state work before Present, and is not exclusively CPU execution
time or an AF-specific cost. No AF-off control run exists in this session, so it establishes neither
an AF frame-rate penalty nor a speedup from these changes.

Regression anchors: `tests/test_d3d11_shader_af_plan.cpp` exhaustively compares all sample-kind
combinations against the existing classifier, covers all 128 texture slots, precise dependency
invalidation, old replacement restoration, unrelated-setting gating, nested/thread-isolated
translation ownership and real WARP sampler provenance. `tests/flow/test_flow_sampler_translation.cpp`
calls the hook DLL's production dynamic-sampler callback with native and translated inputs.

A local bounded probe using the installed DXUP DLLs exercised a real textured D3D11 draw through the
raw hooks and native output. Eight scenarios (default, AF16, AF16 plus offset bias, prerender 0/1,
frame latency 1, backbuffer count 3 and FIFO VSync) each completed 12 sampler assertions and 12
healthy Presents, one overlay per output, a successful screenshot and clean process exit. AF
alternated correctly between a three-mip color texture and a protected single-mip texture; the
application bias -0.75 plus offset -0.5 remained -1.25 on both. Native policy delegation logged
on translated AF descriptors; prerender used one native DX12 fence stream and no D3D11 query
stream. Latency setters logged once per configured scenario. This checks functional routing,
not steady-state throughput or latency under a game workload. Active FPS limiting was source/
regression audited, not exercised by this virtual-clock DXUP probe.

One preliminary probe completed all frame checks but exceeded its cleanup stop bound and was
terminated with its process tree. Cleanup-stage instrumentation and the subsequent complete matrix
exited normally. The original cleanup stall was not localized or reproduced; do not represent
those later passes as proof of its root cause. Private probe sources, logs and screenshots stay
under the ignored diagnostics directory, outside Git.

Closing gate: build 0.1.7180 compiled/verified x64 and x86 products, passed the remaining native
suite and all 79 flow scenarios, and produced the setup package. The unfiltered native run found
one unrelated source assertion in the pre-existing cursor-precomposition edits:
`VideoEncoderSourceTest.CursorPrecompositionBacksUpOnlyTouchedRgbRegionOnNormalCaptureSurfaces`.
Only that test was excluded on the successful resumed gate. Python tool self-tests are skipped
for a filtered gate; lint/sanitizers and the optional integration matrix were not requested.
The installer reflects the entire current workspace, including those pre-existing media edits.

The closing evidence explicitly includes test execution after an initial plain resume omitted it;
see [failed-build resume and ordinary test gates](build.py.md#failed-build-resume-and-ordinary-test-gates).

Session `20261016_194516` (0.1.7186, HOTS + DXUP, 4K, general limiter basic 140, FIFO,
prerender 1, backbuffers 2, RCAS + sRGB gamma, AF 16x, inject recording p6 for 17.6 min):
- Thread split: the game's D3D11 calls run on its render thread (`T64C8`), DXUP's submission thread
  (`T2484`) issues ECL and the native Present. CE's pacing, overlay, post-process and capture publish
  all run on `T2484`; the frontend Present is delegated. The limiter waited 5.2 ms median per frame
  there (`fps_limit_wait_us`), so that thread had slack; `[PRESENT STAGE COST]` own ~330 us without
  the limiter (overlay 220-260, startup_routing ~40, context ~31-36, post_present 35-78 recording).
- Pacing (CSV, 146,397 frames): p50 7.07 ms. While recording, frames over 20 ms were 0.42%
  (598/142,972); outside recording 34% (menus/loading near 30 fps, 32-34 ms). During those hitches
  CE's `total_us` averaged 245 us (max 489): the hitches are not CE present-path work. Limiter steady
  state `late=0`; prerender wait 7.6 us mean; capture 54 us mean; PC latency ~14.5 ms; GPU ~48%.
- Not measured before this change: the raw D3D11 AF work on `T64C8` (per bind and draw), the only CE
  work on the game's critical thread.

Session `20261016_224237` (0.1.7192, DXUP now in the game folder): in game ~92k SRV, 80k sampler and
71k shader binds/s, 172k reconcile passes/s at `passUs` 0.40, `estMsPerS` ~68 (about 0.48 ms per frame
at 140 fps), 29% slow slots from per-slot memo loss on sampler rebinds; CE also issued ~186k physical
sampler binds/s. Separately the frontend ownership recheck ran every frame (see
`dx12-queue-dispatch.md`). Both fixed the same day; the next run's line shows the remainder.

Session `20261016_233435` (0.1.7195, 90 s recording): slow slots 0.9% (35k of 3.97M per 10 s),
evictions 0, `passUs` 0.25 (was 0.40), `bindUs` 0.10; total ~70 ms/s at ~182k passes/s and ~250k
binds/s (binds were not in the earlier figure). No VTableHook flood; attribution skipped DXUP's class;
the translation owner decided once. Then: cached view classes read under the deciding lock and
rebinds recorded there (one lock per pass instead of two or three).

Fresh live validation: repeat the same replay/camera and recording settings with AF default and 16x,
then the updated build at 16x. Keep post-processing, VSync and pacing fixed. Compare source frame
times and the main/render-thread CPU profile; check texture quality and protected passes. Exercise
each pacing setting separately and verify one native wait/cadence per physical output.

Deferred descriptor creation on a translator worker is outside the synchronous sampler scope;
do not claim coverage for an unobserved asynchronous frontend/native association. No game-specific
heuristic or process-wide disabling of native sampler policy is used.
