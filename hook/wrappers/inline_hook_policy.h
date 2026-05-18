#pragma once

namespace ce::inline_hook_policy {

inline bool IsVerifiedExternalHookResumeOffset(int resumeOffset, int externalJumpSize, bool liveBytesMatchDisk) {
    return resumeOffset >= externalJumpSize && liveBytesMatchDisk;
}

inline bool ShouldExtendExternalHookResumeOffset(int firstCandidateOffset, int selectedOffset,
                                                 bool selectedLiveBytesMatchDisk) {
    return selectedLiveBytesMatchDisk && firstCandidateOffset > 0 && selectedOffset > firstCandidateOffset;
}

}  // namespace ce::inline_hook_policy
