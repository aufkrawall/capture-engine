# Post-Processing Sharpen (FidelityFX CAS / RCAS)

Last cross-checked: 2026-09-20 (initial implementation; no hardware run yet)

Primary sources:
- `common/sharpen_policy.h`
- `hook/common/sharpen_constants.{h,cpp}`
- `hook/common/sharpen_request.h`
- `hook/common/sharpen_pass_log.h`
- `hook/common/sharpen_d3d11.{h,cpp}`
- `hook/common/sharpen_d3d12.{h,cpp}`
- `hook/apis/dx11_hook_sharpen.cpp`
- `hook/apis/dx12_hook_sharpen.cpp`
- `hook/vulkan_layer/layer_sharpen.{h,cpp}`
- `hook/vulkan_layer/layer_sharpen_setup.cpp`
- `hook/vulkan_layer/layer_sharpen_state.h`
- `hook/vulkan_layer/vulkan_swapchain_usage_policy.h`
- `hook/shaders/sharpen_{fullscreen,cas,rcas}.hlsl`, `hook/shaders/sharpen_hlsl_common.hlsli`
- `hook/vulkan_layer/shaders/sharpen_{fullscreen.vert,common.glsl,cas.frag,rcas.frag}`
- `tools/compile_sharpen_shaders.py`
- `external/fidelityfx/` (vendored MIT headers, see its README)
- `tests/test_sharpen_policy.cpp`, `tests/test_vulkan_swapchain_usage_policy.cpp`
- `tests/test_config_part3.cpp` (the `[Graphics] sharpen*` cases)

## Summary

CE filters the frame the application is about to present with AMD FidelityFX
CAS or RCAS, on D3D11, D3D12 and Vulkan. The algorithm headers are AMD's own,
vendored from the MIT-licensed FidelityFX SDK 1.1.4 archive the build already
downloads for FSR frame generation, so the CPU constant setup and the shaders
share one implementation of the math.

## The ordering rule

**The filter runs on the frame this Present will put on screen, before inject
capture copies it and before the overlay draws into it.**

Consequences, in the order they matter:

- CE's overlay is never sharpened: its pixels are written after the filter ran.
- The recording and the screen always agree, including with
  `capture_include_overlay=false`, where the capture runs *before* the overlay.
- It runs whether or not the overlay is enabled.

That last point is why each backend resolves its own target instead of
borrowing the overlay's. The target is the buffer the swapchain's current
back-buffer index names - `IDXGISwapChain3::GetCurrentBackBufferIndex` on DXGI,
the acquired image index on Vulkan - which is exactly what the capture path
reads.

**The application's own HUD is part of that frame and is sharpened with it.**
So is any third-party overlay that drew before CE. This is inherent to
filtering a finished frame and is not something a post-present pass can
separate; it is documented in `config.ini.template` rather than worked around.

## Frame generation

Every displayed frame is filtered, generated frames included. Filtering only
some of them would appear as a sharpness pulse at the generation cadence - at
2x, every other frame; at 4x MFG, one in four - which reads far worse than
either uniform choice.

The cost therefore scales with the *displayed* rate, not the rendered rate: at
4x MFG the pass runs four times per rendered frame. **This has not been
measured on hardware yet.** `hook/common/overlay_gpu_timing.cpp` already does
GPU timestamp slots and is the instrument for it; the default (`sharpen=off`)
is deliberately conservative until those numbers exist.

Filtering *before* frame generation instead would cost one pass per rendered
frame, but it was rejected: with FSR FG, CE never sees the application's Present
through AMD's proxy (see `frame-generation/`), so there would be no buffer to
filter, and with DLSS-G it would mean writing into a buffer Streamline owns as
its interpolation input.

## Where it refuses

`ce::sharpen::Decide` is the single gate; every refusal carries a stable reason
string that reaches the session log. Refusals in precedence order:

| Reason | Meaning |
| --- | --- |
| `disabled` | `sharpen=off`. Nothing is allocated and nothing is queried. |
| `ui_resource_route_carries_no_frame` | The FG runtimes' UI-resource route. That texture is a transparent overlay, not a frame. |
| `route_unclassified` | The backend could not name the route. Never guessed at. |
| `unknown_presentation_encoding` | The target's presentation meaning is not one CE recognizes. |
| `target_too_small` | Below 32 px on either axis: a probe chain or a thumbnail, not a frame. |
| `frame_not_readable` | Multisampled, `DENY_SHADER_RESOURCE`, or a Vulkan swapchain without `TRANSFER_SRC`. |
| `target_not_writable` | No usable render target view. |

A present interposer's private output chain (NvPresent64) **is** a frame route:
it carries every displayed frame, real and generated alike.

## Color space

The question is not how the frame is presented but **what the shader loads**.
An `_SRGB` view decodes on load and re-encodes on store, so an 8-bit sRGB
target reaches the kernel as linear light exactly like scRGB FP16 does, and
filtering linear light rings around highlights.

