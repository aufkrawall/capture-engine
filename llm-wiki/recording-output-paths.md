# Recording Output Paths

Last cross-checked: 2026-09-23 (recordings with committed packets survive trailer/close errors)

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
- A video muxer opens only an identity-owned same-directory reservation with the container extension. The container format is selected from configured metadata rather than the staging filename. Publication requires positive encoded duration and at least one successfully written video packet; a collision-safe atomic rename then exposes the final collision-safe name. A trailer or close failure with committed video no longer deletes the file (`VideoOutputDisposition::kPublishAfterFinalizeFailure`, 2026-09-23): `av_write_trailer` returns the AVIOContext's sticky error, so one transient write error anywhere in a long recording, or a full disk while the final cues are written, used to delete the whole recording. It is published with an `ERROR` log line; audio-only recordings follow the same rule (`ShouldPublishAudioOnlyOutput`). Warm-up cancellation and output without video still delete only the owned staging identity.
- An audio-only muxer retains the final-extension reservation model. Its reservation handle remains open without delete sharing for the writer lifetime. Successful close/trailer publishes the file; after a trailer or close error, at least one successfully written packet is enough to publish the recording with an error log. A failed empty output is still cleaned up by identity.
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

## Source changes after the output opened (2026-09-24)

`mediaengine/encode_geometry_policy.h` decides what a committed recording does when its capture source changes:

- **Colour contract** (HDR on/off, 10-bit input appearing/disappearing): `ReinitForFormatModeChange` /
  `PrepareFrameD3D11` used to `Stop()` and re-`Init()` with the preserved staging reservation, and the next
  `avio_open2(AVIO_FLAG_WRITE)` truncated the same staging file - everything before the switch was lost. After
  `fileOpened` the encoder now refuses the frame, calls `RequestStopForSourceContractChange` (shm
  `cmdStopRecording`) and `WasLastOutputDegraded` reports it via `sourceContractChanged`. Pre-open re-init is
  unchanged. Open: seamless continuation would need an SDR->HDR transform (`RgbColorTransform` has none) or
  segmenting into a second file with a re-anchored audio timeline.
- **Size**: the geometry is locked at header acceptance (`lockedGeometryWidth/Height`). A resized source is drawn
  by `FitSourceToLockedGeometry` (`video_encoder_geometry.cpp`) into a CE texture of the locked size, aspect
  preserved, centred on black, linear sampling, same typed format - every downstream path (VP, direct RGB, HDR
  P010 shader, repeat caches, face camera) keeps seeing the original geometry. The cursor state is kept raw and
  re-mapped onto the fitted rectangle (`AdjustCursorForFit`) for fresh frames and CFR repeats.
- **Persistent encode failure** (captureengine side): `ObserveVideoOutputAttempt` (`recording_health.h`) ends the
  recording after 5 s and >= 8 attempts with neither a fresh frame nor a cached repeat emitted (deferrals excluded),
  latching the video-degraded health flag.

Audio: sources that ran without their endpoint (`AudioCapture::GetDeviceUnavailableEpisodes`) and tracks with
`contentHoleSamples > 0` also mark the output degraded (`AudioSourcesLostTheirDevice`, `AudioTracksHaveContentHoles`).
Hardware validation of all of this is pending.
