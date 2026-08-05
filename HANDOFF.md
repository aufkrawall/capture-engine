# Hand-off: remaining semantic-unit conversion (<800 lines per file)

Last updated: 2026-08-06. Everything below is committed; the tree is clean at
`10b2a723` (incremental build + native unit tests + Python tool self-tests all
pass). Resume directly with the highest remaining tier below.

## Goal

Every first-party C++ file must be a proper semantic unit of at most 800 lines
(lint-enforced via `tools/file_size_baseline.json`; files that drop under the
ceiling are removed automatically on baseline refresh). Python
(`tools/*_part_*.py` facades) is a separate follow-up and explicitly out of
scope. No `.inl` fragments may return - new code files must be `.cpp`.

## Done (committed this conversion effort)

| File | Was | Now | Commit |
| --- | --- | --- | --- |
| `hook/common/system_metrics.cpp` | 955 | 505 + `system_metrics_gpu.cpp` (464) | af42d025 |
| `hook/common/custom_overlay_gl.cpp` | 951 | 660 + `custom_overlay_gl_render.cpp` (231) + internal header (230) | af42d025 |
| `hook/main.cpp` | 2743 | 12 semantic units; `main_internal.h` 591 | 76ec7086 |
| `hook/common/dxgi_shared` module | internal header 1728 | 11 units, header 779; `DetourPresent` 1242 decomposed | d87e500c |
| `common/config_load.cpp` | 1492 | `ConfigReader` + 8 section loaders, all <350 | 3bcbf8e5 |
| `captureengine/media_main` module | internal header 2401 | header 598; +`media_main_wgc/recording/window/priority.cpp` | d405b0ec |
| `captureengine/media_main_start.cpp` | 1950 | 634 + loop/targets/shutdown units (`MediaProcessSession`) | a9816048 |
| `captureengine/media_main_threads.cpp` | 9437 | 15 semantic units + `media_main_encoder_session.h` (728); `EncoderThreadFunc` -> `MediaEncoderSession` | 1cce877b |
| `hook/apis/dx12_hook_internal.h` | 12183 | 10 `dx12_hook_0_internal_helpers*.cpp` + `dx12_hook_internal_globals.cpp` + `dx12_hook_types.h` (624) + `dx12_hook_types_impl.cpp` + `dx12_hook_postsl_*` units; header -> 184 | 7f29da60 |
| `hook/apis/dx12_hook_*.cpp` (main/overlay/ffx/ecl) | 1823/1915/404 | semantic parts (`_main`, `_overlay`, `_ffx`, `_ecl` + `_2.._4`, shared headers) | 10b2a723 |

## Remaining files (all > 800 lines)

### Tier 1 - video_encoder module

- `mediaengine/video_encoder_encode.cpp` 2511 (`EncodeFrame` 1040),
  `video_encoder_conversion.cpp` 1416, `video_encoder_configure.cpp` 1197,
  `video_encoder_lifecycle.cpp` 878, `video_encoder_options.cpp` 876,
  `video_encoder_internal.h` 1156.
- Facade: parent of a2a7a853 (i.e. a4d3578a) - `git restore --source a4d3578a
  -- mediaengine/video_encoder.cpp mediaengine/video_encoder_part_*.inl` (12 parts).

### Tier 2 - class-heavy internal headers (de-inline classes into units)

`mediaengine_internal.h` 6667 (class `MediaEngine`, ~6579 inline),
`wgc_capture_internal.h` 3924 (`WGCCapture::Impl` ~3285),
`dx9_hook_internal.h` 4631 (`DX9Capture`), `dx11_hook_internal.h` 3989
(`DX11Capture`), `streamline_hook_internal.h` 3868, `ddraw_hook_internal.h`
2770 (`DDrawCapture`), `dx8_hook_internal.h` 1809 (`DX8Capture`),
`ffx_hook_internal.h` 1549, `opengl_hook_internal.h` 1218 (`OpenGLCapture`),
`hook/vulkan_layer/layer_capture_internal.h` 1285.

Approach: keep the class *declaration skeleton* in the header, move each
inline member-function body out to a unit `.cpp` as an out-of-line definition
(declarations keep default args; definitions drop them). This shrinks headers
to declarations only. The splitter does NOT do this automatically - it hoists
whole classes - so this part is manual/semi-scripted.

### Tier 3 - remaining over-800 units (facade re-splits)

`dx11_hook_present.cpp` 809 (parent of a4d3578a), `opengl_sampler_override.cpp`
1126 (8ff0b9cd), `fg_session_state.cpp` 1037 (never split - first-time split
from current tree), `dxgi_swapchain_wrap_present.cpp` 1022 (e7361204),
`custom_overlay_dx12.cpp` 894 (never split), `overlay_adapter_render.cpp` 857
(8ff0b9cd), `app_audio_capture_loop.cpp` 808 (652d85f0).

### Tier 4 - test apps

`testapp/dx12_av_sync_test.cpp` 1159, `dx12_fg_switch_render.cpp` 1033,
`vulkan_fg_switch_renderer.cpp` 1025, `vulkan_fg_switch_test.cpp` 963,
`vulkan_fg_switch_test_internal.h` 973, `vulkan_fg_switch_fidelityfx.cpp` 837,
`vulkan_fg_switch_streamline.cpp` 827, `tests/test_dxgi_shared_part3.cpp` 807.
Multi-source test-app builds are registered in `tools/build/build_part_011.py`
(`testapp_object_path(cmd, arch, source_index)`); keep one object per source.

`dx12_dlss_fg_test.cpp` 787 and `cursor_renderer.cpp` 793 are close to the
ceiling; they are legal today (<=800) and need no work.

## Tooling (tools/refactor/source_splitter.py)

