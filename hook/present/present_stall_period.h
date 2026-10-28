#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <vector>

// Whether slow D3D12 down-calls follow a clock outside the game.
//
// Heroes of the Storm's inject-recording stall (llm-wiki/log/recent.md,
// 2026-10-28) took a day of CE-side hypotheses until the slow calls' times
// showed they were phase-locked to exactly 40 ms: the face camera's built-in
// USB microphone, which flagged a WASAPI data discontinuity every 40 ms while
// the recording held it open. Nothing in the frame's own work runs on such a
// clock, so a lock names an outside source (a device, a driver, another
// process's timer) at once, and its period says which.
//
// The test is Rayleigh's: the slow calls' entry times folded onto a candidate
// period give a resultant length R (1 = all at one phase, ~0 = spread); Z =
// n R^2 tells a lock from chance across the sweep. A period of the game's own
// is not an outside clock: one that every call folds onto as well (control R),
// or a whole number of calls long (a stall every Nth frame, where every call
// spreads evenly and the control cannot see it). Events sit on the frame grid,
// so a true frequency also shows at its aliases against the frame rate and at
// its harmonics; the line lists the strongest distinct peaks.
namespace ce::present_stall {

inline constexpr double kPeriodLockMinHz = 10.0;   // 100 ms
inline constexpr double kPeriodLockMaxHz = 100.0;  // 10 ms
inline constexpr uint32_t kPeriodLockMinSlowCalls = 12;
inline constexpr double kPeriodLockMinZ = 16.0;          // chance over the whole sweep stays < ~1e-3
inline constexpr double kPeriodLockMaxControlR = 0.1;     // above it every call shares the period
inline constexpr double kPeriodLockWholeCallsSlack = 0.05;  // calls per period this close to an integer
inline constexpr size_t kPeriodLockMaxSlowCalls = 256;
inline constexpr size_t kPeriodLockPeaks = 3;

struct PeriodPeak {
    double periodMs = 0;
    double r = 0;               // slow calls
    double controlR = 0;        // every call
    double callsPerPeriod = 0;  // period over the median call interval
    bool gameCadence = false;   // the control or a whole number of calls says it is the game's own
};

struct PeriodLock {
    uint32_t calls = 0;
    uint32_t slowCalls = 0;  // folded (at most kPeriodLockMaxSlowCalls, evenly thinned)
    bool analyzed = false;   // enough slow calls to test
    bool locked = false;     // peaks[0] is an outside clock and passes Z
    double z = 0;            // of peaks[0]
    size_t peakCount = 0;
    std::array<PeriodPeak, kPeriodLockPeaks> peaks{};  // strongest distinct first
};

// Times in microseconds on one clock, ascending.
PeriodLock FindPeriodLock(const std::vector<int64_t>& callUs, const std::vector<int64_t>& slowUs);

// Down-call entry times between two drains. Present threads note (a second
// thread arriving while one notes skips its call); the hook service thread
// drains. A drain that finds the ring lapped keeps what was not overwritten.
class PeriodRecorder {
public:
    static constexpr size_t kCapacity = 4096;  // ~29 s at 140 fps: two windows of headroom

    void Note(int64_t entryUs, bool slow);
    // Appends to `callUs` and `slowUs`; returns the calls lost to a lap.
    uint64_t Drain(std::vector<int64_t>& callUs, std::vector<int64_t>& slowUs);

private:
    std::array<std::atomic<int64_t>, kCapacity> events_{};  // entryUs << 1 | slow
    std::atomic<uint64_t> written_{0};
    std::atomic_flag writing_ = ATOMIC_FLAG_INIT;
    uint64_t read_ = 0;  // service thread
};

// Returns the number of characters written.
int FormatPeriodLock(const PeriodLock& lock, uint64_t windowMs, uint64_t lapped, char* buffer, size_t bufferSize);

// Production wiring (present_stall_period.cpp).
PeriodRecorder& PresentPeriodRecorder();

}  // namespace ce::present_stall
