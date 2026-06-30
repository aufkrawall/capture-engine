# Recording Output Paths

Last cross-checked: 2026-06-30 (mapped-drive output directories are resolved to UNC for elevated recording)

## Summary

Recording output paths are resolved in the MediaEngine DLL:

- Normal video recordings use `VideoEncoder::GenerateOutputFilename()` in `mediaengine/video_encoder.cpp`.
- Audio-only recordings use `MediaEngine::InitAudioOnlyMuxer()` in `mediaengine/mediaengine.cpp`.

`[Video] output_dir` may be empty, relative, absolute local, UNC, or a mapped-drive path. Empty normal video output writes to the `captures` subfolder next to the executable. Relative normal video output is resolved below the executable directory. Audio-only output still uses the existing audio-only behavior for empty/relative paths (`.` / current process working directory); the mapped-drive fix applies to drive-absolute output directories before the muxer opens the file.

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
- `mediaengine/video_encoder.cpp` (`GenerateOutputFilename`, mapped output-directory log)
- `mediaengine/mediaengine.cpp` (`InitAudioOnlyMuxer`, audio-only mapped output-directory log)
- `tests/test_path_utils.cpp` (drive-absolute detection and UNC root replacement)

## Validation

- `python build.py --skip-updates --run-tests --gtest-filter=PathUtilsTest.*:ConfigTest.ParsePerformancePriorityValues:ConfigTest.InvalidPerformancePriorityValuesFallBackConservatively` passed 6 focused tests and completed build 0.1.4369.

## Open Questions / Stale-risk

- Runtime validation with an actual elevated CE process writing to a persistent mapped network drive should confirm the log reports `source=registry_mapping` when the elevated token cannot see the live mapping.
- Screenshot output still has a separate path path in `captureengine/screenshot.cpp`; this page only documents recording outputs.