Subcommands: `reassemble <facade>`, `map <facade>` (chunk table), `split
<facade> <grouping.json>`. Grouping schema (`build/refactor/*.grouping.json`):
`module`, `header`, `units` (`chunks: [..]` and/or `"rest": true`), `delete`
(the `.inl` names), `facade`, plus options:

- `statics_in_units: true` - shared file-scope statics stay in their units
  (definitions) with prototypes/externs in the header instead of `inline`
  bodies. Strips `static` AND `inline` from definitions/prototypes (an
  `inline` definition unused in its own TU emits no symbol -> link errors).
- `unscope_anon: [regionIds]` - promote whole anonymous-namespace regions to
  file scope; preserves enclosing named-namespace components.
- `unstatic: [idx]`, `keep_static: [idx]` - un-static one chunk / keep one
  static (for unit-local statics whose names collide across modules, e.g.
  `EnumWindowsCallback`).
- `destatic`, `hoist_regions`, `classes_in_units`, `extern_in_units`,
  `allow_anon_split`, `keep_in_units`, `define_prefix_rewrites`.
- Renames skip names already prefixed with `<module>_`. Extern decls cut at
  `=`/`{`/`(` after the name (constructor-style globals like
  `FrameQueue g(32)`). const/constexpr shared constants stay inline in the
  header.

Workflow per module:
1. Restore facade + `.inl` parts from the parent of the module's conversion
   commit (conversion commits: main eb927200, dx12_hook 81a66743,
   dx9/dx11/dxgi_shared a4d3578a, media_main/wgc_capture a7e934c4,
   mediaengine/video_encoder a2a7a853, config 1a886ccc (no .inl),
   system_metrics/custom_overlay_gl/nvngx/overlay_adapter/opengl_sampler_override
   8ff0b9cd, audio_encoder/app_audio_capture/injection/pseudo_overlay 652d85f0,
   dx8/ddraw/ffx/opengl/layer_capture/dxgi_swapchain_wrap e7361204,
   streamline c74d4239).
2. `map` the facade, design units (compute sizes with a script; iterate until
   all <800), write the grouping JSON.
3. Delete the generated units + internal header, run `split`, build
   (`python build.py --incremental --skip-updates --concise`), run tests
   (`--no-build --run-tests --skip-updates --concise`), commit per module.
4. **Delete the restored `.inl` parts + facade afterwards** (they are
   untracked; a stale facade in the tree causes duplicate-symbol link errors -
   happened once).

For files that were never split (`fg_session_state.cpp`,
`custom_overlay_dx12.cpp`, `system_metrics.cpp`, `custom_overlay_gl.cpp`):
treat the current file as the facade. For facade text edits (giant-function
decomposition), write the full modified text into `*_part_001.inl` and make the
facade include only that part (see `build/refactor/dxgi_shared_refactored.txt`
as an example of the edited full text).

## Giant-function decomposition recipes (proven)

### DetourPresent style (static helpers + context struct)
Pure-read preamble flags -> `struct PresentCallContext` + a capture function
(side-effect blocks stay in the caller at their original position); extract
contiguous phase blocks verbatim; early returns propagate via the helper's
return value (caller returns it directly, or uses a `bool* earlyReturn`
sentinel; `std::optional` is fine if `<optional>` is already included).

### MediaProcessSession style (class conversion)
Locals -> members (default-init, assignment in place), lambdas -> private
methods, phases `Init`/`Loop`/`Shutdown`; `Init` returns `int` so early
`return 1;` sites still skip `Loop`/`Shutdown`. Class skeleton goes into the
module internal header (or a dedicated `*_session.h` when it is too large).
Default args live only in the class declaration; out-of-line definitions drop
them.

## Pitfalls

- **Source-policy tests** (`tests/test_*.cpp`) read the *logical* source via
  `tests/source_fragment_reader.h` = internal header + `<stem>.cpp` + sorted
  `<stem>_*.cpp` siblings. The header's prototypes now come FIRST, so tests
  that anchor on a signature must target the definition: use
  `rfind("...")` or qualified names like `"MediaProcessSession::foo("`.
  When splitting units, re-check `CaptureCoordinatorSourceTest`,
  `DXGISharedSourceTest` (already updated) and any other source-policy tests.
- Off-by-one line slices: always find function ends with brace matching, not
  map line numbers (closing `}` often sits one line past the chunk end).
- After re-running a split, `build/refactor/*.grouping.json` chunk indices are
  only valid for the exact facade text they were computed on - re-map after
  any facade edit.
- Do not run `tools/refactor/reapply.py` after manual edits - it restores
  `.inl` facades and regenerates, clobbering hand work.
- Keep clang-tidy ratchet at 0 (fix findings, never fold them). Refresh
  `tools/file_size_baseline.json` only after a full product build with
  `python build.py --no-build --lint --update-lint-baseline --skip-updates
  --concise`; shrinking files fold out automatically.
- Wiki (`llm-wiki/`) must stay accurate: update `codestyle.md`/`recent.md`
  once the conversion is complete, and note the new splitter options.
- Final gate before calling the task done: `python build.py --verify
  --skip-updates --concise` (clean build + all tests + lint + ASan/UBSan),
  then refresh baselines and commit.

## Resumption order

1. `video_encoder` module (Tier 1).
2. Class-heavy internal headers (Tier 2): mediaengine 6667, dx9 4631,
   dx11 3989, wgc 3924, streamline 3868, ddraw 2770, dx8 1809, ffx 1549,
   layer_capture 1285, opengl 1218.
3. Remaining units (Tier 3), test apps (Tier 4).
4. Final `--verify`, baselines, wiki, one last commit.

Commit after every finished file/module - never leave the tree dirty across a
compaction.
