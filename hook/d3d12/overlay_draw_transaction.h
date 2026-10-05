#pragma once

#include <cstdint>
#include <utility>

// Private frame control: initialization skips still reach sync/capture; draw
// skips finish the draw region; only kReturn ends the whole frame transaction.
enum class ProcessFrameFlow {
    kContinue, kReturn, kSkipOverlayInit, kSkipOverlayDraw, kOverlayDone,
};

namespace ce::dx12 {
enum class DrawFailure { MissingCommands, AllocatorReset, CommandListReset, MissingSwapchain, GetBuffer, Close };

// The per-present COM reference has one normal retirement point and an
// idempotent destructor backstop for exits before that point. It never caches
// a backbuffer across presents and acquires no additional reference.
template<class Buffer>
class DrawBackBuffer {
public:
    DrawBackBuffer() = default;
    ~DrawBackBuffer() { Release(); }
    DrawBackBuffer(const DrawBackBuffer&) = delete;
    DrawBackBuffer& operator=(const DrawBackBuffer&) = delete;
    void Adopt(Buffer* buffer) { Release(); buffer_ = buffer; }
    Buffer* Borrow() const { return buffer_; }
    void Release() { if (Buffer* buffer = std::exchange(buffer_, nullptr)) buffer->Release(); }
private:
    Buffer* buffer_ = nullptr;
};

template<class Buffer>
struct BackBufferAcquisition { int32_t result; Buffer* buffer; };

// Production and controlled unit operations execute this same draw transaction.
// SDK calls remain statically bound; no allocation, virtual adapter, new copies
// or changed scheduling policy is introduced. Recovery belongs only to failed
// operations. Submission early exits keep the frame-owned destructor backstop.
template<class Operations, class State, class Buffer>
ProcessFrameFlow ExecuteOverlayDraw(Operations& operations, State& state, DrawBackBuffer<Buffer>& backBuffer) {
    auto flow = operations.SelectCommands();
    if (flow != ProcessFrameFlow::kContinue) return flow;
    if (!operations.HasCommands()) {
        operations.ReportFailure(DrawFailure::MissingCommands, 0);
        return ProcessFrameFlow::kContinue;
    }
    flow = operations.ResetAllocator();
    if (flow != ProcessFrameFlow::kContinue) return flow;
    if (operations.AllocatorResetResult() < 0) {
        operations.ReportFailure(DrawFailure::AllocatorReset, operations.AllocatorResetResult());
        state.syncInit = false;
        return ProcessFrameFlow::kContinue;
    }
    operations.ResetCommandList();
    if (operations.CommandListResetResult() < 0) {
        operations.ReportFailure(DrawFailure::CommandListReset, operations.CommandListResetResult());
        state.syncInit = false;
        return ProcessFrameFlow::kContinue;
    }
    flow = operations.PrepareRecording();
    if (flow != ProcessFrameFlow::kContinue) return flow;
    if (!operations.HasSwapchain()) {
        operations.ReportFailure(DrawFailure::MissingSwapchain, 0);
        return ProcessFrameFlow::kContinue;
    }
    const auto acquired = operations.AcquireBackBuffer();
    backBuffer.Adopt(acquired.buffer);
    if (acquired.result < 0 || !backBuffer.Borrow()) {
        operations.ReportFailure(DrawFailure::GetBuffer, acquired.result);
        operations.RetireRenderTargets();
        state.overlayInit = false;
        backBuffer.Release();
        return ProcessFrameFlow::kContinue;
    }
    operations.RefreshRenderTarget(backBuffer.Borrow());
    flow = operations.RecordCommands();
    if (flow != ProcessFrameFlow::kContinue) return flow;
    const int32_t closeResult = operations.CloseCommands();
    if (closeResult < 0) {
        operations.ReportFailure(DrawFailure::Close, closeResult);
        state.syncInit = false;
    } else {
        flow = operations.SubmitCommands();
        if (flow != ProcessFrameFlow::kContinue) return flow;
    }
    // Preserve the original release before post-overlay capture/publication.
    backBuffer.Release();
    return ProcessFrameFlow::kContinue;
}
}  // namespace ce::dx12
