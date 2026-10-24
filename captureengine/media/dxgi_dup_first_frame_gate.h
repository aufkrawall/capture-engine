#pragma once

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>

// First-frame handover between the DXGI duplication thread and its owner.
// The duplication's ModeDesc only hints the format; the first acquired texture
// proves it. The thread proves that frame and then holds it until the owner has
// adopted the proven format (retained route, budget, pool) and opens delivery,
// so the owner and the thread never build the pool at the same time and the
// first frame is still delivered. Close (Stop) releases every waiter.
class DuplicationFirstFrameGate {
public:
    // Owner, before the thread starts.
    void Reset() {
        std::lock_guard<std::mutex> lock(mutex_);
        proof_ = Proof::Pending;
        failureReason_.clear();
        deliveryOpen_ = false;
        closed_ = false;
    }

    // Thread: the first frame is acceptable.
    void Prove() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (proof_ == Proof::Pending)
                proof_ = Proof::Valid;
        }
        cv_.notify_all();
    }

    // Thread: no acceptable first frame. Ignored once the frame is proven.
    void Fail(const std::string& reason) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (proof_ != Proof::Pending)
                return;
            proof_ = Proof::Invalid;
            failureReason_ = reason;
        }
        cv_.notify_all();
    }

    // Owner: true once the first frame is proven; false when it failed, the
    // gate was closed or `timeout` passed (FailureReason says which).
    bool WaitForProof(std::chrono::milliseconds timeout) {
        std::unique_lock<std::mutex> lock(mutex_);
        const bool decided = cv_.wait_for(lock, timeout, [&] { return proof_ != Proof::Pending || closed_; });
        if (proof_ == Proof::Valid && !closed_)
            return true;
        if (failureReason_.empty()) {
            failureReason_ = !decided ? "timed out waiting for first duplication frame format proof"
                                      : "capture thread stopped";
        }
        return false;
    }

    // Owner: the proven format is adopted; the held first frame may go out.
    void OpenDelivery() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            deliveryOpen_ = true;
        }
        cv_.notify_all();
    }

    // Thread, after Prove: blocks until OpenDelivery (true) or Close (false).
    bool WaitForDelivery() {
        std::unique_lock<std::mutex> lock(mutex_);
        cv_.wait(lock, [&] { return deliveryOpen_ || closed_; });
        return deliveryOpen_ && !closed_;
    }

    // Stop: nothing is delivered afterwards and every waiter returns.
    void Close() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            closed_ = true;
        }
        cv_.notify_all();
    }

    std::string FailureReason() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return failureReason_;
    }

private:
    enum class Proof : uint8_t { Pending, Valid, Invalid };
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    Proof proof_ = Proof::Pending;
    std::string failureReason_;
    bool deliveryOpen_ = false;
    bool closed_ = false;
};
