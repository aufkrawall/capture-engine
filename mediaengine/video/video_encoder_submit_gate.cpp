#include "video_encoder_internal.h"
#include "common/logging/log_meter.h"

// The encoder device bridge (encoder_device_bridge.h) on the VideoEncoder side:
// pool frames on the encoder's own device, CE's work-device aliases of them,
// and the gate that hands a frame over at its fence and submits it once the
// CPU observes that fence complete.

namespace {

constexpr DWORD kSubmitGateTimeoutMs = 2000;

uint64_t ElapsedUs(PerfTimer::TimePoint start) {
    return static_cast<uint64_t>(std::max(0.0, PerfTimer::elapsed_ms(start, PerfTimer::now()) * 1000.0));
}

const char* WaitResultName(ce::media::EncoderDeviceBridge::WaitResult result) {
    using WaitResult = ce::media::EncoderDeviceBridge::WaitResult;
    switch (result) {
        case WaitResult::Complete:
            return "complete";
        case WaitResult::Timeout:
            return "timeout";
        case WaitResult::DeviceLost:
            return "device-lost";
        case WaitResult::Failed:
            return "failed";
    }
    return "unknown";
}

AVBufferRef* AllocateD3D11FramesContext(AVBufferRef* deviceCtx, AVPixelFormat swFormat, int width, int height,
                                        UINT miscFlags) {
    AVBufferRef* framesRef = av_hwframe_ctx_alloc(deviceCtx);
    if (!framesRef)
        return nullptr;
    auto* frames = reinterpret_cast<AVHWFramesContext*>(framesRef->data);
    auto* framesHw = reinterpret_cast<AVD3D11VAFramesContext*>(frames->hwctx);
    frames->format = AV_PIX_FMT_D3D11;
    // RGB->YUV output is written directly into AVHWFrame-owned textures. NVENC
    // retains the AVFrame until that input is no longer in flight, so
    // lookahead/B-frame depth cannot recycle a surface the encoder still reads.
    framesHw->BindFlags |= D3D11_BIND_RENDER_TARGET;
    framesHw->MiscFlags |= miscFlags;
    frames->sw_format = swFormat;
    frames->width = width;
    frames->height = height;
    // Dynamic pool: one texture per frame (array slice 0), allocated on demand.
    frames->initial_pool_size = 0;
    if (av_hwframe_ctx_init(framesRef) < 0)
        av_buffer_unref(&framesRef);
    return framesRef;
}

}  // namespace

ID3D11Texture2D* VideoEncoder::EncoderFrameTexture(const AVFrame* frame) {
    if (!frame || !frame->data[0])
        return nullptr;
    auto* texture = reinterpret_cast<ID3D11Texture2D*>(frame->data[0]);
    if (!encoderBridgeInUse)
        return texture;
    ID3D11Texture2D* workTexture = encoderBridge.WorkTexture(texture);
    if (!workTexture) {
        static ce::log_meter::ChangeGate aliasFailureLog(5000);
        const auto log = aliasFailureLog.Observe(ce::log_meter::FieldKey(0), GetTickCount64());
        if (log) {
            DLL_Log("[EncoderBridge] Opening a pool texture on CE's device failed (cached=%zu)%s",
                    encoderBridge.CachedTextureCount(), ce::log_meter::SuppressedNote(log.suppressed).c_str());
        }
    }
    return workTexture;
}

void VideoEncoder::ReleaseEncoderFramesContexts() {
    DropPendingEncoderSubmits("frames-context-release");
    if (hwFramesCtx)
        av_buffer_unref(&hwFramesCtx);
    if (hwDeviceCtx)
        av_buffer_unref(&hwDeviceCtx);
    if (d3d11FramesCtx)
        av_buffer_unref(&d3d11FramesCtx);
    if (encoderDeviceCtx)
        av_buffer_unref(&encoderDeviceCtx);
    encoderBridge.Reset();
    encoderBridgeInUse = false;
}

