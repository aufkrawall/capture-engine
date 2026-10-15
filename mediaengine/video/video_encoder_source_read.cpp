#include "video_encoder_internal.h"
#include "common/logging/log_meter.h"

#include <utility>

// Inject frames arrive in producer-owned shared texture slots. EncodeFrame only
// submits its GPU reads (fit, conversion, repeat-source staging) and returns;
// the caller then hands the slot back to the producer. Without a completion
// point the producer's next write into that slot can execute on the GPU before
// the encoder's queued read, so the encoded frame shows newer content than its
// timestamp. The fence below marks that completion point.
bool VideoEncoder::PrepareSourceReadCompletion() {
    if (!d3d11Device || !d3d11Context)
        return false;

    D3D11ScopedLock lock;
    if (sourceReadFenceDevice && sourceReadFenceDevice != d3d11Device)
        ReleaseSourceReadFence();
    sourceReadFenceDevice = d3d11Device;
    // Reserve a fallback before issuing any source reads. Fence creation or
    // Signal failure must never turn an outstanding read into an empty token.
    if (!sourceReadFallbackQuery) {
        const D3D11_QUERY_DESC desc{D3D11_QUERY_EVENT, 0};
        const HRESULT hr = d3d11Device->CreateQuery(&desc, &sourceReadFallbackQuery);
        if (FAILED(hr)) {
            static ce::log_meter::ChangeGate queryFailureLog(1000);
            const auto log = queryFailureLog.Observe(ce::log_meter::FieldKey(hr), GetTickCount64());
            if (log)
                DLL_Log("[VideoEncoder] Source-read query allocation failed HR=%x; no source read submitted%s",
                        static_cast<unsigned>(hr), ce::log_meter::SuppressedNote(log.suppressed).c_str());
            return false;
        }
    }
    if (!sourceReadFence && !sourceReadFenceUnavailableLogged) {
        const HRESULT hr = d3d11Device->CreateFence(0, D3D11_FENCE_FLAG_NONE, IID_PPV_ARGS(&sourceReadFence));
        if (FAILED(hr) || !sourceReadFence) {
            sourceReadFence = nullptr;
            if (!sourceReadFenceUnavailableLogged) {
                DLL_Log("[VideoEncoder] Source-read fence unavailable (HR=%x); using GPU event queries",
                        static_cast<unsigned>(hr));
                sourceReadFenceUnavailableLogged = true;
            }
            return true;
        }
        sourceReadFenceDevice = d3d11Device;
        sourceReadFenceValue = 0;
        DLL_Log("[VideoEncoder] Source-read fence created: inject slot release waits for the encoder's GPU read");
    }
    return true;
}

void VideoEncoder::SignalSourceReadCompletion() {
    D3D11ScopedLock lock;
    const uint64_t value = sourceReadFenceValue + 1;
    const HRESULT signalHr = sourceReadFence ? d3d11Context->Signal(sourceReadFence, value) : E_NOINTERFACE;
    if (FAILED(signalHr)) {
        static ce::log_meter::ChangeGate signalFailureLog(1000);
        const auto log = signalFailureLog.Observe(ce::log_meter::FieldKey(signalHr), GetTickCount64());
        if (log) {
            DLL_Log("[VideoEncoder] Source-read fence Signal unavailable HR=%x value=%llu; using GPU event query%s",
                    static_cast<unsigned>(signalHr), static_cast<unsigned long long>(value),
                    ce::log_meter::SuppressedNote(log.suppressed).c_str());
        }
        d3d11Context->End(sourceReadFallbackQuery);
        pendingSourceReadQuery = std::exchange(sourceReadFallbackQuery, nullptr);
    } else {
        sourceReadFenceValue = value;
        pendingSourceReadValue = value;
    }
    // Submit the queued reads and the signal now: the slot's release waits on
    // this value, and an unflushed signal would hold it until a later flush.
    d3d11Context->Flush();
}

bool VideoEncoder::TakeSourceReadToken(void** marker, uint64_t* value) {
    if (!marker || !value)
        return false;
    *marker = nullptr;
    *value = 0;
    if (pendingSourceReadQuery) {
        *marker = std::exchange(pendingSourceReadQuery, nullptr);
        return true;
    }
    if (pendingSourceReadValue == 0 || !sourceReadFence)
        return false;
    sourceReadFence->AddRef();
    *marker = sourceReadFence;
    *value = pendingSourceReadValue;
    pendingSourceReadValue = 0;
    return true;
}

void VideoEncoder::ReleaseSourceReadFence() {
    // Tokens already handed out hold their own fence reference.
    if (sourceReadFence) {
        sourceReadFence->Release();
        sourceReadFence = nullptr;
    }
    sourceReadFenceDevice = nullptr;
    sourceReadFenceValue = 0;
    pendingSourceReadValue = 0;
    if (sourceReadFallbackQuery) {
        sourceReadFallbackQuery->Release();
        sourceReadFallbackQuery = nullptr;
    }
    if (pendingSourceReadQuery) {
        pendingSourceReadQuery->Release();
        pendingSourceReadQuery = nullptr;
    }
    sourceReadFenceUnavailableLogged = false;
}
