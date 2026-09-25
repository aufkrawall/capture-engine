#pragma once

#include <windows.h>

#include <cstdint>

// Remembers that a foreign JMP target on dxgi!Present was already judged not to be Streamline's.
//
// DetectSLPresentHook runs on every Present until Streamline routing activates. With another tool's
// jump on the Present entry (GTA session 20260925_233000: an EOS/overlay thunk outside any module),
// it resolved that target's module on every call, on the game thread and on AMD's frame-generation
// presenter thread alike - 15,000 loader lookups in five minutes, the only steady misses left in the
// module identity cache, because an address outside any module cannot be cached. The verdict depends
// only on the target and on which modules are loaded, so it holds until either changes.

namespace ce::present_hook_target {

class RejectedTargetMemo {
public:
    bool IsKnownRejected(const void* target, std::uint64_t moduleSetGeneration) const {
        AcquireSRWLockShared(&lock_);
        const bool known = valid_ && target_ == target && generation_ == moduleSetGeneration;
        ReleaseSRWLockShared(&lock_);
        return known;
    }

    void RememberRejected(const void* target, std::uint64_t moduleSetGeneration) {
        AcquireSRWLockExclusive(&lock_);
        target_ = target;
        generation_ = moduleSetGeneration;
        valid_ = target != nullptr;
        ReleaseSRWLockExclusive(&lock_);
    }

private:
    mutable SRWLOCK lock_ = SRWLOCK_INIT;
    const void* target_ = nullptr;
    std::uint64_t generation_ = 0;
    bool valid_ = false;
};

}  // namespace ce::present_hook_target
