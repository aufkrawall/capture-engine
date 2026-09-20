# Vendored AMD FidelityFX GPU headers

Source: `FidelityFX-SDK-v1.1.4.zip`, the same pinned archive
`tools/build/build_fg_sdk.py` already downloads for the FSR frame-generation
backend. The archive's `sdk/LICENSE.txt` is the MIT license and is copied here
verbatim as `LICENSE.txt` (also installed as `tools/licenses/MIT_FidelityFX.txt`).

Only the shader-side algorithm headers are vendored, with their include chain:

| File | Purpose |
| --- | --- |
| `gpu/cas/ffx_cas.h` | Contrast Adaptive Sharpening |
| `gpu/fsr1/ffx_fsr1.h` | FSR1, of which CaptureEngine uses RCAS only |
| `gpu/ffx_core*.h`, `gpu/ffx_common_types.h` | The dialect layer those two need |

`ffx_core.h` selects its own backend from `FFX_CPU` / `FFX_GPU` plus
`FFX_HLSL` / `FFX_GLSL`, so the same two algorithm headers serve the HLSL
shaders, the GLSL shaders, and the C++ constant setup in
`hook/common/sharpen_constants.cpp`.

**Do not take these headers from the newer FidelityFX SDK 2.x drop.** Its
`docs/license.md` is a binary-redistribution-only license that contradicts the
per-file MIT banner still printed inside the same headers. 1.1.4 has no such
ambiguity: one MIT license covering the whole SDK.

Anything under a path containing `external` is excluded from the clang-format,
clang-tidy and file-size gates (`tools/build/build_tests.py`), so these files
are kept byte-identical to the archive and are never reformatted.
