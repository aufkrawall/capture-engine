#include "frame_submission_internal.h"

#include "common/capture/frame_queue.h"
#include "captureengine/app/mediaengine_loader.h"

namespace ce::media::submission {

FrameSubmissionResultV1 Inject(const QueuedFrame& frame, const ce::cursor::CaptureState* cursor) {
    if (!frame.isShmem && ce::mediaengine_api::MediaEngine_SetInjectTransportGeneration)
        ce::mediaengine_api::MediaEngine_SetInjectTransportGeneration(MediaState().engine, frame.transportGeneration);
    const VideoFrameSubmissionDesc desc{
        reinterpret_cast<uint64_t>(frame.sharedHandle), reinterpret_cast<uint64_t>(frame.fenceHandle),
        frame.fenceValue, frame.timestamp, frame.luidLow, frame.luidHigh, frame.sourcePid,
        frame.width, frame.height, frame.format, frame.isHDR, frame.isShmem,
        static_cast<int>(frame.shmemSlot), cursor};
    return detail::Invoke("inject", true, [&](FrameSubmissionResultV1* result) {
        return ce::mediaengine_api::MediaEngine_SubmitFrameWithResultV1 &&
            ce::mediaengine_api::MediaEngine_SubmitFrameWithResultV1(MediaState().engine,
                                                                                          &desc, result);
    });
}

}  // namespace ce::media::submission
