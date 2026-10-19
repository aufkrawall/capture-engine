#pragma once

#include <atomic>
#include <functional>
#include <optional>
#include <thread>
#include <utility>

namespace ce::startup {

// One setup action in flight on a dedicated thread. The tray and the future setup API share the
// model: a request is accepted only while idle, runs to completion off the caller's thread, and its
// Win32 result is delivered exactly once. "Busy" spans the request until the result was taken, so a
// poller (the controller loop today, the runtime's event pump later) can never miss a completion.
class SetupWorker {
public:
    SetupWorker() = default;
    SetupWorker(const SetupWorker&) = delete;
    SetupWorker& operator=(const SetupWorker&) = delete;
    ~SetupWorker() {
        Shutdown();
    }

    // False when an earlier request is still running or its result was not taken yet.
    bool Start(std::function<unsigned long()> work) {
        if (Busy())
            return false;
        if (thread_.joinable())
            thread_.join();
        busy_.store(true, std::memory_order_release);
        complete_.store(false, std::memory_order_release);
        try {
            thread_ = std::thread([this, work = std::move(work)] {
                unsigned long result = kNotEnoughMemory;
                try {
                    result = work();
                } catch (...) {
                }
                result_.store(result, std::memory_order_relaxed);
                complete_.store(true, std::memory_order_release);
            });
        } catch (...) {
            result_.store(kNotEnoughMemory, std::memory_order_relaxed);
            complete_.store(true, std::memory_order_release);
        }
        return true;
    }

    bool Busy() const {
        return busy_.load(std::memory_order_acquire);
    }

    // Non-blocking: the result of a finished request, once, or nothing while it still runs.
    std::optional<unsigned long> TakeResult() {
        if (!complete_.exchange(false, std::memory_order_acq_rel))
            return std::nullopt;
        return Finish();
    }

    // Blocking: waits for the running request and delivers its result; nothing when idle.
    std::optional<unsigned long> WaitForResult() {
        if (!Busy())
            return std::nullopt;
        if (thread_.joinable())
            thread_.join();
        complete_.store(false, std::memory_order_release);
        return Finish();
    }

    // Joins the running request and drops an undelivered result.
    void Shutdown() {
        if (thread_.joinable())
            thread_.join();
        complete_.store(false);
        busy_.store(false);
    }

private:
    static constexpr unsigned long kNotEnoughMemory = 8;  // ERROR_NOT_ENOUGH_MEMORY

    unsigned long Finish() {
        if (thread_.joinable())
            thread_.join();
        busy_.store(false, std::memory_order_release);
        return result_.load(std::memory_order_relaxed);
    }

    std::atomic<bool> busy_{false};
    std::atomic<bool> complete_{false};
    std::atomic<unsigned long> result_{0};
    std::thread thread_;
};

}  // namespace ce::startup
