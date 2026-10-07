#include "frame_submission_internal.h"

#include "common/capture/frame_queue.h"
#include "captureengine/app/mediaengine_loader.h"

namespace ce::media::submission {
namespace {
FrameSubmissionResultV1 Submit(const D3D11FrameSubmissionDesc& desc) {
    return detail::Invoke("screen-grab", true, [&](FrameSubmissionResultV1* result) {
        return MediaEngine_SubmitFrameD3D11WithResultV1 && MediaEngine_SubmitFrameD3D11WithResultV1(&desc, result);
    });
}
}  // namespace

FrameSubmissionResultV1 ScreenGrab(const QueuedFrame& frame, int64_t mediaTimestampQpc,
                                    int64_t timelineElapsedUs, const ce::cursor::CaptureState* cursor) {
    return Submit({frame.texture, mediaTimestampQpc, frame.width, frame.height, frame.isHDR,
                   frame.captureLeft, frame.captureTop, timelineElapsedUs, cursor});
}

FrameSubmissionResultV1 Black(const BlackFrameSource& frame, int64_t mediaTimestampQpc,
                               int64_t timelineElapsedUs, const ce::cursor::CaptureState* hiddenCursor) {
    return Submit({frame.texture, mediaTimestampQpc, frame.width, frame.height, frame.isHdr,
                   0, 0, timelineElapsedUs, hiddenCursor});
}

FrameSubmissionResultV1 Repeat(int64_t scheduledQpc, int64_t timelineElapsedUs,
                                const ce::cursor::CaptureState* cursor) {
    return detail::Invoke("repeat", false, [&](FrameSubmissionResultV1* result) {
        return MediaEngine_RepeatLastFrameWithResultV1 &&
               MediaEngine_RepeatLastFrameWithResultV1(scheduledQpc, timelineElapsedUs, cursor, result);
    });
}

AcceptedOutputCounts GetAcceptedOutputCounts() {
    return {detail::AcceptedFreshOutputs().load(std::memory_order_relaxed),
            detail::AcceptedTotalOutputs().load(std::memory_order_relaxed)};
}

}  // namespace ce::media::submission
