#pragma once

#include "frame_submission.h"

namespace ce::media::submission::detail {

// Complete the caller-owned fresh candidate, independently of the frame used for recovered output.
// Statically bound callbacks keep the existing source-specific release and cache-transfer operations.
template <typename Promote, typename Discard>
void CompleteCandidate(const FrameSubmissionResultV1& result, bool originalAccepted,
                       Promote&& promote, Discard&& discard) {
    if (result.source == SourceDisposition::RetainForRetry)
        return;
    if (result.Accepted() && originalAccepted && result.output == SubmissionOutput::FreshSource)
        promote();
    else
        discard(result.Accepted());
}

}  // namespace ce::media::submission::detail
