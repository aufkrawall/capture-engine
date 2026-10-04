#pragma once

#include "frame_submission_result.h"

namespace ce::media::detail {

// Statically bound production orchestration. Only acceptance admits anchor/timeline/audio commit.
// The deferred observation occurs under the same existing engine lock as Encode, never via a DLL query.
template <typename Encode, typename Deferred, typename Commit>
FrameSubmissionResultV1 SubmitAndCommit(bool candidate, Encode&& encode, Deferred&& deferred, Commit&& commit) {
    if (!encode())
        return UnacceptedSubmission(deferred(), candidate);
    return commit();
}

// Invalid output storage is rejected before invoking the engine, and remains untouched.
template <typename Submit>
bool PublishSubmissionResult(FrameSubmissionResultV1* output, Submit&& submit) {
    if (!output || output->size != sizeof(FrameSubmissionResultV1))
        return false;
    *output = submit();
    return true;
}

}  // namespace ce::media::detail
