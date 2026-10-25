#include "mediaengine_internal.h"

#include <atomic>
#include <cstdio>
#include <mutex>

#include "common/capture/packed_p010_layout.h"
#include "common/platform/raii_helpers.h"

#include "mediaengine/engine/mediaengine_handles.h"
#include "mediaengine/video/gpu_memory_breakdown.h"
#include "mediaengine/video/retained_yuv_converter.h"

// Retained WGC/DXGI reservoir in the encoder's NV12/P010 (retained_frame_desc.h,
// llm-wiki/wgc-reservoir-vram-design.md).

// The ingress converter is used from the WGC/DXGI capture thread only. It
// never takes the engine API or mux lock (those are held for a whole encode).
// On the encoder's device the immediate context it shares with the encoder is
// serialized by the pipeline's D3D11 lock. A split capture device (DXGI
// duplication) draws on its own context, which only the capture thread uses,
// so it takes no D3D11 lock and never waits for an encode. Lock order
// everywhere: D3D11 lock, then converter mutex (the capture thread may already
// hold the recursive D3D11 lock).

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

MEDIAENGINE_API bool MediaEngine_QueryRetainedFrameRoute(MediaEngineHandle engine,
                                                         const MediaEngineRetainedFrameQuery* query,
                                                         MediaEngineRetainedFrameRoute* route) {
    if (!query || !route || query->size != sizeof(MediaEngineRetainedFrameQuery) ||
        route->size != sizeof(MediaEngineRetainedFrameRoute))
        return false;
    MediaEngineLease lease(engine);
    if (!lease) {
        *route = MediaEngineRetainedFrameRoute{};
        std::snprintf(route->reason, sizeof(route->reason), "%s", "media engine not initialized");
        return true;
    }
    MediaEngineInstance& instance = *lease;
    std::lock_guard<std::recursive_mutex> apiLock(instance.apiMutex);
    if (!instance.engine) {
        *route = MediaEngineRetainedFrameRoute{};
        std::snprintf(route->reason, sizeof(route->reason), "%s", "media engine not initialized");
        return true;
    }
    return instance.engine->QueryRetainedFrameRoute(*query, route);
}

MEDIAENGINE_API bool MediaEngine_ConvertToRetainedFrame(MediaEngineHandle engine,
                                                        const MediaEngineRetainedFrameConversion* conversion) {
    if (!conversion || conversion->size != sizeof(MediaEngineRetainedFrameConversion) ||
        conversion->route.size != sizeof(MediaEngineRetainedFrameRoute) || conversion->route.yuvFormat == 0)
        return false;
    auto* source = static_cast<ID3D11ShaderResourceView*>(conversion->sourceView);
    const bool packed = ce::packed_p010::IsSlotFormat(static_cast<DXGI_FORMAT>(conversion->route.yuvFormat));
    if (!source || (packed ? !conversion->packedTarget : (!conversion->lumaTarget || !conversion->chromaTarget)))
        return false;

    MediaEngineLease lease(engine);
    if (!lease)
        return false;
    MediaEngineInstance& instance = *lease;
    ce::ComGuard<ID3D11Device> device;
    source->GetDevice(device.addressof());
    if (!device)
        return false;
    const bool encoderDevice = device.get() == instance.sharedDevice;
    if (encoderDevice)
        instance.d3d11Mutex.lock();
    struct Unlock {
        bool locked;
        std::recursive_mutex& mutex;
        ~Unlock() {
            if (locked)
                mutex.unlock();
        }
    } unlock{encoderDevice, instance.d3d11Mutex};
    std::lock_guard<std::mutex> converterLock(instance.retainedConverterMutex);
    if (!instance.retainedConverter.EnsureKernels(device.get()))
        return false;
    ce::ComGuard<ID3D11DeviceContext> context;
    device->GetImmediateContext(context.addressof());

    RetainedYuvConverter::Request request;
    request.source = source;
    request.width = conversion->width;
    request.height = conversion->height;
    request.lumaTarget = static_cast<ID3D11RenderTargetView*>(conversion->lumaTarget);
    request.chromaTarget = static_cast<ID3D11RenderTargetView*>(conversion->chromaTarget);
    request.packedTarget = packed ? static_cast<ID3D11RenderTargetView*>(conversion->packedTarget) : nullptr;
    request.eightBit = conversion->route.yuvFormat == DXGI_FORMAT_NV12;
    request.normalize = static_cast<ce::video_format::RgbColorTransform>(conversion->route.normalizeTransform);
    request.hdrOutput = conversion->route.hdrOutput;
    request.bt2020 = conversion->route.bt2020;
    request.fullRange = conversion->route.fullRange;

    const bool converted = context && instance.retainedConverter.Convert(context.get(), request);
    if (!converted && instance.retainedConversionFailures++ < 5) {
        DLL_Log("[RetainedYuv] Ingress conversion failed: format=%u %ux%u transform=%u failures=%llu",
                conversion->route.yuvFormat, conversion->width, conversion->height,
                conversion->route.normalizeTransform,
                static_cast<unsigned long long>(instance.retainedConversionFailures));
    }
    return converted;
}

void MediaEngineInstance::ReleaseRetainedFrameConverter() {
    d3d11Mutex.lock();
    struct Unlock {
        std::recursive_mutex& mutex;
        ~Unlock() { mutex.unlock(); }
    } unlock{d3d11Mutex};
    std::lock_guard<std::mutex> converterLock(retainedConverterMutex);
    if (retainedConverter.ConversionCount() > 0) {
        DLL_Log("[RetainedYuv] Ingress converter released: conversions=%llu packedP010=%llu failures=%llu",
                static_cast<unsigned long long>(retainedConverter.ConversionCount()),
                static_cast<unsigned long long>(retainedConverter.PackedConversionCount()),
                static_cast<unsigned long long>(retainedConversionFailures));
    }
    retainedConverter.Release();
    retainedConversionFailures = 0;
}

MEDIAENGINE_API void MediaEngine_ReleaseRetainedFrameConverter(MediaEngineHandle engine) {
    MediaEngineLease lease(engine);
    if (lease)
        lease->ReleaseRetainedFrameConverter();
}

MEDIAENGINE_API void MediaEngine_ReportCaptureGpuMemory(MediaEngineHandle engine, uint64_t reservoirBytes,
                                                        uint32_t reservoirSlots, uint64_t otherBytes,
                                                        bool sharedAcrossDevices) {
    MediaEngineLease lease(engine);
    if (!lease)
        return;
    ce::gpu_memory::CaptureGpuGauges& gauges = lease->captureGauges;
    gauges.sharedAcrossDevices.store(sharedAcrossDevices, std::memory_order_relaxed);
    gauges.reservoirBytes.store(reservoirBytes, std::memory_order_relaxed);
    gauges.reservoirSlots.store(reservoirSlots, std::memory_order_relaxed);
    gauges.otherBytes.store(otherBytes, std::memory_order_relaxed);
}

void ce::gpu_memory::LoadCaptureReport(const CaptureGpuGauges& gauges, Breakdown* breakdown) {
    breakdown->captureReservoir = gauges.reservoirBytes.load(std::memory_order_relaxed);
    breakdown->captureReservoirSlots = gauges.reservoirSlots.load(std::memory_order_relaxed);
    breakdown->captureOther = gauges.otherBytes.load(std::memory_order_relaxed);
    breakdown->captureSharedAcrossDevices = gauges.sharedAcrossDevices.load(std::memory_order_relaxed);
}
