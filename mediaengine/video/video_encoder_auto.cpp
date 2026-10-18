#include "video_encoder_internal.h"

// encoder=auto inside one VideoEncoder (llm-wiki/encoder-auto-selection.md).
// Init leaves a provisional concrete name; the GPU that will encode decides the
// real one here, before the codec is opened. An opened encoder is never
// switched: the choice changes only while initDone is false.

extern ID3D11Device* g_SharedD3D11Device;  // mediaengine.cpp

namespace {

// The adapter the encoder runs (or will run) on: its own device, else the
// inject source's GPU, else the shared screen-grab device. False while none is
// known yet.
bool EncodeAdapterLuid(ID3D11Device* encoderDevice, int32_t luidLow, int32_t luidHigh, LUID* luid) {
    if (encoderDevice)
        return ce::media::AdapterLuidOfDevice(encoderDevice, luid);
    if (luidLow != 0 || luidHigh != 0) {
        luid->LowPart = static_cast<DWORD>(luidLow);
        luid->HighPart = static_cast<LONG>(luidHigh);
        return true;
    }
    return ce::media::AdapterLuidOfDevice(g_SharedD3D11Device, luid);
}

}  // namespace

// With `retireCurrent` the current choice failed its real open on this GPU and
// is excluded; returns true only when a different encoder replaced it.
// Otherwise returns false only when the replacement codec context could not be
// allocated.
bool VideoEncoder::ResolveAutoEncoder(const char* site, bool retireCurrent) {
    if (autoEncoder.request.empty() || initDone)
        return !retireCurrent;
    LUID luid = {};
    if (!EncodeAdapterLuid(d3d11Device, luidLow, luidHigh, &luid))
        return !retireCurrent;

    if (autoEncoder.resolved && !ce::media::SameAdapterLuid(luid, autoEncoder.adapter)) {
        autoEncoder.excluded.clear();  // exclusions belong to the GPU they failed on
        autoEncoder.resolved = false;
    }
    if (retireCurrent) {
        autoEncoder.excluded.push_back(savedConfig.encoder);
        autoEncoder.resolved = false;
        DLL_Log("[EncoderAuto] %s: %s failed its real open; retrying with the next candidate (excluded=%zu)", site,
                savedConfig.encoder.c_str(), autoEncoder.excluded.size());
    }
    if (autoEncoder.resolved)
        return true;

    const ce::media::AutoEncoderResolution resolution =
        ce::media::ResolveAutoEncoderForAdapter(autoEncoder.request, luid, savedConfig, autoEncoder.excluded);
    DLL_Log("%s site=%s", resolution.log.c_str(), site);
    autoEncoder.resolved = true;
    autoEncoder.adapter = luid;
    if (resolution.encoder.empty()) {
        // Nothing opens here: keep the current name so the real open reports
        // the concrete failure instead of a missing codec.
        return !retireCurrent;
    }
    if (resolution.encoder == savedConfig.encoder)
        return !retireCurrent;

    const AVCodec* codec = avcodec_find_encoder_by_name(resolution.encoder.c_str());
    if (!codec)
        return false;
    DLL_Log("[EncoderAuto] %s: encoder %s -> %s", site, savedConfig.encoder.c_str(), resolution.encoder.c_str());
    savedConfig.encoder = resolution.encoder;
    if (!codecCtx)
        return true;  // Start() or a later EnsureDevice allocates it by name
    // Pools and the Quick Sync derivation belong to the previous encoder's
    // route (bridge device or CE's); EnsureDevice rebuilds them for this one.
    avcodec_free_context(&codecCtx);
    ReleaseEncoderFramesContexts();
    codecCtx = avcodec_alloc_context3(codec);
    if (!codecCtx) {
        DLL_Log("[EncoderAuto] %s: failed to allocate the %s codec context", site, resolution.encoder.c_str());
        return false;
    }
    return true;
}

bool VideoEncoder::EnsureDevice() {
    for (;;) {
        if (EnsureDeviceOnce())
            return true;
        // Only a failed hardware open (codecOpenFailed) before the stream was
        // committed to the container is retried; configuration errors fail the
        // same way for every candidate. Each retry excludes one candidate, so
        // this ends with the list.
        if (!codecOpenFailed || stream || !ResolveAutoEncoder("open-failed", true))
            return false;
        codecOpenFailed = false;
    }
}
