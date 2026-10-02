#pragma once

#include <windows.h>

#include <atomic>
#include <cstdint>

// CPU cycles a hook site burns inside the host process.
//
// Wall time is the wrong instrument for a hook that wraps a runtime call: a
// forwarded Present blocks for frame pacing the game would have paid anyway, so
// it looks expensive while costing the frame nothing, and a hook that merely
// spins looks cheap while costing a CPU-bound game everything.
// QueryThreadCycleTime excludes the time the thread was not running, so what it
// reports is the CPU the hook actually took from the game.
class HookCpuCost {
public:
    void Observe(uint64_t cycles, uint64_t wallUs) {
        cycles_.fetch_add(cycles, std::memory_order_relaxed);
        wallUs_.fetch_add(wallUs, std::memory_order_relaxed);
        calls_.fetch_add(1, std::memory_order_relaxed);
        uint64_t observed = maxCycles_.load(std::memory_order_relaxed);
        while (cycles > observed &&
               !maxCycles_.compare_exchange_weak(observed, cycles, std::memory_order_relaxed)) {
        }
        uint64_t observedWall = maxWallUs_.load(std::memory_order_relaxed);
        while (wallUs > observedWall &&
               !maxWallUs_.compare_exchange_weak(observedWall, wallUs, std::memory_order_relaxed)) {
        }
    }

    uint64_t calls() const { return calls_.load(std::memory_order_relaxed); }
    uint64_t cycles() const { return cycles_.load(std::memory_order_relaxed); }
    uint64_t maxCycles() const { return maxCycles_.load(std::memory_order_relaxed); }
    uint64_t averageCycles() const {
        const uint64_t count = calls();
        return count != 0 ? cycles() / count : 0;
    }
    // Wall microseconds the hook held the calling thread, forwarded call excluded.
    // Cycles cannot see a hook that BLOCKS: a thread waiting on a lock or a fence
    // burns no cycles while the GPU it feeds runs dry, which is how an injected
    // overlay can cost a frame-generation game milliseconds per frame while its
    // cycle share reads as a rounding error. Wall minus forwarded wall shows it.
    uint64_t wallUs() const { return wallUs_.load(std::memory_order_relaxed); }
    uint64_t maxWallUs() const { return maxWallUs_.load(std::memory_order_relaxed); }
    uint64_t averageWallUs() const {
        const uint64_t count = calls();
        return count != 0 ? wallUs() / count : 0;
    }

private:
    std::atomic<uint64_t> calls_{0};
    std::atomic<uint64_t> cycles_{0};
    std::atomic<uint64_t> maxCycles_{0};
    std::atomic<uint64_t> wallUs_{0};
    std::atomic<uint64_t> maxWallUs_{0};
};

inline int64_t HookQpcTicks() {
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return now.QuadPart;
}

// Microseconds per QPC tick, resolved once: the frequency is fixed for the boot.
inline double HookQpcTicksToUs() {
    static const double s_scale = [] {
        LARGE_INTEGER frequency;
        QueryPerformanceFrequency(&frequency);
        return frequency.QuadPart > 0 ? 1000000.0 / static_cast<double>(frequency.QuadPart) : 0.0;
    }();
    return s_scale;
}

// Two counter reads per hooked call is small but not free, and the hooks this
// wraps are the hottest in the process, so the measurement follows debug
// logging rather than running for every player. Resolved once: the log level
// cannot change within a process lifetime.
bool HookCpuCostMeasurementEnabled();

// Cycles the current thread spent inside the runtime call a hook forwards to.
// Those belong to the game whether CE is present or not, so they are measured
// separately and taken back off the hook's own total.
inline ULONG64& HookForwardedCycles() {
    static thread_local ULONG64 forwarded = 0;
    return forwarded;
}

// Wall ticks the same forwarded call held the thread, so a hook is never charged
// for the runtime's own pacing block.
inline int64_t& HookForwardedWallTicks() {
    static thread_local int64_t forwarded = 0;
    return forwarded;
}