bool VideoEncoder::CreateEncoderFramesContext(const ResolvedVideoFormat& format, int framesWidth, int framesHeight) {
    ReleaseEncoderFramesContexts();
    const AVPixelFormat swFormat = format.d3d11SwFormat;
    const bool wantsBridge =
        format.codecPixFmt != AV_PIX_FMT_QSV && ce::media::EncoderUsesSeparateDevice(savedConfig.encoder.c_str());
    auto createBridged = [&](std::string* reason) -> bool {
        if (!encoderBridge.Create(d3d11Device, reason))
            return false;
        encoderDeviceCtx = av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_D3D11VA);
        if (!encoderDeviceCtx) {
            *reason = "hwdevice alloc";
            return false;
        }
        auto* deviceCtx = reinterpret_cast<AVHWDeviceContext*>(encoderDeviceCtx->data);
        auto* d3d11Ctx = reinterpret_cast<AVD3D11VADeviceContext*>(deviceCtx->hwctx);
        d3d11Ctx->device = encoderBridge.EncoderDevice();
        d3d11Ctx->device->AddRef();  // FFmpeg owns one reference
        if (av_hwdevice_ctx_init(encoderDeviceCtx) < 0) {
            *reason = "hwdevice init";
            return false;
        }
        if (!DeviceSupportsHwFrameSwFormat(encoderDeviceCtx, swFormat)) {
            *reason = "sw_format unsupported on the encoder device";
            return false;
        }
        d3d11FramesCtx = AllocateD3D11FramesContext(encoderDeviceCtx, swFormat, framesWidth, framesHeight,
                                                    ce::media::kEncoderBridgePoolMiscFlags);
        if (!d3d11FramesCtx) {
            *reason = "shared frames context init";
            return false;
        }
        // One real pool frame proves the format can be shared and opened on
        // CE's device before the codec opens; its alias stays cached.
        encoderBridgeInUse = true;
        AVFrame* probe = av_frame_alloc();
        const bool shared =
            probe && av_hwframe_get_buffer(d3d11FramesCtx, probe, 0) >= 0 && EncoderFrameTexture(probe) != nullptr;
        av_frame_free(&probe);
        if (!shared) {
            *reason = "pool texture cannot be opened on CE's device";
            return false;
        }
        hwDeviceCtx = av_buffer_ref(encoderDeviceCtx);
        hwFramesCtx = av_buffer_ref(d3d11FramesCtx);
        if (!hwDeviceCtx || !hwFramesCtx) {
            *reason = "hw context reference";
            return false;
        }
        return true;
    };
    if (wantsBridge) {
        std::string reason;
        if (createBridged(&reason)) {
            deferredSubmitDepth = ce::media::DeferredSubmitDepth(savedConfig.fps);
            submitGateStats.Reset(0);
            ApplyGpuThreadPriority(currentGpuThreadPriority, "initial");
            DLL_Log("[EncoderBridge] %s runs on its own D3D11 device: pool=%s %dx%d shared-nt, gate depth=%zu "
                    "(%dms at %dfps); CE's device carries no encoder submit",
                    savedConfig.encoder.c_str(), GetPixFmtNameSafe(swFormat), framesWidth, framesHeight,
                    deferredSubmitDepth, ce::media::kDeferredSubmitBudgetMs, savedConfig.fps);
            return true;
        }
        DLL_Log("[EncoderBridge] Separate %s device unavailable (%s); the encoder shares CE's D3D11 device",
                savedConfig.encoder.c_str(), reason.c_str());
        ReleaseEncoderFramesContexts();
    }

    if (!DeviceSupportsHwFrameSwFormat(d3d11DeviceCtx, swFormat)) {
        DLL_Log("[VideoEncoder] D3D11 HW frames do not support sw_format=%s on this device",
                GetPixFmtNameSafe(swFormat));
        return false;
    }
    d3d11FramesCtx = AllocateD3D11FramesContext(d3d11DeviceCtx, swFormat, framesWidth, framesHeight, 0);
    if (!d3d11FramesCtx) {
        DLL_Log("[VideoEncoder] Failed to init D3D11 frames context");
        return false;
    }
    return true;
}

