#pragma once

// The live state a recording session keeps when it detaches from the inject session's shared memory.
//
// One SharedMemoryLayout serves the hook, the controller and the current recording. A recording that has been told
// to stop keeps finalizing (encoder drain, mux) while the controller may already start the next one on the same
// layout, so the finishing session must stop writing it: its phase resets, counter resets and encoder-texture
// retraction used to land on the newer recording, whose warmup-to-live commit then failed forever
// (llm-wiki/log/recent.md, 2026-10-21). On detach the session switches its view to a private layout seeded with
// what it still reads for its own decisions: its lifecycle phase and frame counters, the encoder<->media channel
// (overload flags, mux queue), the encoder-texture handshake its drain encodes from, and the header and identity
// fields validation reads. Everything else starts at its default; ring-slot leases keep completing on the shared
// ring through InjectFrameRingLeaseState.

#include "common/ipc/shared_defs.h"

#include <atomic>

namespace ce::detached_live_state {

namespace detail {
template <typename T>
inline void Copy(const std::atomic<T>& from, std::atomic<T>& to) noexcept {
    to.store(from.load(std::memory_order_acquire), std::memory_order_relaxed);
}
}  // namespace detail

inline void CopyForDetachedSession(const SharedMemoryLayout& live, SharedMemoryLayout& detached) noexcept {
    using detail::Copy;
    detached.SetVersion(live.GetVersion());
    Copy(live.structSize, detached.structSize);
    Copy(live.abiSignature, detached.abiSignature);
    detached.SetHostPID(live.GetHostPID());
    detached.SetLuidLowPart(live.GetLuidLowPart());
    detached.SetLuidHighPart(live.GetLuidHighPart());
    detached.SetLuidSourcePid(live.GetLuidSourcePid());
    detached.SetShmemMappingCreated(live.GetShmemMappingCreated());
    detached.SetShmemMappingSize(live.GetShmemMappingSize());

    const auto& from = live.encoderTextures;
    auto& to = detached.encoderTextures;
    for (int slot = 0; slot < ENCODER_TEXTURE_SLOT_COUNT; ++slot) {
        to.SetTextureHandle(slot, from.GetTextureHandle(slot));
        to.SetKmtTextureHandle(slot, from.GetKmtTextureHandle(slot));
    }
    to.SetFenceHandle(from.GetFenceHandle());
    to.SetWidth(from.GetWidth());
    to.SetHeight(from.GetHeight());
    to.SetFormat(from.GetFormat());
    Copy(from.ready, to.ready);
    Copy(from.kmtReady, to.kmtReady);
    Copy(live.useEncoderTextures, detached.useEncoderTextures);
    Copy(live.throttleCapture, detached.throttleCapture);
    Copy(live.encoderQueueDepth, detached.encoderQueueDepth);

    const CaptureState& state = live.runtimeState;
    CaptureState& own = detached.runtimeState;
    Copy(state.capturePhase, own.capturePhase);
    Copy(state.liveFramesEncoded, own.liveFramesEncoded);
    Copy(state.drainFramesEncoded, own.drainFramesEncoded);
    Copy(state.framesEncoded, own.framesEncoded);
    Copy(state.framesQueued, own.framesQueued);
    Copy(state.sourceFramesReceived, own.sourceFramesReceived);
    Copy(state.duplicateFrames, own.duplicateFrames);
    Copy(state.lateFrames, own.lateFrames);
    Copy(state.hostDroppedFrames, own.hostDroppedFrames);
    Copy(state.encoderOverloadFlags, own.encoderOverloadFlags);
    Copy(state.encoderSustainFpsX100, own.encoderSustainFpsX100);
    Copy(state.encoderBottlenecked, own.encoderBottlenecked);
    Copy(state.muxQueueBytes, own.muxQueueBytes);
    Copy(state.muxQueuePackets, own.muxQueuePackets);
    Copy(state.muxQueuePeakBytes, own.muxQueuePeakBytes);
    Copy(state.muxQueuePeakPackets, own.muxQueuePeakPackets);
    Copy(state.muxBackpressureCount, own.muxBackpressureCount);
    Copy(state.muxBackpressureWaitUs, own.muxBackpressureWaitUs);
    Copy(state.muxBackpressureMaxWaitUs, own.muxBackpressureMaxWaitUs);
    Copy(state.wgcAudioLeadExcessSamples, own.wgcAudioLeadExcessSamples);
    Copy(state.recordingHealthFlags, own.recordingHealthFlags);
    Copy(state.recordingTimelineDebtMs, own.recordingTimelineDebtMs);
    Copy(state.recordingPeakTimelineDebtMs, own.recordingPeakTimelineDebtMs);
    Copy(state.recordingStartTime, own.recordingStartTime);
    Copy(state.isRecording, own.isRecording);
    Copy(state.audioOnly, own.audioOnly);
    Copy(state.captureRequested, own.captureRequested);
    Copy(state.runtimeFlags, own.runtimeFlags);
    Copy(state.recordingFailureCode, own.recordingFailureCode);
    Copy(state.cmdStartRecording, own.cmdStartRecording);
    Copy(state.cmdStopRecording, own.cmdStopRecording);
    // Last, as on the shared layout: a reader that validates the header sees a complete copy.
    detached.SetMagic(live.GetMagic());
}

}  // namespace ce::detached_live_state
