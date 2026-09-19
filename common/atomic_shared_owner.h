#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <utility>

namespace ce {

namespace detail {

constexpr size_t kMaxTrackedSharedOwners = 8;

struct SharedOwnerThreadEntry {
    const void* owner = nullptr;
    uint32_t depth = 0;
};

inline SharedOwnerThreadEntry* GetSharedOwnerThreadEntries() {
    thread_local SharedOwnerThreadEntry entries[kMaxTrackedSharedOwners] = {};
    return entries;
}

inline uint32_t AcquireSharedReadDepth(const void* owner) {
    auto* entries = GetSharedOwnerThreadEntries();
    for (size_t i = 0; i < kMaxTrackedSharedOwners; ++i) {
        if (entries[i].owner == owner) {
            return entries[i].depth++;
        }
    }
    for (size_t i = 0; i < kMaxTrackedSharedOwners; ++i) {
        if (entries[i].owner == nullptr) {
            entries[i].owner = owner;
            entries[i].depth = 1;
            return 0;
        }
    }
    return 0;
}

inline uint32_t ReleaseSharedReadDepth(const void* owner) {
    auto* entries = GetSharedOwnerThreadEntries();
    for (size_t i = 0; i < kMaxTrackedSharedOwners; ++i) {
        if (entries[i].owner == owner) {
            if (entries[i].depth > 0) {
                --entries[i].depth;
                const uint32_t remaining = entries[i].depth;
                if (remaining == 0) {
                    entries[i].owner = nullptr;
                }
                return remaining;
            }
            return 0;
        }
    }
    return 0;
}

}  // namespace detail

// Atomically publishes shared ownership while preserving a pointee for the
// complete duration of each `owner->Method()` expression. This is useful for
// hot-swappable runtime services whose readers must never observe a freed
// object while a control thread replaces the active instance.
template <typename T>
class AtomicSharedOwner {
public:
    class Access {
    public:
        Access(Access&& other) noexcept
            : owner_(other.owner_),
              lockedMutex_(other.lockedMutex_),
              value_(std::move(other.value_)) {
            other.owner_ = nullptr;
            other.lockedMutex_ = false;
        }

        Access& operator=(Access&& other) noexcept {
            if (this != &other) {
                cleanup();
                owner_ = other.owner_;
                lockedMutex_ = other.lockedMutex_;
                value_ = std::move(other.value_);
                other.owner_ = nullptr;
                other.lockedMutex_ = false;
            }
            return *this;
        }

        Access(const Access&) = delete;
        Access& operator=(const Access&) = delete;

        ~Access() {
            cleanup();
        }

        T* operator->() const {
            return value_.get();
        }

        T* get() const {
            return value_.get();
        }

        const std::shared_ptr<T>& Shared() const {
            return value_;
        }

        explicit operator bool() const {
            return static_cast<bool>(value_);
        }

    private:
        friend class AtomicSharedOwner<T>;

        explicit Access(const AtomicSharedOwner<T>* owner)
            : owner_(owner),
              value_(std::atomic_load_explicit(&owner->value_, std::memory_order_acquire)) {
            if (owner_ && detail::AcquireSharedReadDepth(owner_) == 0) {
                owner_->accessMutex_.lock_shared();
                lockedMutex_ = true;
            }
        }

        void cleanup() noexcept {
            if (owner_) {
                detail::ReleaseSharedReadDepth(owner_);
                if (lockedMutex_) {
                    owner_->accessMutex_.unlock_shared();
                }
                owner_ = nullptr;
                lockedMutex_ = false;
            }
        }

        const AtomicSharedOwner<T>* owner_ = nullptr;
        bool lockedMutex_ = false;
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

        const std::shared_ptr<T>& Shared() const {
            return value_;
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