bool VideoEncoder::HandOffFrameToEncoder(AVFrame* frame, ce::media::gpu_stage::Path path, const char* label,
                                         AVPacket* pkt, int* packetCount, bool logPacketDebug) {
    AVFrame* held = av_frame_clone(frame);
    if (!held) {
        DLL_Log("[EncoderBridge] %s: could not reference the frame for the submit gate", label);
        return false;
    }
    uint64_t fenceValue = 0;
    {
        D3D11ScopedLock lock;
        const auto start = PerfTimer::now();
        gpuStageMonitor.BeforeSend(path, d3d11Context);
        fenceValue = encoderBridge.SignalWorkDone(d3d11Context);
        gpuStageMonitor.AfterSend(path, d3d11Context, ElapsedUs(start));
    }
    if (fenceValue == 0) {
        av_frame_free(&held);
        DLL_Log("[EncoderBridge] %s: fence signal on CE's device failed", label);
        return false;
    }
    pendingEncoderSubmits.push_back({held, fenceValue, path, label, logPacketDebug});
    pendingEncoderSubmitCount.store(pendingEncoderSubmits.size(), std::memory_order_release);
    submitGateStats.pendingPeak = std::max(submitGateStats.pendingPeak, pendingEncoderSubmits.size());
    // The frame is accepted once handed over; a later send failure of an older
    // frame is logged by the gate and leaves its hole to the CFR coverage check.
    ServicePendingEncoderSubmits(false, pkt, packetCount);
    return true;
}

bool VideoEncoder::ServicePendingEncoderSubmits(bool drainAll, AVPacket* pkt, int* packetCount) {
    static ce::log_meter::ChangeGate gateFailureLog(1000);
    bool success = true;
    while (!pendingEncoderSubmits.empty()) {
        ce::media::PendingEncoderSubmit entry = pendingEncoderSubmits.front();
        const auto action = ce::media::NextSubmitGateAction(pendingEncoderSubmits.size(), deferredSubmitDepth,
                                                            encoderBridge.IsComplete(entry.fenceValue), drainAll);
        if (action == ce::media::SubmitGateAction::Hold)
            break;
        pendingEncoderSubmits.pop_front();
        pendingEncoderSubmitCount.store(pendingEncoderSubmits.size(), std::memory_order_release);
        bool ready = true;
        if (action == ce::media::SubmitGateAction::WaitThenSend) {
            const auto start = PerfTimer::now();
            const auto result = encoderBridge.Wait(entry.fenceValue, kSubmitGateTimeoutMs);
            const uint64_t waitUs = ElapsedUs(start);
            ++submitGateStats.waits;
            submitGateStats.waitAccumUs += waitUs;
            submitGateStats.waitMaxUs = std::max(submitGateStats.waitMaxUs, waitUs);
            ready = result == ce::media::EncoderDeviceBridge::WaitResult::Complete;
            if (!ready) {
                const auto log = gateFailureLog.Observe(ce::log_meter::FieldKey(static_cast<int>(result)),
                                                        GetTickCount64());
                if (log) {
                    DLL_Log("[EncoderBridge] %s: conversion fence %llu not reached (%s after %lluus, pending=%zu); "
                            "frame pts=%lld dropped%s",
                            entry.label, static_cast<unsigned long long>(entry.fenceValue), WaitResultName(result),
                            static_cast<unsigned long long>(waitUs), pendingEncoderSubmits.size(),
                            static_cast<long long>(entry.frame->pts),
                            ce::log_meter::SuppressedNote(log.suppressed).c_str());
                }
            }
        }
        if (ready && !SendFrameNow(entry.frame, entry.path, entry.label, pkt, packetCount, entry.logPacketDebug,
                                   false))
            ready = false;
        success = success && ready;
        av_frame_free(&entry.frame);
    }
    return success;
}

void VideoEncoder::FlushPendingEncoderSubmits(const char* reason) {
    if (pendingEncoderSubmits.empty())
        return;
    const size_t pending = pendingEncoderSubmits.size();
    AVPacket* pkt = av_packet_alloc();
    const bool success = pkt && codecCtx && ServicePendingEncoderSubmits(true, pkt, nullptr);
    av_packet_free(&pkt);
    if (!pendingEncoderSubmits.empty())
        DropPendingEncoderSubmits(reason);
    DLL_Log("[EncoderBridge] Flushed %zu gated submit(s) at %s: %s", pending, reason, success ? "sent" : "failed");
}

void VideoEncoder::DropPendingEncoderSubmits(const char* reason) {
    if (pendingEncoderSubmits.empty())
        return;
    const size_t dropped = pendingEncoderSubmits.size();
    for (auto& entry : pendingEncoderSubmits)
        av_frame_free(&entry.frame);
    pendingEncoderSubmits.clear();
    pendingEncoderSubmitCount.store(0, std::memory_order_release);
    DLL_Log("[EncoderBridge] Dropped %zu gated submit(s) at %s", dropped, reason);
}
