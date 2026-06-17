#pragma once

#include <stdint.h>
#include <string>

struct ID3D11Device;

// A/V self-calibration runner (controller/media-process side). Measures the TRUE audio-vs-video
// content offset through CE's real WGC + WASAPI-loopback pipeline by emitting, at the same instant,
// a near-inaudible audio burst (loopback) and a white flash on a small calibration window (WGC),
// then measuring offset = loopback-audio-center-QPC - WGC-flash-center-QPC. This is the raw-offset
// gate methodology internalized so it runs automatically and caches per device+display. The pure
// detection/aggregation logic lives in mediaengine/av_sync_calibration.h (unit-tested); this is the
// hardware orchestration. Fail-safe: returns ok=false on any error (caller falls back).

namespace ce::avcal {

struct AvCalibrationResult {
    bool ok = false;         // a usable A/V offset is available (cache hit or fresh measurement)
    bool fromCache = false;  // served from the on-disk cache
    bool measured = false;   // a fresh measurement ran this call
    double offsetMs = 0.0;   // audio-late content offset (= video content delay to apply); >= 0
    std::string key;         // cache key (device + render format + display geometry)
    int sampleRate = 0;
    int channels = 0;
};

// Measure (or load from cache under cacheDir) the default render endpoint's audio-vs-video content
// offset. d3dDevice is the shared D3D11 device used both for the WGC capture session and for frame
// luma readback (pass MediaEngine_GetD3D11Device()). forceRemeasure ignores any cached value. Plays
// a faint calibration tone and briefly shows a small flashing window once per uncached device.
// Never throws; logs every step via the media-process logger ([AVSyncCalib] ...).
AvCalibrationResult MeasureAvOffset(ID3D11Device* d3dDevice, const std::string& cacheDir, bool forceRemeasure);

}  // namespace ce::avcal
