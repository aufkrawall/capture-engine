#pragma once

#include <condition_variable>
#include <mutex>

namespace ce::flow {

// Completion also wakes a waiter when the callback was never reached.
class CallBarrier {
public:
    void Enter() {
        std::unique_lock lock(mutex_);
        entered_ = true;
        changed_.notify_all();
        changed_.wait(lock, [&] { return released_; });
    }
    bool WaitUntilEntered() {
        std::unique_lock lock(mutex_);
        changed_.wait(lock, [&] { return entered_ || completed_; });
        return entered_;
    }
    void Release() {
        std::lock_guard lock(mutex_);
        released_ = true;
        changed_.notify_all();
    }
    void Complete() {
        std::lock_guard lock(mutex_);
        completed_ = true;
        changed_.notify_all();
    }

private:
    std::mutex mutex_;
    std::condition_variable changed_;
    bool entered_ = false;
    bool released_ = false;
    bool completed_ = false;
};

}  // namespace ce::flow
