# llm-wiki Log Archive 2026-W34i

Covers 2026-08-22 (Vulkan presentation, injection integrity, and Streamline bridge fixes). Newest-first.

### 2026-08-22 - A duplicate refusal must not replay the duplicate's absolute path

Witcher 3 session `20260822_182415` reached Streamline 2.12.128, bound the native device and
resolved all requested feature functions, but the external fatal-exit dump proved that the game's
`nvngx_dlss.dll` 3.1.1 and the configured 310.7.128 image were both live. DriverStore
`nvngx_dlssg.dll` 310.2.1 and configured 310.7.128 were live too. The retained device was healthy
at the 11.7-second capability probe, then reported `DXGI_ERROR_DEVICE_RESET` before the title's
final object request; the title again terminated with `0xe06d7363`.

- The duplicate guard logged "keeping the loaded copy" but returned an empty redirect. That only
  preserves the resident image when the caller originally requested it; SL2 requested the custom
  absolute path, so replaying the original request mapped the second image anyway. Duplicate
  refusal now returns the resident physical path through every loader front end.
- An eligible 1.x -> 2.x bridge preloads SR/FG from `streamline_dll_path` before the 15-slot import
  takeover and immediately patches the already-resident legacy SL modules' LoadLibrary imports.
  This lets an in-flight late 1.x `slInit` converge on the same NGX images as 2.x.
- The quiesce captures all live SR/FG feature images before calling legacy `slShutdown`. After a
  successful shutdown it releases each captured foreign image once, before SL2 initialization,
  while never draining an opaque loader count that another integration may own. Logs and bridge
  inventories include every physical SR/FG image, not only `GetModuleHandle`'s first answer.
- The duplicate NGX state is the concrete unsafe difference from the preceding run; attributing
  the device reset specifically to it remains an inference until a fresh title run proves the
  single-generation state and reaches the render loop.

### 2026-08-22 - Reuse the proven D3D12 device before a redundant creation can reset it

Witcher 3 session `20260822_174509` reached a healthy CE-owned Streamline 2.12.128 runtime, loaded
the custom-path DLSS, DLSS-G, Reflex and PCL plugins, bound a native D3D12 device, and applied the
configured SR/FG overrides. Before any translated feature call, its later `ID3D12Device1` request
returned `DXGI_ERROR_DEVICE_RESET` for both the LUID-matched and default-adapter attempts; the title
then threw `0xe06d7363`. The retained-device fallback ran too late because the failed driver call
had already reset the object it needed to recover.

- A compatible same-LUID repeated object request now checks device health and queries the requested
  interface before entering D3D12 at all. Different adapters, higher feature requirements,
  unsupported interfaces and unhealthy devices still take the native path.
- Cache identity comes from the created device's actual adapter LUID, including explicit tracking
  for default-adapter creations; the cache always follows the newest successfully created device.
- Streamline handoff deduplication now uses canonical COM identity rather than interface-pointer
  equality, and a failed `slSetD3DDevice` is not recorded as an accepted runtime device.
- A genuinely new device closes the feature-call gate until `slGetFeatureFunction` answers again;
  Reflex set/sleep entry points both remain in the retry-completeness condition.
- The dump and module inventory confirm that the statically imported 1.x interposer is mapped but
  inert; every live Streamline plugin and NGX binary came from the configured 2.x folder.

### 2026-08-22 - Vulkan compute presents need a compute-side overlay composite

DOOM Eternal's async-present mode changed the present topology from graphics family 0 to a
compute-only family 2 and reduced the swapchain from three images to two. CE's CPU and overlay
cost stayed effectively flat (about 8-9 us overlay GPU time), but the direct graphics render pass
inserted a compute -> graphics -> compute dependency round trip that the game does not have without
the overlay.

- On eligible non-graphics compute presents, render the overlay independently into a transparent
  sampled image on CE's reserved graphics queue, then blend only its occupied rectangle into the
  storage-capable swapchain image on the original compute/present queue.