// Cycles burned between construction and destruction on the calling thread,
// excluding anything a nested ScopedHookForwardedCall attributed to the runtime.
class ScopedHookCpuCost {
public:
    explicit ScopedHookCpuCost(HookCpuCost& sink)
        : ScopedHookCpuCost(sink, HookCpuCostMeasurementEnabled()) {}

    // The explicit form exists so the accounting can be exercised without
    // depending on the host process's log level.
    ScopedHookCpuCost(HookCpuCost& sink, bool enabled)
        : sink_(sink), savedForwarded_(HookForwardedCycles()), savedForwardedWall_(HookForwardedWallTicks()) {
        if (!enabled) {
            return;
        }
        HookForwardedCycles() = 0;
        HookForwardedWallTicks() = 0;
        if (!QueryThreadCycleTime(GetCurrentThread(), &enterCycles_)) {
            enterCycles_ = 0;
            return;
        }
        enterTicks_ = HookQpcTicks();
    }

    ~ScopedHookCpuCost() {
        ULONG64 exitCycles = 0;
        if (enterCycles_ != 0 && QueryThreadCycleTime(GetCurrentThread(), &exitCycles) &&
            exitCycles > enterCycles_) {
            const ULONG64 total = exitCycles - enterCycles_;
            const ULONG64 forwarded = HookForwardedCycles();
            const int64_t totalTicks = HookQpcTicks() - enterTicks_;
            const int64_t forwardedTicks = HookForwardedWallTicks();
            const int64_t ownTicks = totalTicks > forwardedTicks ? totalTicks - forwardedTicks : 0;
            sink_.Observe(total > forwarded ? total - forwarded : 0,
                          static_cast<uint64_t>(static_cast<double>(ownTicks) * HookQpcTicksToUs()));
        }
        // A nested hook must not steal this scope's forwarded cycles from the
        // outer one, so the previous scope's tally is restored rather than lost.
        HookForwardedCycles() = savedForwarded_;
        HookForwardedWallTicks() = savedForwardedWall_;
    }

    ScopedHookCpuCost(const ScopedHookCpuCost&) = delete;
    ScopedHookCpuCost& operator=(const ScopedHookCpuCost&) = delete;

private:
    HookCpuCost& sink_;
    ULONG64 savedForwarded_ = 0;
    int64_t savedForwardedWall_ = 0;
    ULONG64 enterCycles_ = 0;
    int64_t enterTicks_ = 0;
};

// Wrap the forwarded runtime call itself.
class ScopedHookForwardedCall {
public:
    ScopedHookForwardedCall() : ScopedHookForwardedCall(HookCpuCostMeasurementEnabled()) {}

    explicit ScopedHookForwardedCall(bool enabled) {
        if (!enabled || !QueryThreadCycleTime(GetCurrentThread(), &enterCycles_)) {
            enterCycles_ = 0;
            return;
        }
        enterTicks_ = HookQpcTicks();
    }

    ~ScopedHookForwardedCall() {
        ULONG64 exitCycles = 0;
        if (enterCycles_ != 0 && QueryThreadCycleTime(GetCurrentThread(), &exitCycles) &&
            exitCycles > enterCycles_) {
            HookForwardedCycles() += exitCycles - enterCycles_;
            HookForwardedWallTicks() += HookQpcTicks() - enterTicks_;
        }
    }

    ScopedHookForwardedCall(const ScopedHookForwardedCall&) = delete;
    ScopedHookForwardedCall& operator=(const ScopedHookForwardedCall&) = delete;

private:
    ULONG64 enterCycles_ = 0;
    int64_t enterTicks_ = 0;
};

HookCpuCost& HookPresentCpuCost();
HookCpuCost& HookExecuteCommandListsCpuCost();
void ReportHookCpuCostIfDue();
