#pragma once

namespace ce::inline_hook_policy {

inline constexpr int kExternalPrependPatchSize = 5;

inline bool IsPrependChainableEntryJump(unsigned char firstByte, unsigned char secondByte, bool is64Bit) {
    return firstByte == 0xE9 || (is64Bit && firstByte == 0xFF && secondByte == 0x25);
}

inline bool ShouldRestoreOwnedPatch(bool liveBytesMatchInstalledBytes) {
    return liveBytesMatchInstalledBytes;
}

inline bool IsVerifiedExternalHookResumeOffset(int resumeOffset, int externalJumpSize, bool liveBytesMatchDisk) {
    return resumeOffset >= externalJumpSize && liveBytesMatchDisk;
}

inline bool ShouldExtendExternalHookResumeOffset(int firstCandidateOffset, int selectedOffset,
                                                 bool selectedLiveBytesMatchDisk) {
    return selectedLiveBytesMatchDisk && firstCandidateOffset > 0 && selectedOffset > firstCandidateOffset;
}

}  // namespace ce::inline_hook_policy
