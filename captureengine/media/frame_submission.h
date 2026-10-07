#pragma once

#include "mediaengine/engine/frame_submission_result.h"

struct QueuedFrame;
struct ID3D11Texture2D;
namespace ce::cursor { struct CaptureState; }

namespace ce::media::submission {

// Synchronous source adapters borrow input and cursor state. They never release or promote a lease.
FrameSubmissionResultV1 Inject(const QueuedFrame& frame, const ce::cursor::CaptureState* cursor);
FrameSubmissionResultV1 ScreenGrab(const QueuedFrame& frame, int64_t mediaTimestampQpc,
                                    int64_t timelineElapsedUs, const ce::cursor::CaptureState* cursor);

struct BlackFrameSource {
    ID3D11Texture2D* texture;
    uint32_t width;
    uint32_t height;
    bool isHdr;
};
FrameSubmissionResultV1 Black(const BlackFrameSource& frame, int64_t mediaTimestampQpc,
                               int64_t timelineElapsedUs, const ce::cursor::CaptureState* hiddenCursor);
FrameSubmissionResultV1 Repeat(int64_t scheduledQpc, int64_t timelineElapsedUs,
                                const ce::cursor::CaptureState* cursor);

// Accepted video outputs since process start, counted at the one place every adapter's
// result passes. The mux byte budget measures the fresh share of the bytes it paces.
struct AcceptedOutputCounts {
    uint64_t fresh = 0;
    uint64_t total = 0;
};
AcceptedOutputCounts GetAcceptedOutputCounts();

}  // namespace ce::media::submission
