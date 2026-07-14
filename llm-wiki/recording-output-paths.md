# Recording Output Paths

Last cross-checked: 2026-07-15 (shared collision-safe reservation and atomic publication for video, audio-only, PNG, and AVIF outputs)

## Summary

All capture outputs use `ce::capture_output::ReservedCaptureOutput`:

- Normal video recordings reserve through `VideoEncoder` in `mediaengine/video_encoder.cpp`.
- Audio-only recordings reserve through `MediaEngine::InitAudioOnlyMuxer()` in `mediaengine/mediaengine.cpp`.
- SDR PNG and HDR AVIF screenshots reserve and atomically publish through `captureengine/screenshot_encoding.cpp`.

`[Video] output_dir` may be empty, relative, absolute local, UNC, or a mapped-drive path. Empty video, audio-only, and screenshot output writes to the `captures` subfolder next to the executable. Relative output is resolved below the executable directory. All three paths preserve the mapped-drive behavior below.

## Reservation And Publication Invariants

- Filenames contain UTC milliseconds, the writer PID, and an atomic process-local sequence. A collision adds a bounded retry suffix.
- The destination is reserved with `CreateFileW(CREATE_NEW)`. Existing paths are never truncated, removed, or selected as the recording destination.
- The reservation records the Windows volume/file identity. Failure cleanup deletes only a path that still has the reserved identity.
- A video or audio muxer opens only its owned placeholder. The reservation handle remains open without delete sharing for the writer lifetime; successful close/trailer publishes the file, while failure cleanup removes only the owned partial file.
- A screenshot is fully encoded and closed in a separately reserved staging file, then atomically replaces only its owned zero-byte placeholder with `ReplaceFileW(..., REPLACEFILE_WRITE_THROUGH)`.
- Post-mux duration probing and exact audio finalization retain the reserved filename. User-visible screenshot notification occurs only after final atomic publication.

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
- `mediaengine/video_encoder.cpp` (`ReserveOutputFilename`, muxer ownership and publication)
- `mediaengine/mediaengine.cpp` (`InitAudioOnlyMuxer`, audio-only ownership and publication)
- `captureengine/screenshot_encoding.cpp` (reserved staging and atomic final commit)
- `tests/test_path_utils.cpp` and `tests/test_reserved_capture_output.cpp`

## Validation

- `ReservedCaptureOutputTest` covers forced identical clock/PID/sequence values for video, audio-only, PNG, and AVIF extensions; it proves collision retry and byte-identical sentinel preservation.
- The required full product build passed as build 0.1.4806. The canonical no-build run passed 1,501 native tests in 105 suites and all four Python tool self-tests.

## Open Questions / Stale-risk

- Runtime validation with an actual elevated CE process writing to a persistent mapped network drive should confirm the log reports `source=registry_mapping` when the elevated token cannot see the live mapping.
- Filesystem atomicity and identity semantics still depend on the destination filesystem implementing the corresponding Windows operations; network-share runtime validation remains useful.
