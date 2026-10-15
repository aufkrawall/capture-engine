#include "video_encoder_internal.h"
#include "common/logging/log_meter.h"

#include <utility>

// Inject frames arrive in producer-owned shared texture slots. EncodeFrame only
// submits its GPU reads (fit, conversion, repeat-source staging) and returns;
// the caller then hands the slot back to the producer. Without a completion
// point the producer's next write into that slot can execute on the GPU before
// the encoder's queued read, so the encoded frame shows newer content than its
// timestamp. An event query placed after the last read marks that point.
//
// It is deliberately not an ID3D11Fence Signal: on the NVENC encoder device a
// per-frame Signal held CE's whole 3D queue for about two encode times (session
// 20261016_003600, [VideoEncoder GPU Timeline] sourceRead=16-21 ms on the GPU
// against 0.13 ms on the CPU), so every later conversion queued behind NVENC
// and inject p4 overloaded while DXGI duplication, which signals nothing,
// sustained p6. A query only records completion and never stalls the queue.
bool VideoEncoder::PrepareSourceReadCompletion() {
    if (!d3d11Device || !d3d11Context)
        return false;

    D3D11ScopedLock lock;
    if (sourceReadQueryDevice && sourceReadQueryDevice != d3d11Device)
        ReleaseSourceReadQueries();
    sourceReadQueryDevice = d3d11Device;
    // Reserve the marker before issuing any source read: a failed allocation
    // afterwards would turn an outstanding read into an empty token.
    if (!sourceReadQuery) {
        const D3D11_QUERY_DESC desc{D3D11_QUERY_EVENT, 0};
        const HRESULT hr = d3d11Device->CreateQuery(&desc, &sourceReadQuery);
        if (FAILED(hr)) {
            sourceReadQuery = nullptr;
            static ce::log_meter::ChangeGate queryFailureLog(1000);
            const auto log = queryFailureLog.Observe(ce::log_meter::FieldKey(hr), GetTickCount64());
            if (log)
                DLL_Log("[VideoEncoder] Source-read query allocation failed HR=%x; no source read submitted%s",
                        static_cast<unsigned>(hr), ce::log_meter::SuppressedNote(log.suppressed).c_str());
            return false;
        }
    }
    if (!sourceReadMarkerLogged) {
        sourceReadMarkerLogged = true;
        DLL_Log("[VideoEncoder] Source-read completion marker: event query after the last read (no fence Signal "
                "on the encoder device); inject slot release waits for the encoder's GPU read");
    }
    return true;
}

void VideoEncoder::SignalSourceReadCompletion() {
    D3D11ScopedLock lock;
    if (!sourceReadQuery)
        return;  // PrepareSourceReadCompletion failed, so no read was issued
    d3d11Context->End(sourceReadQuery);
    if (pendingSourceReadQuery)
        pendingSourceReadQuery->Release();  // never taken; its read is covered by the newer marker
    pendingSourceReadQuery = std::exchange(sourceReadQuery, nullptr);
    // Submit the queued reads and the marker now: the slot's release waits on
    // it, and an unflushed marker would hold the slot until a later flush.
    d3d11Context->Flush();
}

bool VideoEncoder::TakeSourceReadToken(void** marker, uint64_t* value) {
    if (!marker || !value)
        return false;
    *marker = nullptr;
    *value = 0;
    if (!pendingSourceReadQuery)
        return false;
    *marker = std::exchange(pendingSourceReadQuery, nullptr);
    return true;
}

void VideoEncoder::ReleaseSourceReadQueries() {
    // Tokens already handed out own their query.
    if (sourceReadQuery) {
        sourceReadQuery->Release();
        sourceReadQuery = nullptr;
    }
    if (pendingSourceReadQuery) {
        pendingSourceReadQuery->Release();
        pendingSourceReadQuery = nullptr;
    }
    sourceReadQueryDevice = nullptr;
    sourceReadMarkerLogged = false;
}
