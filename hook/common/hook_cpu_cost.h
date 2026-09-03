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
    void Observe(uint64_t cycles) {
        cycles_.fetch_add(cycles, std::memory_order_relaxed);
        calls_.fetch_add(1, std::memory_order_relaxed);
        uint64_t observed = maxCycles_.load(std::memory_order_relaxed);
        while (cycles > observed &&
               !maxCycles_.compare_exchange_weak(observed, cycles, std::memory_order_relaxed)) {
        }
    }

    uint64_t calls() const { return calls_.load(std::memory_order_relaxed); }
    uint64_t cycles() const { return cycles_.load(std::memory_order_relaxed); }
    uint64_t maxCycles() const { return maxCycles_.load(std::memory_order_relaxed); }
    uint64_t averageCycles() const {
        const uint64_t count = calls();
        return count != 0 ? cycles() / count : 0;
    }

private:
    std::atomic<uint64_t> calls_{0};
    std::atomic<uint64_t> cycles_{0};
    std::atomic<uint64_t> maxCycles_{0};
};

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

// Cycles burned between construction and destruction on the calling thread,
// excluding anything a nested ScopedHookForwardedCall attributed to the runtime.
class ScopedHookCpuCost {
public:
    explicit ScopedHookCpuCost(HookCpuCost& sink)
        : ScopedHookCpuCost(sink, HookCpuCostMeasurementEnabled()) {}

    // The explicit form exists so the accounting can be exercised without
    // depending on the host process's log level.
    ScopedHookCpuCost(HookCpuCost& sink, bool enabled)
        : sink_(sink), savedForwarded_(HookForwardedCycles()) {
        if (!enabled) {
            return;
        }
        HookForwardedCycles() = 0;
        if (!QueryThreadCycleTime(GetCurrentThread(), &enterCycles_)) {
            enterCycles_ = 0;
        }
    }

    ~ScopedHookCpuCost() {
        ULONG64 exitCycles = 0;
        if (enterCycles_ != 0 && QueryThreadCycleTime(GetCurrentThread(), &exitCycles) &&
            exitCycles > enterCycles_) {
            const ULONG64 total = exitCycles - enterCycles_;
            const ULONG64 forwarded = HookForwardedCycles();
            sink_.Observe(total > forwarded ? total - forwarded : 0);
        }
        // A nested hook must not steal this scope's forwarded cycles from the
        // outer one, so the previous scope's tally is restored rather than lost.
        HookForwardedCycles() = savedForwarded_;
    }

    ScopedHookCpuCost(const ScopedHookCpuCost&) = delete;
    ScopedHookCpuCost& operator=(const ScopedHookCpuCost&) = delete;

private:
    HookCpuCost& sink_;
    ULONG64 savedForwarded_ = 0;
    ULONG64 enterCycles_ = 0;
};

// Wrap the forwarded runtime call itself.
class ScopedHookForwardedCall {
public:
    ScopedHookForwardedCall() : ScopedHookForwardedCall(HookCpuCostMeasurementEnabled()) {}

    explicit ScopedHookForwardedCall(bool enabled) {
        if (!enabled || !QueryThreadCycleTime(GetCurrentThread(), &enterCycles_)) {
            enterCycles_ = 0;
        }
    }

    ~ScopedHookForwardedCall() {
        ULONG64 exitCycles = 0;
        if (enterCycles_ != 0 && QueryThreadCycleTime(GetCurrentThread(), &exitCycles) &&
            exitCycles > enterCycles_) {
            HookForwardedCycles() += exitCycles - enterCycles_;
        }
    }

    ScopedHookForwardedCall(const ScopedHookForwardedCall&) = delete;
    ScopedHookForwardedCall& operator=(const ScopedHookForwardedCall&) = delete;

private:
    ULONG64 enterCycles_ = 0;
};

HookCpuCost& HookPresentCpuCost();
HookCpuCost& HookExecuteCommandListsCpuCost();
void ReportHookCpuCostIfDue();
