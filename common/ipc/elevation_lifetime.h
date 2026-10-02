#pragma once

#include <mutex>
#include <cstdint>

namespace ce::elevation {
// Once the final admitted client releases its resources, this process cannot
// accept a new generation. SCM starts a fresh process for subsequent controllers.
class ClientLifetime {
public:
    bool Admit() {
        std::lock_guard<std::mutex> guard(mutex_);
        if (stopping_)
            return false;
        ++clients_;
        return true;
    }
    bool Release() {
        std::lock_guard<std::mutex> guard(mutex_);
        if (!clients_)
            return false;
        if (--clients_)
            return false;
        stopping_ = true;
        return true;
    }
    uint32_t Count() const {
        std::lock_guard<std::mutex> guard(mutex_);
        return clients_;
    }

private:
    mutable std::mutex mutex_;
    uint32_t clients_ = 0;
    bool stopping_ = false;
};
}  // namespace ce::elevation
