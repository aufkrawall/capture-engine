#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>

// A D3D12 submit blocks for 12-32 ms once after a new chain's first Present, and after the game regains focus, whoever
// submits: CE's overlay list (21.9 ms, 20261104_222523; 30.8 ms and 25.0 ms after focus, 20261104_221303), and the
// ExecuteCommandLists inside Present's own down-call while CE submits nothing (25.7 ms, 20261104_210842; 31.7 ms,
// 20261104_183540). So it is not the cost of CE's list, and leaving CE's submit out would move it, not remove it. What
// it waits on only the thread's stack can say, so the calls right after such a change get theirs sampled when they
// block (present_stall_stack.h), with no capture running as well.
namespace ce::present_stall {

class PresentationChangeWatch {
public:
    // Calls, not presents: a Present makes one down-call and at most one overlay submit, and the stall has always hit
    // within the first three presents.
    static constexpr uint32_t kCalls = 24;

    void Note(const char* why, int64_t atTicks) {
        why_.store(why, std::memory_order_relaxed);
        atTicks_.store(atTicks, std::memory_order_relaxed);
        remaining_.store(kCalls, std::memory_order_release);
    }
    // True while a call is left in the window after a change, and uses it up.
    bool TakeCall() {
        uint32_t remaining = remaining_.load(std::memory_order_acquire);
        while (remaining != 0 && !remaining_.compare_exchange_weak(remaining, remaining - 1, std::memory_order_acq_rel,
                                                                   std::memory_order_acquire)) {
        }
        return remaining != 0;
    }
    const char* Why() const { return why_.load(std::memory_order_relaxed); }  // null before the first change
    int64_t AtTicks() const { return atTicks_.load(std::memory_order_relaxed); }

private:
    std::atomic<const char*> why_{nullptr};
    std::atomic<int64_t> atTicks_{0};
    std::atomic<uint32_t> remaining_{0};
};

PresentationChangeWatch& PresentationChange();

// `why` is a string literal. Production wiring (present_stall_attribution.cpp).
void NotePresentationChange(const char* why);

inline constexpr uint64_t kSlowOverlaySubmitUs = 2000;

// `change` null: no presentation change yet, and `sinceChangeMs` means nothing.
int FormatSlowOverlaySubmit(double durationMs, const char* reason, bool sampled, const char* change, double sinceChangeMs,
                            char* buffer, size_t bufferSize);

// Brackets one of CE's overlay submits: a slow one logs what it was and how soon after a presentation change, because a
// submit through the raw D3D12 entry passes no ExecuteCommandLists detour and logged nothing (20261104_222523).
class OverlaySubmitProbe {
public:
    explicit OverlaySubmitProbe(const char* reason);
    ~OverlaySubmitProbe();
    OverlaySubmitProbe(const OverlaySubmitProbe&) = delete;
    OverlaySubmitProbe& operator=(const OverlaySubmitProbe&) = delete;

private:
    const char* reason_;
    int64_t startTicks_;
    uint64_t stackCall_ = 0;
};

}  // namespace ce::present_stall
