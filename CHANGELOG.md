# Changelog

## v0.1.6142

Changes since the last stable release [v0.1.5299](https://github.com/aufkrawall/capture-engine/releases/tag/v0.1.5299).

### New

- Added `[ThirdParty]` configuration to load ReShade, OptiScaler, and Special K from user-supplied DLL paths
  (`reshade_dll_path`, `optiscaler_dll_path`, `specialk_dll_path`) so the tools work without copying their DLLs into
  each game folder; all three can be active at once and load in the order Special K, ReShade, OptiScaler.
- Added persistent `[UE5]` overrides for injected x64 games: `force_ray_reconstruction`,
  `ray_reconstruction_optimal_settings`, `disable_post_processing_effects`, `tonemapper_sharpen`,
  `internal_fps_limit`, and `internal_anisotropic_filtering`. They redirect validated game-thread/render-thread
  CVar shadows in memory (never Engine.ini or game files), stay authoritative across map, scalability, and config
  reloads, and skip missing CVars from older UE/plugin builds safely. `internal_texture_mip_bias` shifts which
  mip level all 2D textures sample from via `r.MipMapLODBias`; `display_gamma` selects `r.TonemapperGamma`
  with sRGB/Rec709 or a pure power-curve exponent and is guarded so `r.HDR.Display.OutputDevice` is only
  written on SDR devices. Added "internal_texture_mip_bias" and "display_gamma" to the per-app profile example in the default config template.
- Expanded `ray_reconstruction_optimal_settings` into `off|light|medium|full` quality levels without implicitly
  forcing DLSS Ray Reconstruction, and added `custom_cvar_overrides` for typed final-value overrides of every
  supported UE5 CVar. The former `on` spelling remains an alias for `full`.
- Added per-application `[ThirdParty]` override keys (`reshade_dll_path`, `optiscaler_dll_path`,
  `specialk_dll_path`) that take precedence over the global paths.

### Improved

- Made build and verification gates faster by isolating sanitizer Vulkan objects and overlapping packaging with lint.
- Packaged a default `testappconfig.ini` into the test-app archive folders.
- Reduced diagnostic log spam from the injected overlay and media pipeline with rate-limited, trace-level logging.
- Extended the UE5 console-registry scanner with data-pointer redirects so Lumen CVars resolve correctly in
  UE 5.6 and across UE versions; every proven element region is now covered, not just the first.
- Made the UE5 console-registry sweep resumable so large heap scans do not freeze partial results; absence
  conclusions now require a complete enumeration of all committed private RW regions.
- Prevented CE's DLL redirect from duplicating a loaded Streamline runtime instance: the hook-slot retarget
  now refuses to move a live forward pointer to a second mapped image, keeping each runtime's plugin set
  coherent.
- Decided Present-entry ownership from the loaded overlay module rather than a single byte sample, and applied
  the Streamline override all-or-nothing anchored on sl.common.
- Retried the deferred temp-swapchain Present-hook install via the guarded system-DXGI route on every service
  pass so late-injection installs do not stall when the device signal never arrives.
- Made the PostSL keep-alive submit attribute its draw to the enclosing present, preventing a false
  uncovered-present count during FG-toggle transitions.

### Fixed

- Fixed the injected overlay disappearing under FSR FG and DLSS FG, including during the DLSS toggle-ON startup
  window and after warm DLSS-FG resume; the overlay now stays visible across every FG-mode switch
  (off <-> FSR FG <-> DLSS FG) with stable topmost ownership through handoffs.
- Hardened the FG-switching matrix against blanks, lost overlay rendering, and crashes; fatal E_ACCESSDENIED switch
  failures now dump through the external helper process instead of freezing the game in-process for ~36 s.
- Fixed overlay coexistence with Steam, RTSS, and other overlays: CaptureEngine now intercepts Present below the
  foreign overlay chain, classifies the chain owner (Steam vs RTSS), validates foreign hook targets, and invokes
  RTSS's own Present handler directly instead of re-patching its callback slots.
- Fixed crashes and deadlocks around third-party tool loading: ReShade proxy-queue re-entry, the ReShade
  factory-proxy crash in the temp-swapchain install, swapchain wrapper base-reference over-release on game close, and
  the startup loader deadlock when Special K, ReShade, and OptiScaler load together (Special K now loads last, peer
  threads are suspended around tool loads, and loads run on the loader work queue).
- Fixed the pseudo-overlay font and circle scale to track the anchor monitor's DPI instead of the
  DPI-awareness-dependent window DPI, so text no longer resizes when the foreground app's DPI awareness changes.
- Fixed a DX12 startup crash in which the third-party-overlay Present-hook deferral was never made real: the
  deferred temp-swapchain Present hook now waits for the game's own D3D12 device and installs inside the same
  startup window, resolving the intermittent Steam-overlay recursion and the CaptureEngine access violation.
- Fixed FSR heuristic FG: an authoritative `ffxConfigure` OFF now vetoes the heuristic, preventing it from
  composing into AMD's UI resource after frame generation was explicitly disabled.
- Stopped re-attempting console-registry CVars whose layout CaptureEngine cannot drive, avoiding repeated failed
  writes and scan retries.
- Fixed frame timing under native FSR FG by ticking from the FFX present callback and submitting the overlay on the
  queue the FG runtime actually flushes.
- Fixed DLSS FG integration: the overlay no longer uses a dedicated overlay queue for NVIDIA DLSS FG, late-inject
  overlay submits route to the swapchain-owning queue, the FG multiplier is reported from MultiFrameCount, the
  Streamline multiplier stays latched across CreateFeature, and already-loaded DLSS-G/Reflex exports resolve at late
  injection.
- Fixed CodeQL-flagged format-argument and multiplication-overflow defects in overlay and device code.
- Fixed GCC compilation of the FFX hook header by ordering the `template` keyword before `inline`.
