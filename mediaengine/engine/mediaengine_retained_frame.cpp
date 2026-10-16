#include "mediaengine_internal.h"

#include <atomic>
#include <cstdio>
#include <mutex>

#include "common/platform/raii_helpers.h"

#include "mediaengine/video/gpu_memory_breakdown.h"
#include "mediaengine/video/retained_yuv_converter.h"

// Retained WGC/DXGI reservoir in the encoder's NV12/P010 (retained_frame_desc.h,
// llm-wiki/wgc-reservoir-vram-design.md).

namespace {

// The ingress converter is used from the WGC/DXGI capture thread only. It
// never takes the engine API or mux lock (those are held for a whole encode).
// On the encoder's device the immediate context it shares with the encoder is
// serialized by the MediaEngine D3D11 lock. A split capture device (DXGI
// duplication) draws on its own context, which only the capture thread uses,
// so it takes no D3D11 lock and never waits for an encode. Lock order
// everywhere: D3D11 lock, then converter mutex (the capture thread may already
// hold the recursive D3D11 lock).
// NOLINTNEXTLINE(bugprone-throwing-static-initialization) - std::mutex-family constructors are noexcept on this toolchain
std::mutex g_RetainedConverterMutex;
uint64_t g_RetainedConversionFailures = 0;

// Last capture pool report (MediaEngine_ReportCaptureGpuMemory).
std::atomic<uint64_t> g_CaptureReservoirBytes{0};
std::atomic<uint32_t> g_CaptureReservoirSlots{0};
std::atomic<uint64_t> g_CaptureOtherBytes{0};
std::atomic<bool> g_CaptureSharedAcrossDevices{false};

// Released by captureengine with its pool (MediaEngine_ReleaseRetainedFrameConverter),
// never by a static destructor at DLL unload, where COM releases are unsafe.
RetainedYuvConverter& RetainedConverter() {
    static auto* converter = new RetainedYuvConverter();
    return *converter;
}

}  // namespace

bool MediaEngine::QueryRetainedFrameRoute(const MediaEngineRetainedFrameQuery& query,
                                          MediaEngineRetainedFrameRoute* route) {
    std::lock_guard<std::recursive_mutex> lock(muxMutex);
    if (!videoEnc) {
        *route = MediaEngineRetainedFrameRoute{};
        std::snprintf(route->reason, sizeof(route->reason), "%s", "no video encoder");
        return true;
    }
    videoEnc->ResolveRetainedFrameRoute(query, route);
    return true;
}

MEDIAENGINE_API bool MediaEngine_QueryRetainedFrameRoute(const MediaEngineRetainedFrameQuery* query,
                                                         MediaEngineRetainedFrameRoute* route) {
    if (!query || !route || query->size != sizeof(MediaEngineRetainedFrameQuery) ||
        route->size != sizeof(MediaEngineRetainedFrameRoute))
        return false;
    std::lock_guard<std::recursive_mutex> apiLock(mediaengine_g_EngineApiMutex);
    if (!mediaengine_g_Engine) {
        *route = MediaEngineRetainedFrameRoute{};
        std::snprintf(route->reason, sizeof(route->reason), "%s", "media engine not initialized");
        return true;
    }
    return mediaengine_g_Engine->QueryRetainedFrameRoute(*query, route);
}

MEDIAENGINE_API bool MediaEngine_ConvertToRetainedFrame(const MediaEngineRetainedFrameConversion* conversion) {
    if (!conversion || conversion->size != sizeof(MediaEngineRetainedFrameConversion) ||
        conversion->route.size != sizeof(MediaEngineRetainedFrameRoute) || conversion->route.yuvFormat == 0)
        return false;
    auto* source = static_cast<ID3D11ShaderResourceView*>(conversion->sourceView);
    if (!source || !conversion->lumaTarget || !conversion->chromaTarget)
        return false;

    ce::ComGuard<ID3D11Device> device;
    source->GetDevice(device.addressof());
    if (!device)
        return false;
    const bool encoderDevice = device.get() == g_SharedD3D11Device;
    if (encoderDevice)
        MediaEngine_LockD3D11();
    struct Unlock {
        bool locked;
        ~Unlock() {
            if (locked)
                MediaEngine_UnlockD3D11();
        }
    } unlock{encoderDevice};
    std::lock_guard<std::mutex> converterLock(g_RetainedConverterMutex);
    if (!RetainedConverter().EnsureKernels(device.get()))
        return false;
    ce::ComGuard<ID3D11DeviceContext> context;
    device->GetImmediateContext(context.addressof());

    RetainedYuvConverter::Request request;
    request.source = source;
    request.width = conversion->width;
    request.height = conversion->height;
    request.lumaTarget = static_cast<ID3D11RenderTargetView*>(conversion->lumaTarget);
    request.chromaTarget = static_cast<ID3D11RenderTargetView*>(conversion->chromaTarget);
    request.eightBit = conversion->route.yuvFormat == DXGI_FORMAT_NV12;
    request.normalize = static_cast<ce::video_format::RgbColorTransform>(conversion->route.normalizeTransform);
    request.hdrOutput = conversion->route.hdrOutput;
    request.bt2020 = conversion->route.bt2020;
    request.fullRange = conversion->route.fullRange;

    const bool converted = context && RetainedConverter().Convert(context.get(), request);
    if (!converted && g_RetainedConversionFailures++ < 5) {
        DLL_Log("[RetainedYuv] Ingress conversion failed: format=%u %ux%u transform=%u failures=%llu",
                conversion->route.yuvFormat, conversion->width, conversion->height,
                conversion->route.normalizeTransform,
                static_cast<unsigned long long>(g_RetainedConversionFailures));
    }
    return converted;
}

MEDIAENGINE_API void MediaEngine_ReleaseRetainedFrameConverter() {
    MediaEngine_LockD3D11();
    struct Unlock {
        ~Unlock() { MediaEngine_UnlockD3D11(); }
    } unlock;
    std::lock_guard<std::mutex> converterLock(g_RetainedConverterMutex);
    if (RetainedConverter().ConversionCount() > 0) {
        DLL_Log("[RetainedYuv] Ingress converter released: conversions=%llu failures=%llu",
                static_cast<unsigned long long>(RetainedConverter().ConversionCount()),
                static_cast<unsigned long long>(g_RetainedConversionFailures));
    }
    RetainedConverter().Release();
    g_RetainedConversionFailures = 0;
}

MEDIAENGINE_API void MediaEngine_ReportCaptureGpuMemory(uint64_t reservoirBytes, uint32_t reservoirSlots,
                                                        uint64_t otherBytes, bool sharedAcrossDevices) {
    g_CaptureSharedAcrossDevices.store(sharedAcrossDevices, std::memory_order_relaxed);
    g_CaptureReservoirBytes.store(reservoirBytes, std::memory_order_relaxed);
    g_CaptureReservoirSlots.store(reservoirSlots, std::memory_order_relaxed);
    g_CaptureOtherBytes.store(otherBytes, std::memory_order_relaxed);
}

void ce::gpu_memory::LoadCaptureReport(Breakdown* breakdown) {
    breakdown->captureReservoir = g_CaptureReservoirBytes.load(std::memory_order_relaxed);
    breakdown->captureReservoirSlots = g_CaptureReservoirSlots.load(std::memory_order_relaxed);
    breakdown->captureOther = g_CaptureOtherBytes.load(std::memory_order_relaxed);
    breakdown->captureSharedAcrossDevices = g_CaptureSharedAcrossDevices.load(std::memory_order_relaxed);
}