`ValuesReachShaderAsLinear(encoding, viewAppliesSrgbConversion)` answers that,
and `sharpen_color_space=auto` converts to a gamma space for the filter and back
only when it is true. Stored sRGB, plain UNORM and HDR10/PQ are already
perceptual and are filtered directly. `direct` and `gamma` force one of the two
for diagnosis.

The gamma round trip uses a sign-preserving `pow(2.2)` pair, because scRGB is
legally negative outside Rec.709 and a plain `pow` returns NaN there.

## Strength

One user-facing `sharpen_strength` in 0..1 maps to each effect's own native
convention - CAS sharpness rises with the value, RCAS attenuation is in stops
and falls with it. **Neither effect is off at 0**: 0 is the mildest setting each
one supports, and `sharpen=off` is the only switch. The config parser and the
unit tests both pin that, because treating 0 as "absent" would silently turn a
deliberate mildest setting into the 0.5 default.

## Per-backend mechanics

A neighbourhood kernel cannot run in place, so every backend copies the frame
and filters the copy back over it. That is two full-frame round trips of
bandwidth - roughly 0.15-0.25 ms at 4K RGBA8 on a current GPU - and it is the
floor for any post-present sharpener.

- **D3D11** (`sharpen_d3d11.cpp`): `CopyResource` into a cached texture, then a
  fullscreen pixel-shader pass. Full pipeline state is saved and restored around
  the draw, including the geometry shader - a stray one bound by the game would
  eat the generated vertices. The source view uses the render target view's
  format, so both sides apply the same sRGB conversion or neither does.
- **D3D12** (`sharpen_d3d12.cpp`): records its own command list and submits it
  on the queue that rendered the frame, the way `SharedCaptureD3D12::CaptureFrame`
  does. One queue executes in order, so that submission alone orders the filter
  after the game's rendering and before the present - no cross-queue fence, and
  nothing added to CE's own overlay queue. The constants are root constants (12
  DWORDs), so there is no constant buffer to keep alive. Allocators ring over 8
  slots gated on a fence; the source copy does not ring, because the in-order
  queue plus the per-frame barriers make one copy sufficient.
- **Vulkan** (`layer_sharpen.cpp`): copy, then a render pass into the acquired
  image, submitted on the present queue and chained into the present's own wait
  list exactly like the capture and overlay submissions. The signal semaphore is
  indexed by **presentable image**, not by slot: reacquiring an image proves the
  present that waited on its semaphore consumed it, which a fence on CE's own
  submission never does. Command buffers ring over 3 slots.

Nothing waits on the present thread. When every slot is still in flight the
frame is skipped with a rate-limited log rather than stalling the game.

## Vulkan swapchain usage

Reading a presentable image needs `VK_IMAGE_USAGE_TRANSFER_SRC_BIT`, and an
application that never reads its own frames has no reason to request it. CE now
negotiates it at `vkCreateSwapchainKHR` through
`ce::vulkan_swapchain_usage::Decide`: added only when
`VkSurfaceCapabilitiesKHR::supportedUsageFlags` proves the surface allows it,
never when the capabilities cannot be read. Requesting an unsupported usage
fails `vkCreateSwapchainKHR`, and that swapchain is the game's.

This also fixes an older latent assumption: inject capture already copied from
swapchain images without the bit ever being requested.

## Shaders

`tools/compile_sharpen_shaders.py` emits both backends' bytecode and is run by
hand after a shader change, like the overlay's compilers; the generated headers
are committed.

- HLSL to DXBC through `D3DCompile`, shader model 5.0 only. `#include` is
  resolved by textual inlining because the tool passes no `ID3DInclude`.
- GLSL to SPIR-V through `glslangValidator`, then **`spirv-opt -O --strip-debug`**,
  then `spirv-val`. The optimization is not cosmetic: the unoptimized CAS
  fragment module is 61 KB and comes out at about 6 KB, which is a tenth of the
  blob to embed and a tenth of the work the driver does at pipeline creation.

## Configuration

`[Graphics]` in `captureengine/config.ini.template`:

- `sharpen` - `off` (default), `cas`, `rcas`. An unrecognized value is `off`,
  never a different effect.
- `sharpen_strength` - `0.0`-`1.0`, default `0.5`.
- `sharpen_color_space` - `auto` (default), `direct`, `gamma`.

The host parses them once and publishes the resolved enums in
`SharedGraphicsConfig`, so the D3D hooks and the Vulkan layer cannot read the
same setting three ways. That addition grew the struct past its tail padding and
moved `SHARED_MEMORY_VERSION` to 61.

## Open questions / stale-risk

- **No hardware run at all.** Nothing below has been observed on a GPU: image
  quality, the FG cost multiplier, HDR behaviour, or the Vulkan usage
  negotiation against a real surface.
- The DX12 pass is wired into the main route and the PostSL route. The FFX
  present-callback and UI-resource routes refuse by policy; whether a real FSR
  FG session reaches the main route often enough to be filtered at all is
  unverified.
- `sharpen_strength` mapping is AMD's native range on both effects, but no
  side-by-side has been done to check that 0.5 looks comparable between CAS and
  RCAS.
