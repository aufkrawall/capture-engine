#include "video_encoder_internal.h"

#include "face_camera_renderer.h"
#include "gpu_memory_breakdown.h"

// [GpuMemory] per-component video memory of a recording (gpu_memory_breakdown.h).
// Logged with the once-per-second encoder timing window while the totals move
// by 64 MB, at least once a minute while they stay, and once at stop with the
// peak. Everything CE allocates is named; shared textures the process counter
// includes twice are subtracted, and `unaccounted` is the encoder session's
// internal buffers, the drivers' per-device allocations and the desktop
// duplication surfaces.

namespace {

DXGI_FORMAT FramesContextFormat(const AVBufferRef* framesRef, uint32_t* width, uint32_t* height) {
    *width = *height = 0;
    if (!framesRef || !framesRef->data)
        return DXGI_FORMAT_UNKNOWN;
    const auto* frames = reinterpret_cast<const AVHWFramesContext*>(framesRef->data);
    *width = static_cast<uint32_t>(frames->width);
    *height = static_cast<uint32_t>(frames->height);
    switch (frames->sw_format) {
        case AV_PIX_FMT_NV12:
            return DXGI_FORMAT_NV12;
        case AV_PIX_FMT_P010:
            return DXGI_FORMAT_P010;
        default:
            return DXGI_FORMAT_B8G8R8A8_UNORM;
    }
}

}  // namespace

void VideoEncoder::LogGpuMemory(const char* stage, bool force) {
    if (!d3d11Device)
        return;
    ce::gpu_memory::Breakdown b;
    if (!ce::gpu_memory::QueryProcessVideoMemory(d3d11Device, &b.processLocal, &b.processNonLocal, &b.budget))
        return;
    gpuMemory.peakLocal = std::max(gpuMemory.peakLocal, b.processLocal);
    ce::gpu_memory::LoadCaptureReport(instance.captureGauges, &b);

    uint32_t frameWidth = 0;
    uint32_t frameHeight = 0;
    const DXGI_FORMAT frameFormat = FramesContextFormat(d3d11FramesCtx, &frameWidth, &frameHeight);
    b.encoderFrameCount = static_cast<uint32_t>(encoderBridgeInUse ? encoderBridge.CachedTextureCount()
                                                                   : gpuMemory.poolTextures.size());
    b.encoderFrames = b.encoderFrameCount * ce::gpu_memory::EstimateTextureBytes(frameWidth, frameHeight, frameFormat);

    using ce::gpu_memory::TextureBytes;
    b.repeat = TextureBytes(repeatFrameTexture) + TextureBytes(stagedRepeatFrameTexture) +
               TextureBytes(repeatSourceFrameTexture) + TextureBytes(pendingRepeatSourceFrameTexture);
    b.conversion = TextureBytes(bgraStagingTexture) + TextureBytes(swapRBTexture) + TextureBytes(geometryFitTexture) +
                   TextureBytes(rgb10IntermediateTexture) + TextureBytes(vpInputFp16Staging) +
                   TextureBytes(sampleableSourceCopy.Texture()) + frameScaler.GpuBytes();
    b.overlays = TextureBytes(cursorRestoreTexture) + TextureBytes(cursorCompositeTexture) +
                 TextureBytes(faceCameraRestoreTexture) + TextureBytes(faceCameraCompositeTexture) +
                 TextureBytes(retainedYuv.camera.texture) + TextureBytes(retainedYuv.cursor.texture) +
                 (faceCameraRenderer ? faceCameraRenderer->GpuBytes() : 0);
    b.codecOpenDelta = gpuMemory.codecOpenDelta;
    // The bridge's pool lives on the encoder device and is opened on CE's;
    // the split capture pool is opened on the capture device.
    b.countedTwice =
        (encoderBridgeInUse ? b.encoderFrames : 0) + (b.captureSharedAcrossDevices ? b.captureReservoir : 0);

    const uint64_t key = ce::gpu_memory::BreakdownLogKey(b);
    const uint64_t now = GetTickCount64();
    const auto verdict = force ? gpuMemory.gate.Force(key, now) : gpuMemory.gate.Observe(key, now);
    if (!verdict)
        return;
    DLL_Log("%s peakLocal=%.0fMB%s", ce::gpu_memory::FormatBreakdown(b, stage).c_str(),
            static_cast<double>(gpuMemory.peakLocal) / (1024.0 * 1024.0),
            ce::log_meter::SuppressedNote(verdict.suppressed).c_str());
    if (force) {
        gpuMemory.peakLocal = 0;
        gpuMemory.gate.Reset();
    }
}