- Gate the formatless compute shader on the application's existing swapchain storage usage and
  SPIR-V read/write-without-format capability (legacy device features, Vulkan 1.3, or the
  format-feature-flags2 extension), plus the exact format's corresponding feature bits when it is
  not core-guaranteed. Do not widen device features or swapchain usage; retain the direct
  render-pass fallback when any capability is absent.
- Keep the independent graphics submit free of game waits and prefer CE's queue, avoiding both the
  cross-engine critical path and serialization with the game's next graphics submission.
- Follow-up A/B/A session `20260822_165450` showed 138.48 / 140.08 / 138.56 FPS for async-on / off / on.
  There were no fallback or fence stalls; the remaining measurable difference was 33-34 us of overlay CPU.
  Cache the executable final-composite command per swapchain image while its occupied rectangle is unchanged,
  and retain compute wait scratch vectors across presents. A once-per-2048-frame CPU summary samples the steady
  record/submit phases every 128 frames and times only actual command-cache misses, so future runtime evidence can
  distinguish submission overhead without materially perturbing the present path.

### 2026-08-22 - Redundant D3D12 recreation can reuse the proven device

Witcher 3 `20260822_021816` successfully created a native D3D12 device, handed it to SL2, and
answered a later null-output probe from that proof. Eleven seconds later an object-producing
request with the same adapter/feature level failed with `DXGI_ERROR_DEVICE_RESET`; the default
retry failed identically. The title threw `0xe06d7363`.

- Keep a COM reference to the last successful native device per adapter LUID.
- On a device-lost-class failure only, satisfy the repeated request by querying that retained
  device for the requested interface after checking its removal reason.
- Normal first requests still create distinct devices; this is a recovery path for drivers that
  reject redundant validation they had just completed.

### 2026-08-22 - Bridged Reflex needs per-frame sleep, not just activation

Witcher 3 `20260822_015042` accepted the synthesized `eLowLatencyWithBoost` options, but SL2
kept every generated frame in
`eDLSSGStatusFailReflexNotDetectedAtRuntime - sl.reflex must be enabled and active`. A native 2.x
title also calls `slReflexSleep` once per frame; a 1.x title has no such export.

- The bridge now resolves `slReflexSleep`, enables it only while translated DLSS-G is on, and
  deduplicates it by game-frame token before common constants and again as an evaluate fallback.
- The statically imported 1.x interposer necessarily remains mapped after takeover. The post-quiesce
  log now states that its loader reference is inert and all live imports reach CE.

### 2026-08-22 - Null-output D3D12 probes need bridge-side support caching

Witcher 3 `20260822_014209` crashed during startup after a successful bridged device creation.
Its later `D3D12CreateDevice` request used the optional null output form, but both the
LUID-matched adapter and null/default retries returned `DXGI_ERROR_DEVICE_RESET`. The game treated
that failed redundant probe as fatal.

- CE now remembers each successful native D3D12 creation by adapter LUID and feature level.
- A later equal-or-lower ID3D12Device capability probe with a null output is answered `S_OK` from
  that proof instead of asking the driver again.

### 2026-08-22 - Bridged DLSS-G requires a synthesized Reflex activation

Witcher 3 `20260822_011315` reached DLSS-G present handling, but SL2 rejected every frame with
`eDLSSGStatusFailReflexNotDetectedAtRuntime - sl.reflex must be enabled and active`. The measured
1.x payload had Reflex mode zero even while the title used NVAPI Reflex separately; forwarding
that zero faithfully told 2.x that Reflex was off.

- While translated DLSS-G is on, the bridge forwards `eLowLatencyWithBoost`; when DLSS-G turns
  off, it restores off. An explicit Reflex call while FG is on is also promoted instead of
  overwriting the required signal.
- Repeat suppression remains process-wide because 2.x `ReflexOptions` has no viewport argument.
- This explains the earlier "options accepted but no FPS gain" state. The next validation must
  show `Achieved 'good' FC feedback state` without the Reflex-not-detected errors.

