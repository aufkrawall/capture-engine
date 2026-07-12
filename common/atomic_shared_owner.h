#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <utility>

namespace ce {

// Atomically publishes shared ownership while preserving a pointee for the
// complete duration of each `owner->Method()` expression. This is useful for
// hot-swappable runtime services whose readers must never observe a freed
// object while a control thread replaces the active instance.
template <typename T>
class AtomicSharedOwner {
public:
    class Access {
    public:
        Access(Access&&) noexcept = default;
        Access& operator=(Access&&) noexcept = default;
        Access(const Access&) = delete;
        Access& operator=(const Access&) = delete;

        T* operator->() const {
            return value_.get();
        }

        T* get() const {
            return value_.get();
        }

        explicit operator bool() const {
            return static_cast<bool>(value_);
        }

    private:
        friend class AtomicSharedOwner<T>;

        explicit Access(const AtomicSharedOwner<T>* owner)
            : lock_(owner->accessMutex_),
              value_(std::atomic_load_explicit(&owner->value_, std::memory_order_acquire)) {}

        std::shared_lock<std::shared_mutex> lock_;
        std::shared_ptr<T> value_;
    };

    class ExclusiveAccess {
    public:
        ExclusiveAccess(ExclusiveAccess&&) noexcept = default;
        ExclusiveAccess& operator=(ExclusiveAccess&&) noexcept = default;
        ExclusiveAccess(const ExclusiveAccess&) = delete;
        ExclusiveAccess& operator=(const ExclusiveAccess&) = delete;

        T* operator->() const {
            return value_.get();
        }

        T* get() const {
            return value_.get();
        }

        explicit operator bool() const {
            return static_cast<bool>(value_);
        }

    private:
        friend class AtomicSharedOwner<T>;

        explicit ExclusiveAccess(AtomicSharedOwner<T>* owner)
            : lock_(owner->accessMutex_),
              value_(std::atomic_load_explicit(&owner->value_, std::memory_order_acquire)) {}

        std::unique_lock<std::shared_mutex> lock_;
        std::shared_ptr<T> value_;
    };

    AtomicSharedOwner() = default;
    explicit AtomicSharedOwner(std::shared_ptr<T> value) : value_(std::move(value)) {}

    AtomicSharedOwner(const AtomicSharedOwner&) = delete;
    AtomicSharedOwner& operator=(const AtomicSharedOwner&) = delete;

    std::shared_ptr<T> Load(std::memory_order order = std::memory_order_acquire) const {
        return std::atomic_load_explicit(&value_, order);
    }

    void Store(std::shared_ptr<T> value, std::memory_order order = std::memory_order_release) {
        std::unique_lock<std::shared_mutex> lock(accessMutex_);
        std::atomic_store_explicit(&value_, std::move(value), order);
    }

    std::shared_ptr<T> Exchange(std::shared_ptr<T> value, std::memory_order order = std::memory_order_acq_rel) {
        std::unique_lock<std::shared_mutex> lock(accessMutex_);
        return std::atomic_exchange_explicit(&value_, std::move(value), order);
    }

    Access operator->() const {
        return Access(this);
    }

    Access Read() const {
        return Access(this);
    }

    ExclusiveAccess LockExclusive() {
        return ExclusiveAccess(this);
    }

    explicit operator bool() const {
        return static_cast<bool>(Load());
    }

    bool operator==(std::nullptr_t) const {
        return !Load();
    }

    bool operator!=(std::nullptr_t) const {
        return static_cast<bool>(Load());
    }

private:
    mutable std::shared_mutex accessMutex_;
    mutable std::shared_ptr<T> value_;
};

}  // namespace ce
