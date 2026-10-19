// SPDX-License-Identifier: MIT
// Copyright (c) 2026 aufkrawall
#pragma once

#include "include/cengine/cengine_draft.h"

#include <atomic>
#include <cstdint>
#include <mutex>

namespace ce::capi {

// Logs a failed handle lookup of a public entry point; `failedLookups` is the running count, which
// rate-limits the output (the first few, then a periodic heartbeat).
void LogUnknownHandle(const char* entryPoint, uint64_t failedLookups);

// Which handles are live. A public entry point validates its handle here before touching it, so a
// stale, foreign or NULL handle yields CE_E_INVALID_ARGUMENT instead of a crash. There is room for one
// object (a process hosts at most one runtime), and creating is two-phase so that a second create
// during the first is refused before it constructs anything:
//
//     auto claim = registry.Claim();                 // Empty -> Claiming (CE_E_BUSY otherwise)
//     if (!claim) return claim.status();
//     ... build the object ...
//     claim.Publish(object);                         // Claiming -> Live; an unpublished claim is abandoned
//
// Retire() ends the life of the live object (destroy). A pointer is only an identity: once an object
// is retired its address may be reused by the next one, and a stale handle that happens to equal it
// is indistinguishable (documented limit of pointer handles). Calling an entry point concurrently with
// the destroy of the same handle is a client error; the registry only makes sequential misuse safe.
template <class T>
class HandleRegistry {
public:
    enum class State { Empty, Claiming, Live };

    class [[nodiscard]] Claim {
    public:
        Claim(const Claim&) = delete;
        Claim& operator=(const Claim&) = delete;
        Claim(Claim&& other) noexcept : registry_(other.registry_), status_(other.status_) {
            other.registry_ = nullptr;
        }
        ~Claim() {
            if (registry_)
                registry_->Abandon();
        }
        explicit operator bool() const noexcept {
            return status_ == CE_OK;
        }
        ce_status_t status() const noexcept {
            return status_;
        }
        // Makes `object` the live handle. false (and the claim is abandoned) for NULL.
        bool Publish(T* object) noexcept {
            if (!registry_)
                return false;
            HandleRegistry* registry = registry_;
            registry_ = nullptr;
            return registry->PublishClaimed(object);
        }

    private:
        friend class HandleRegistry;
        Claim(HandleRegistry* registry, ce_status_t status) noexcept
            : registry_(status == CE_OK ? registry : nullptr),
              status_(status) {}
        HandleRegistry* registry_;
        ce_status_t status_;
    };

    HandleRegistry() = default;
    HandleRegistry(const HandleRegistry&) = delete;
    HandleRegistry& operator=(const HandleRegistry&) = delete;

    Claim TryClaim() noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        if (state_ != State::Empty)
            return Claim(this, CE_E_BUSY);
        state_ = State::Claiming;
        return Claim(this, CE_OK);
    }

    // The live object when `handle` is exactly it, else nullptr. Each miss is counted.
    T* Find(const T* handle) noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        if (state_ == State::Live && handle != nullptr && handle == live_)
            return live_;
        misses_.fetch_add(1, std::memory_order_relaxed);
        return nullptr;
    }

    // Live -> Empty. CE_E_INVALID_ARGUMENT when `handle` is not the live object (second destroy, stale).
    ce_status_t Retire(const T* handle) noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        if (state_ != State::Live || handle == nullptr || handle != live_) {
            misses_.fetch_add(1, std::memory_order_relaxed);
            return CE_E_INVALID_ARGUMENT;
        }
        live_ = nullptr;
        state_ = State::Empty;
        return CE_OK;
    }

    State GetState() const noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        return state_;
    }
    // Objects published so far; a diagnostic ("runtime #2").
    uint64_t Generation() const noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        return generation_;
    }
    // Lookups that found no live object, for rate-limited logging by the caller.
    uint64_t Misses() const noexcept {
        return misses_.load(std::memory_order_relaxed);
    }

private:
    bool PublishClaimed(T* object) noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        if (state_ != State::Claiming)
            return false;
        if (object == nullptr) {
            state_ = State::Empty;
            return false;
        }
        live_ = object;
        state_ = State::Live;
        ++generation_;
        return true;
    }
    void Abandon() noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        if (state_ == State::Claiming)
            state_ = State::Empty;
    }

    mutable std::mutex mutex_;
    State state_ = State::Empty;
    T* live_ = nullptr;
    uint64_t generation_ = 0;
    std::atomic<uint64_t> misses_{0};
};

// The process-wide registry of runtime handles: the documented singleton of the C ABI shim.
inline HandleRegistry<ce_runtime>& LiveRuntimes() noexcept {
    static HandleRegistry<ce_runtime> registry;
    return registry;
}

// The entry points' one line: the live runtime behind `handle`, or nullptr (rate-limited log line).
inline ce_runtime* ResolveRuntime(const ce_runtime* handle, const char* entryPoint) noexcept {
    ce_runtime* runtime = LiveRuntimes().Find(handle);
    if (!runtime)
        LogUnknownHandle(entryPoint, LiveRuntimes().Misses());
    return runtime;
}

}  // namespace ce::capi
