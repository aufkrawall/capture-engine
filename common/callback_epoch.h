#pragma once

#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>

namespace ce {

// Lifetime gate for callbacks dispatched by an external queue. A queued
// callback retains the gate instead of its owner. Enter() succeeds only for
// the currently active epoch and returns a lease that keeps the owner valid
// until the callback leaves. StopAndDrain() invalidates callbacks that have
// not entered yet and waits without polling for callbacks already in flight.
//
// The owner must call DetachAndDrain() before it is destroyed. The gate can
// safely outlive the owner because stale callbacks only touch gate state.
template <typename Owner>
class CallbackEpoch final : public std::enable_shared_from_this<CallbackEpoch<Owner>> {
public:
    class Lease final {
    public:
        Lease() = default;
        Lease(const Lease&) = delete;
        Lease& operator=(const Lease&) = delete;

        Lease(Lease&& other) noexcept : gate_(std::move(other.gate_)), owner_(other.owner_) {
            other.owner_ = nullptr;
        }

        Lease& operator=(Lease&& other) noexcept {
            if (this != &other) {
                Reset();
                gate_ = std::move(other.gate_);
                owner_ = other.owner_;
                other.owner_ = nullptr;
            }
            return *this;
        }

        ~Lease() {
            Reset();
        }

        explicit operator bool() const {
            return owner_ != nullptr;
        }

        Owner* get() const {
            return owner_;
        }

        Owner* operator->() const {
            return owner_;
        }

        void Reset() {
            if (gate_) {
                gate_->Leave();
                gate_.reset();
            }
            owner_ = nullptr;
        }

    private:
        friend class CallbackEpoch<Owner>;

        Lease(std::shared_ptr<CallbackEpoch<Owner>> gate, Owner* owner) : gate_(std::move(gate)), owner_(owner) {}

        std::shared_ptr<CallbackEpoch<Owner>> gate_;
        Owner* owner_ = nullptr;
    };

    static std::shared_ptr<CallbackEpoch<Owner>> Create() {
        return std::shared_ptr<CallbackEpoch<Owner>>(new CallbackEpoch<Owner>());
    }

    CallbackEpoch(const CallbackEpoch&) = delete;
    CallbackEpoch& operator=(const CallbackEpoch&) = delete;

    // Starts a new callback epoch. Any queued callback from an older epoch is
    // rejected. If a previous epoch is still active, it is stopped and drained
    // first so epochs can never overlap against the owner.
    uint64_t Begin(Owner* owner) {
        std::unique_lock<std::mutex> lock(mutex_);
        accepting_ = false;
        AdvanceEpochLocked();
        drained_.wait(lock, [this]() { return active_ == 0; });
        owner_ = owner;
        accepting_ = owner != nullptr;
        return epoch_;
    }

    Lease Enter(uint64_t expectedEpoch) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!accepting_ || !owner_ || expectedEpoch != epoch_) {
            return {};
        }
        ++active_;
        return Lease(this->shared_from_this(), owner_);
    }

    // Rejects callbacks that have not entered yet and waits for active leases.
    // The same owner may Begin() another epoch after this returns.
    void StopAndDrain() {
        std::unique_lock<std::mutex> lock(mutex_);
        accepting_ = false;
        AdvanceEpochLocked();
        drained_.wait(lock, [this]() { return active_ == 0; });
    }

    // Final owner teardown. Stale callbacks may retain this gate, but can no
    // longer obtain the detached owner pointer.
    void DetachAndDrain() {
        std::unique_lock<std::mutex> lock(mutex_);
        accepting_ = false;
        AdvanceEpochLocked();
        drained_.wait(lock, [this]() { return active_ == 0; });
        owner_ = nullptr;
    }

    uint32_t ActiveCount() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return active_;
    }

private:
    CallbackEpoch() = default;

    void Leave() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (active_ > 0) {
            --active_;
        }
        if (active_ == 0) {
            drained_.notify_all();
        }
    }

    void AdvanceEpochLocked() {
        ++epoch_;
        if (epoch_ == 0) {
            ++epoch_;
        }
    }

    mutable std::mutex mutex_;
    std::condition_variable drained_;
    Owner* owner_ = nullptr;
    uint64_t epoch_ = 0;
    uint32_t active_ = 0;
    bool accepting_ = false;
};

}  // namespace ce
