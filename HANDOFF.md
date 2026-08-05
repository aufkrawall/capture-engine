# Hand-off: remaining semantic-unit conversion (<800 lines per file)

Last updated: 2026-08-05, before a context compaction. Everything below is
committed and green at `60995f72` (incremental build + 1928 native unit tests +
Python tool self-tests all pass). Resume directly with **Step 1** below.

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
| `captureengine/media_main_threads.cpp` | 9437 | 8574 (only `EncoderThreadFunc`) + inject/wgc units | 60995f72 |

## Remaining files (all > 800 lines)

### Tier 1 - the two mega-monsters (highest risk, do first)

1. **`captureengine/media_main_threads.cpp` 8574** - only `EncoderThreadFunc`
   (8571 lines). Plan:
   - Parse the ~394 depth-1 locals + 40 lambdas (inventories already taken;
     only 13 `const` locals, none reference/array-typed).
   - Put the class skeleton in a NEW header `captureengine/media_main_encoder_session.h`
     (the ~430 member/method declarations will NOT fit in `media_main_internal.h`,
     which is 663 lines).
   - Convert locals to members (default-init, assignment `x = expr;` stays in
     place at the original position), lambdas to private methods (strip `[&]`,
     keep explicit return types, strip default args in out-of-line definitions).
   - Split into `Run`/`Init`/`Loop`/`Shutdown` + ~9 loop-phase member functions.
     Old line numbers (pre-60995f72 file): Init 866-2135, loop 2136-8734,
     shutdown 8735-9436. **New file offset: old line N -> new line N-863.**
   - Loop-body phases (old 1-based ranges, several need sub-splitting because
     they exceed 800 alone): uniform-CFR selection `if (!config.video.useVFR)`
     3876-5059 (1184!), warmup block 5620-6531 (912!), catchup loop 6750-7310,
     WGC drain 2920-3338, second selection block 5068-5353, logging tail
     8472-8734, etc. Depth-2 locals that span phases must also become members.
2. **`hook/apis/dx12_hook_process.cpp` 5578** - only `ProcessFrame` (5414).
   Same session-class treatment. Part of the bigger `dx12_hook` module (see
   Tier 2); do the module re-split first or together.

### Tier 2 - dx12_hook module (facade-based re-split + decomposition)

- `hook/apis/dx12_hook.cpp` 1824, `dx12_hook_ffx.cpp` 2504,
  `dx12_hook_overlay.cpp` 1169, `dx12_hook_ecl.cpp` 1066,
  `dx12_hook_internal.h` 12183 (includes `PostSLOverlayRender` 2696 inline -
   must be decomposed, not just moved).
- Facade restore: `git restore --source 81a66743^ -- hook/apis/dx12_hook.cpp
  hook/apis/dx12_hook_part_*.inl` (37 parts). Chunk map: `python
  tools/refactor/source_splitter.py map hook/apis/dx12_hook.cpp` (see
  `build/refactor/dx12_hook.map2.txt` for the pre-analysis).
- Design units by theme; the ~100 shared statics must be distributed to units
  (with `statics_in_units` they stay in units, externs go to the header).
- `ProcessFrame` (chunk 755) is one chunk - decompose in the facade text
  BEFORE splitting (like DetourPresent), or after via session-class.

### Tier 3 - video_encoder module

- `mediaengine/video_encoder_encode.cpp` 2511 (`EncodeFrame` 1040),
  `video_encoder_conversion.cpp` 1416, `video_encoder_configure.cpp` 1197,
  `video_encoder_lifecycle.cpp` 878, `video_encoder_options.cpp` 876,
  `video_encoder_internal.h` 1156.
- Facade: parent of a2a7a853 (i.e. a4d3578a) - `git restore --source a4d3578a
  -- mediaengine/video_encoder.cpp mediaengine/video_encoder_part_*.inl` (12 parts).

### Tier 4 - class-heavy internal headers (de-inline classes into units)

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

### Tier 5 - remaining over-800 units (facade re-splits)

`dx11_hook_present.cpp` 809 (parent of a4d3578a), `opengl_sampler_override.cpp`
1126 (8ff0b9cd), `fg_session_state.cpp` 1037 (never split - first-time split
from current tree), `dxgi_swapchain_wrap_present.cpp` 1022 (e7361204),
`custom_overlay_dx12.cpp` 894 (never split), `overlay_adapter_render.cpp` 857
(8ff0b9cd), `app_audio_capture_loop.cpp` 808 (652d85f0).

### Tier 6 - test apps

`testapp/dx12_av_sync_test.cpp` 1159, `dx12_fg_switch_render.cpp` 1033,
`dx12_fsr_fg_test.cpp` 862, `vulkan_fg_switch_renderer.cpp` 1025,
`vulkan_fg_switch_test.cpp` 963, `vulkan_fg_switch_test_internal.h` 973,
`vulkan_fg_switch_fidelityfx.cpp` 837, `vulkan_fg_switch_streamline.cpp` 827,
`tests/test_dxgi_shared_part3.cpp` 807. Multi-source test-app builds are
registered in `tools/build/build_part_011.py`
(`testapp_object_path(cmd, arch, source_index)`); keep one object per source.

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

1. `media_main_threads.cpp` (`EncoderThreadFunc` session class).
2. `dx12_hook` module (incl. `ProcessFrame` + `PostSLOverlayRender`).
3. `video_encoder` module.
4. Class-heavy internal headers (Tier 4).
5. Remaining units (Tier 5), test apps (Tier 6).
6. Final `--verify`, baselines, wiki, one last commit.

Commit after every finished file/module - never leave the tree dirty across a
compaction.
