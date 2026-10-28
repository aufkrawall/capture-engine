#pragma once

#include <windows.h>

#include <atomic>
#include <cstddef>
#include <cstdint>

// What a stalled D3D12 Present down-call is blocked in.
//
// Flip timing (present_flip_timing.h) showed the HotS stall ends right before
// the frame's kernel submit: DXGI's Present starts with the call, then 2-5 ms
// pass inside the call before the present reaches the kernel
// (llm-wiki/log/recent.md, 2026-10-28). Which user-mode layer holds it (DXGI,
// the D3D12 runtime, the driver, a lock or a kernel call) only the presenting
// thread's stack says.
//
// A high-resolution timer is armed at the down-call's entry while inject
// capture runs. When it fires and the call is still inside, a sampler thread
// suspends the presenting thread only to copy its registers and stack, resumes
// it, and walks the copy afterwards: no lock, allocation or log write happens
// while the game's thread is held. Frames are logged as module+offset to be
// symbolized offline. Rate-limited by StackSampleBudget; x64 only.
namespace ce::present_stall {

inline constexpr int64_t kStackSampleAfterUs = 1500;
inline constexpr size_t kStackSampleFrames = 24;

// The first kBurst samples, then one per kIntervalMs.
class StackSampleBudget {
public:
    static constexpr uint32_t kBurst = 8;
    static constexpr uint64_t kIntervalMs = 10'000;

    bool Allows(uint64_t nowMs) const;
    void Taken(uint64_t nowMs);
    uint32_t taken() const { return taken_.load(std::memory_order_relaxed); }

private:
    std::atomic<uint32_t> taken_{0};
    std::atomic<uint64_t> lastMs_{0};
};

// Registers and the top of the stack of a suspended thread. The buffer is
// committed once; slack past the copy absorbs an unwind step that reads
// beyond it before the walk notices it left the copied range.
class StackCopy {
public:
    static constexpr size_t kBytes = 256 * 1024;
    static constexpr size_t kSlack = 128 * 1024;

    StackCopy();
    ~StackCopy();
    StackCopy(const StackCopy&) = delete;
    StackCopy& operator=(const StackCopy&) = delete;

    bool valid() const { return data_ != nullptr; }
    const CONTEXT& context() const { return context_; }
    uintptr_t low() const { return low_; }  // the copied stack's address in the sampled thread
    size_t bytes() const { return bytes_; }
    const uint8_t* data() const { return data_; }

    // Suspends `thread`, copies its context and the stack from its stack
    // pointer up (at most kBytes), resumes it. `stillWanted` runs while the
    // thread is suspended (it must not lock or allocate); false discards the
    // copy. False when nothing was copied. x64 only.
    bool CopySuspended(HANDLE thread, bool (*stillWanted)(void*), void* user);

private:
    CONTEXT context_{};
    uintptr_t low_ = 0;
    size_t bytes_ = 0;
    uint8_t* data_ = nullptr;
};

// Walks the copy from its captured context. Registers that point into the
// copied range are rebased onto the copy after every step, so the walk never
// reads the live stack. Returns the frame count, instruction pointer first.
size_t UnwindStackCopy(const StackCopy& copy, uint64_t* frames, size_t maxFrames);

// " module+0xoffset" per frame (" 0x..." when no module holds it).
int FormatStackFrames(const uint64_t* frames, size_t count, char* buffer, size_t bufferSize);

// Production wiring (present_stall_stack.cpp), present thread. Arm at the
// down-call's entry while capture runs; Disarm right after it returns.
void ArmStackSample();
void DisarmStackSample();

}  // namespace ce::present_stall
