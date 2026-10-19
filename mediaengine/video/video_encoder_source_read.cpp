#include "video_encoder_internal.h"
#include "common/logging/log_meter.h"

#include <utility>

// Inject frames arrive in producer-owned shared texture slots. EncodeFrame only
// submits its GPU reads (fit, conversion, repeat-source staging) and returns;
// the caller then hands the slot back to the producer. Without a completion
// point the producer's next write into that slot can execute on the GPU before
// the encoder's queued read, so the encoded frame shows newer content than its
// timestamp. An event query after the last read marks that point; see
// source_read_markers.h for why it is neither a fence Signal nor polled from
// another thread.
bool VideoEncoder::PrepareSourceReadCompletion() {
    if (!d3d11Device || !d3d11Context)
        return false;

    D3D11ScopedLock lock;
    if (sourceReadQueryDevice && sourceReadQueryDevice != d3d11Device && sourceReadQuery) {
        sourceReadQuery->Release();
        sourceReadQuery = nullptr;
    }
    sourceReadQueryDevice = d3d11Device;
    // Reserve the marker before issuing any source read: a failed allocation
    // afterwards would leave an outstanding read without a completion point.
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
        DLL_Log("[VideoEncoder] Source-read completion marker: event query after the last read, polled on the "
                "encoder thread after encoder submits and show-existing repeats; inject slot release waits for the "
                "encoder's GPU read");
    }
    return true;
}

void VideoEncoder::SignalSourceReadCompletion() {
    D3D11ScopedLock lock;
    if (!sourceReadQuery)
        return;  // PrepareSourceReadCompletion failed, so no read was issued
    d3d11Context->End(sourceReadQuery);
    // No Flush: the encoder submit that follows submits the reads and this
    // marker together with the conversion NVENC waits for. A Flush of its own
    // here split CE's work into an extra GPU submission ahead of the encoder
    // submit, and on NVIDIA every such submission waited about one encode for
    // NVENC (session 20261016_010712, GPU timeline sourceRead=14-17 ms with only
    // End+Flush in that step, after the fence Signal was already gone), so the
    // conversions serialized behind NVENC. Error paths without a submit are
    // flushed by the next submission or by FinishSourceReads at teardown.
    ce::media::SourceReadMarker marker;
    marker.sequence = ce::media::InjectSourceReadSequencer().Issue();
    marker.query = std::exchange(sourceReadQuery, nullptr);
    marker.context = d3d11Context;
    marker.context->AddRef();
    // An untaken older sequence is covered too: markers complete in order.
    pendingSourceReadSequence = marker.sequence;
    outstandingSourceReads.push_back(marker);
}

void VideoEncoder::PollSourceReadCompletion() {
    if (outstandingSourceReads.empty())
        return;
    D3D11ScopedLock lock;
    ce::media::PollSourceReadMarkers(outstandingSourceReads, ce::media::InjectSourceReadSequencer());
}

bool VideoEncoder::TakeSourceReadToken(uint64_t* sequence) {
    if (!sequence)
        return false;
    *sequence = std::exchange(pendingSourceReadSequence, 0);
    return *sequence != 0;
}

bool VideoEncoder::FinishSourceReads(DWORD timeoutMs) {
    if (outstandingSourceReads.empty())
        return true;
    const size_t outstanding = outstandingSourceReads.size();
    const bool finished = ce::media::FinishSourceReadMarkers(
        outstandingSourceReads, ce::media::InjectSourceReadSequencer(), timeoutMs, MediaEngine_LockD3D11,
        MediaEngine_UnlockD3D11);
    if (!finished) {
        DLL_Log("[VideoEncoder] Source-read teardown: %zu of %zu GPU read(s) still pending after %lu ms; their "
                "slots stay held",
                outstandingSourceReads.size(), outstanding, static_cast<unsigned long>(timeoutMs));
    }
    return finished;
}

void VideoEncoder::ReleaseSourceReadQueries() {
    // Runs after the codec is gone, so no encoder submit can race the waits.
    // Markers that still did not complete stay queued (they own their query and
    // context) and are polled again: an unproven read never releases its slot.
    FinishSourceReads(kSourceReadTeardownTimeoutMs);
    D3D11ScopedLock lock;
    if (sourceReadQuery) {
        sourceReadQuery->Release();
        sourceReadQuery = nullptr;
    }
    pendingSourceReadSequence = 0;
    sourceReadQueryDevice = nullptr;
    sourceReadMarkerLogged = false;
}
