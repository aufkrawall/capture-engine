#pragma once

#include <cstdint>
#include <mutex>

namespace ce::resident_worker {

// One background thread of the resident hook or layer (system metrics, freeze watchdog) across host
// generations.
//
// A host stop makes the module dormant: its entry points pass through and its workers stop, so each
// worker's loop ends once it observes dormancy. A replacement host later reactivates the same image, and
// the worker must run again - otherwise the game keeps a hook that publishes no adapter LUID and watches
// no freeze for the rest of its life (session 20261025_212736: adapterLuid=0x0 for 15 minutes after the
// reconnect). Lifecycle decides, under one lock, whether a run that ended was parked by dormancy (resumed
// by BeginResume), stopped by its owner, or finished on its own (a one-shot exit, never resumed), and hands
// out a generation so a late-exiting thread can never overwrite the state of the run that replaced it.
//
// The owner keeps the std::thread: after BeginRun/BeginResume returned a generation it joins the previous
// thread and launches a new one that loops on Continue(generation) and calls EndRun(generation) last.
class Lifecycle {
public:
    using DormancyProbe = bool (*)();

    explicit Lifecycle(DormancyProbe isDormant) noexcept : isDormant_(isDormant) {}
    Lifecycle(const Lifecycle&) = delete;
    Lifecycle& operator=(const Lifecycle&) = delete;

    // Owner start path. The new run's generation, or 0 while a run is active.
    uint64_t BeginRun() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (state_ == State::kRunning)
            return 0;
        return StartLocked();
    }

    // Reactivation path; call only after the module left dormancy. The new run's generation for a worker
    // its own loop parked on dormancy, else 0: a worker that never noticed the dormant window is still
    // running, and a stopped or finished one stays down.
    uint64_t BeginResume() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (state_ != State::kParked)
            return 0;
        return StartLocked();
    }

    // The worker loop's condition. False ends the run: a stop request, a superseded generation, or
    // dormancy, which parks the worker for BeginResume. Dormancy is read under the lock, so a resume
    // that cleared it before calling BeginResume either finds this run parked or lets it continue.
    bool Continue(uint64_t run) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (run != generation_ || state_ != State::kRunning)
            return false;
        if (isDormant_ && isDormant_()) {
            state_ = State::kParked;
            return false;
        }
        return true;
    }

    // The worker's thread function returned. A run its loop did not end through Continue() exited on
    // its own and is finished.
    void EndRun(uint64_t run) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (run == generation_ && state_ == State::kRunning)
            state_ = State::kFinished;
    }

    // Owner stop path: the active run ends at its next Continue() and is never resumed.
    void RequestStop() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (state_ == State::kRunning || state_ == State::kParked)
            state_ = State::kStopped;
    }

    bool IsRunning() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return state_ == State::kRunning;
    }

    bool IsParked() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return state_ == State::kParked;
    }

private:
    enum class State : uint8_t { kIdle, kRunning, kParked, kStopped, kFinished };

    uint64_t StartLocked() {
        state_ = State::kRunning;
        return ++generation_;
    }

    mutable std::mutex mutex_;
    DormancyProbe isDormant_;
    State state_ = State::kIdle;
    uint64_t generation_ = 0;
};

}  // namespace ce::resident_worker