### 2026-08-22 - Streamline bridge tag lifetime and FG option deduplication

Witcher 3 `20260822_005204` reached the render loop and DLSS-G created its swapchain, but the
first SR evaluate returned `eErrorMissingInputParameter`. The bridge's arbitrary 16-entry tag
queue had already overflowed; evaluate then flushed a mixed/incomplete set. SL2 also logged a
Present race for every repeated `slDLSSGSetOptions`, while 1.x drives feature constants every
frame.

- `slSetTag` now translates immediately to deprecated 2.x `slSetTag` with a null command buffer;
  every translated tag is `eValidUntilPresent`, which the 2.x API explicitly permits without a
  command buffer. No cross-frame queue exists.
- Cache DLSS-G mode/generated-frame count per viewport and forward only state changes. Unchanged
  calls return success without provoking SL2's documented SetOptions/Present race.

### 2026-08-22 - Streamline bridge default retry must cover null-output capability probes

Witcher 3 `20260822_003944` still ended at the same adapter-bound reset, but the expected
`null/default retry` diagnostic was absent. The fallback was incorrectly gated on `ppDevice`
being non-null; `D3D12CreateDevice` allows a null output as a support probe, and this title uses
that form on its later request.

- Run the device-lost-class null/default retry for both object-creating calls and null-output
  capability probes.
- Log the actual output-pointer value in retry/failure diagnostics; never dereference an absent
  output just to clear it.

### 2026-08-22 - Fresh Streamline bridge adapters can still be reset; add scoped default retry

Witcher 3 `20260822_003051` created the first bridged D3D12 device successfully and resolved all
feature contexts, but its later `ID3D12Device` request still returned `DXGI_ERROR_DEVICE_RESET`
with feature level 11.0—even after resolving a fresh DXGI adapter by the requested LUID.

- Keep LUID normalization, but on device-lost-class failures only (`REMOVED`, `HUNG`, `RESET`,
  driver internal error), retry once with a null/default adapter.
- Log both attempts, both adapter pointers and the feature level. This keeps multi-GPU selection
  changes visible instead of silently substituting hardware.

### 2026-08-22 - Streamline bridge adapters need a fresh LUID-matched instance

Witcher 3 `20260822_001759` proved that native `D3D12CreateDevice` alone was not enough. The first
bridged device succeeded and feature contexts came up, then the later render-device request failed
again with `DXGI_ERROR_DEVICE_RESET` while reusing the game's adapter object after short-lived
factory/proxy generations had been destroyed.

- Before calling D3D12, read the requested adapter's LUID, create a fresh OS `IDXGIFactory4`, and
  resolve an equivalent adapter by that LUID. This preserves multi-GPU intent while avoiding
  dependence on another module's adapter/factory object lifetime.
- Device failures now include requested vs resolved adapter, feature level and IID.

### 2026-08-22 - Streamline bridge: native device handoff and NGX project identity

Witcher 3 `20260821_234606` isolated two bridge failures. V2's interposer returned
`DXGI_ERROR_DEVICE_RESET` for the game's later real-device request, which made the title throw;
and V2 turned a zero application ID into its production temporary ID, disabling all NGX-backed
features.

- Bridge `D3D12CreateDevice` no longer forwards to the V2 interposer. It creates through
  Microsoft's `d3d12.dll`, unwraps a factory-adapter proxy with `slGetNativeInterface` when
  necessary (balancing that call's AddRef), and explicitly hands each distinct native device to
  V2. Explicit devices supersede one another; queue-derived discovery remains fallback-only.
- Since late activation cannot observe the game's original 1.x `slInit` application ID, CE gives
  V2 a deterministic project identity from the host path plus host version. This avoids both a
  game table and embedding the local path in the identity sent to NVIDIA.
- Regression coverage pins the stable project-ID format and source-level absence of V2-targeted
  device creation. Focused `StreamlineBridgePolicyTest.*` passes; runtime confirmation in
  Witcher 3 is still manual.
