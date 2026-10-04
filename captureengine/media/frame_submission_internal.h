#pragma once

#include "frame_submission.h"
#include "common/logging/logging.h"
#include "common/logging/log_meter.h"

namespace ce::media::submission::detail {

template <typename Submit>
FrameSubmissionResultV1 Invoke(const char* source, bool candidate, Submit&& submit) {
    FrameSubmissionResultV1 result;
    if (!candidate)
        result.source = SourceDisposition::NoCandidate;
    if (!submit(&result)) {
        static ce::log_meter::ChangeGate gate;
        const auto verdict = gate.Observe(1);
        if (verdict)
            LogError("[MediaSubmission] %s result contract rejected (+%llu unchanged)", source,
                     static_cast<unsigned long long>(verdict.suppressed));
        result = FrameSubmissionResultV1{};
        if (!candidate)
            result.source = SourceDisposition::NoCandidate;
    }
    return result;
}

}  // namespace ce::media::submission::detail
