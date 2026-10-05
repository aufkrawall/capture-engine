#pragma once

#include <mutex>
#include <utility>

namespace ce::dx12 {
// A normal present owns the overlay lock through recording, submission and
// capture. PostSL drains outside it to preserve render -> overlay lock order.
template<class Mutex>
class FrameRenderAdmission {
public:
    bool TryAcquire(Mutex& mutex) {
        lock_ = std::unique_lock<Mutex>(mutex, std::try_to_lock);
        return lock_.owns_lock();
    }

    template<class RouteCheck>
    bool RouteOutsideLock(RouteCheck&& check) {
        lock_.unlock();
        if (std::forward<RouteCheck>(check)()) return true;
        lock_.lock();
        return false;
    }

    template<class Retirement>
    decltype(auto) RetireOutsideLock(Retirement&& retire) {
        lock_.unlock();
        struct Reacquire {
            std::unique_lock<Mutex>& lock;
            ~Reacquire() { lock.lock(); }
        } reacquire{lock_};
        return std::forward<Retirement>(retire)();
    }
private:
    std::unique_lock<Mutex> lock_;
};
}  // namespace ce::dx12
