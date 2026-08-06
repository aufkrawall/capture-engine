# Recording Output Paths

Last cross-checked: 2026-07-22 (video staging now uses container extension directly, allowing mid-recording playback)

## Summary

All capture outputs use `ce::capture_output::ReservedCaptureOutput`:

- Normal video recordings reserve at recording start and write through `VideoEncoder` to an unpublished same-directory file with the final container extension (`.mkv`, `.mp4`, etc.); idle media initialization creates no output. The file receives its final collision-safe name after valid mux finalization.
- Audio-only recordings reserve through `MediaEngine::InitAudioOnlyMuxer()` in `mediaengine/mediaengine_impl.cpp`.
- SDR PNG and HDR AVIF screenshots reserve and atomically publish through `captureengine/screenshot_encoding.cpp`.

`[Output] output_dir` may be empty, relative, absolute local, UNC, or a mapped-drive path. `[Output] screenshot_dir` follows the same rules. Empty video, audio-only, and screenshot output writes to the `captures` subfolder next to the executable. Relative output is resolved below the executable directory. All three paths preserve the mapped-drive behavior below. Legacy `[Video] output_dir` and `[Screenshot] screenshot_dir` remain readable.

## Reservation And Publication Invariants

- Filenames contain UTC milliseconds, the writer PID, and an atomic process-local sequence. A collision adds a bounded retry suffix.
- The destination is reserved with `CreateFileW(CREATE_NEW)`. Existing paths are never truncated, removed, or selected as the recording destination.
- The reservation records the Windows volume/file identity. Failure cleanup deletes only a path that still has the reserved identity.
- A video muxer opens only an identity-owned same-directory reservation with the container extension. The container format is selected from configured metadata rather than the staging filename. After trailer and close succeed, publication additionally requires positive encoded duration and at least one successfully written video packet; only then does a collision-safe atomic rename expose the final collision-safe name. Warm-up cancellation, empty output, and finalize failure delete only the owned staging identity.
- An audio-only muxer retains the final-extension reservation model. Its reservation handle remains open without delete sharing for the writer lifetime; successful close/trailer publishes the file, while failure cleanup removes only the owned partial file.
- A screenshot is fully encoded, flushed, and closed in a separately reserved `.part` file. Only then does `MoveFileExW(..., MOVEFILE_WRITE_THROUGH)` atomically give that same file a fresh final-extension name. No zero-byte `.png`/`.avif` placeholder is exposed during encoding, no existing file is replaced, and a destination collision is retried with a bounded suffix.
- Video post-mux duration probing runs only after final atomic publication and uses the published filename. User-visible screenshot notification likewise occurs only after final atomic publication.

## Mapped Drives And Elevation

Windows UAC split tokens can make a mapped drive such as `Z:\Captures` unavailable to an elevated CE process even when the same user can access it from non-elevated Explorer. CE now handles this by rewriting drive-letter absolute mapped-drive paths to UNC before creating the output directory or opening the muxer file.

Resolution order:

1. `WNetGetConnectionW("Z:")` via dynamically loaded `mpr.dll` for a live mapping visible to the current token.
2. `HKCU\Network\Z\RemotePath` as a fallback for persistent mappings, which is the important elevated-process case when the live split-token mapping is not visible.

Only drive-absolute paths (`Z:\...` or `Z:/...`) are candidates. Existing UNC paths, local paths without a network mapping, and relative paths are unchanged. When a mapped output directory is rewritten, CE logs the original path, resolved UNC path, mapping source (`live_mapping` or `registry_mapping`), drive letter, and both lookup status codes.

Limits:

- A temporary/non-persistent mapping that is invisible to the elevated token and has no `HKCU\Network\<drive>\RemotePath` entry cannot be reconstructed; use a UNC path directly in that case.
- UNC access still depends on Windows credentials and share permissions. This fix removes the drive-letter visibility problem; it does not bypass network authentication.

## Source Anchors

- `common/path_utils.{h,cpp}` (`ResolveMappedDrivePath`, `ReplaceDriveRootWithRemotePath`)
- `common/reserved_capture_output.{h,cpp}` (`ResolveCaptureDirectory`, `ReservedCaptureOutput`)
- `mediaengine/video_encoder.cpp` (`ReserveOutputStagingFile`, content-gated muxer publication and cancellation cleanup)
- `mediaengine/mux_invariants.h` (`SelectVideoOutputDisposition`)
- `mediaengine/mediaengine_impl*.cpp` (`InitAudioOnlyMuxer`, audio-only ownership and publication)
- `captureengine/screenshot_encoding.cpp` (reserved staging and atomic final commit)
- `tests/test_path_utils.cpp`, `tests/test_reserved_capture_output.cpp`, `tests/test_mux_invariants.cpp`, and `tests/test_recording_start_feedback.cpp`

## Validation

- `ReservedCaptureOutputTest` covers forced identical clock/PID/sequence values for video, audio-only, PNG, and AVIF extensions. It also proves staging-to-new-name publication exposes no final placeholder and retries a collision while preserving the byte-identical existing output.
- `MuxInvariantTest.VideoOutputPublishesOnlyFinalizedCommittedVideo` locks the independent cancellation, mux-finalization, duration, and written-video-packet publication gates.
- The current cancellation-safe video publication change passed focused lifecycle/mux/source-contract coverage, incremental installed product build `0.1.5128`, the complete exact-build native suite, and all six Python tool self-tests.

## Open Questions / Stale-risk

- Runtime validation with an actual elevated CE process writing to a persistent mapped network drive should confirm the log reports `source=registry_mapping` when the elevated token cannot see the live mapping.
- A real hotkey stop during both inject and WGC/DXGI warm-up should confirm that no final recording and no lingering staging file remain; deterministic lifecycle and output-disposition tests cover the race and cleanup policy offline.
- Filesystem atomicity and identity semantics still depend on the destination filesystem implementing the corresponding Windows operations; network-share runtime validation remains useful.
