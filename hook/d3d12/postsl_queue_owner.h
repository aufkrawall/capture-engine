#pragma once

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <utility>
#include <vector>

namespace ce::dx12 {
// Private lifetime transaction adapter; production uses the real COM queue and fence.
// Observations borrow identity only. Existing command-queue locking or an admitted
// callback lease protects dereference; reading several identities is not a snapshot.
template<class Queue, class Fence>
class PostSLQueueOwner {
public:
    class RetiredReference {
    public:
        RetiredReference() = default;
        explicit RetiredReference(Queue* value) : value_(value) {}
        ~RetiredReference() { Release(); }
        RetiredReference(const RetiredReference&) = delete;
        RetiredReference& operator=(const RetiredReference&) = delete;
        RetiredReference(RetiredReference&& other) noexcept : value_(std::exchange(other.value_, nullptr)) {}
        RetiredReference& operator=(RetiredReference&& other) noexcept {
            if (this != &other) { Release(); value_ = std::exchange(other.value_, nullptr); }
            return *this;
        }
        Queue* Borrow() const { return value_; }
        void Release() { if (Queue* old = std::exchange(value_, nullptr)) old->Release(); }
    private:
        Queue* value_ = nullptr;
    };
    explicit PostSLQueueOwner(std::recursive_mutex& queueMutex) : mutex_(queueMutex) {}
    // Global owner is explicitly shut down by the hook, never by loader-lock destruction.
    Queue* SelectedQueue() const { return selected_.load(std::memory_order_acquire); }
    Queue* DedicatedQueue() const { return dedicated_.load(std::memory_order_acquire); }
    Queue* LastDeviceHealthyQueue() const { return healthy_.load(std::memory_order_acquire); }
    Queue* PinnedWrapperQueue() const { return pinned_.load(std::memory_order_acquire); }

    RetiredReference ReplaceSelection(Queue* next) {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        if (next == SelectedQueue()) return {};
        if (next) next->AddRef();
        return RetiredReference(selected_.exchange(next, std::memory_order_acq_rel));
    }
    void PinWrapperForEpoch(Queue* wrapper) {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        if (pinned_.load(std::memory_order_acquire) || !wrapper) return;
        wrapper->AddRef();
        pinned_.store(wrapper, std::memory_order_release);
    }
    void RememberDeviceHealthySubmission(Queue* queue) {
        RetiredReference old;
        {
            std::lock_guard<std::recursive_mutex> lock(mutex_);
            if (queue == LastDeviceHealthyQueue()) return;
            if (queue) queue->AddRef();
            old = RetiredReference(healthy_.exchange(queue, std::memory_order_acq_rel));
        }
    }
    RetiredReference DetachPinnedWrapper() {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        Queue* pinned = pinned_.exchange(nullptr, std::memory_order_acq_rel);
        if (pinned && !GpuCompleteLocked()) {
            retired_.push_back(pinned);
            workPending_.store(true, std::memory_order_release);
            return {};
        }
        return RetiredReference(pinned);
    }
    template<class Release>
    bool ClearSelection(Release&& release) {
        Queue* selected;
        Queue* dedicated;
        {
            std::lock_guard<std::recursive_mutex> lock(mutex_);
            selected = selected_.exchange(nullptr, std::memory_order_acq_rel);
            dedicated = dedicated_.exchange(nullptr, std::memory_order_acq_rel);
            pending_.store(false, std::memory_order_release);
            if (!GpuCompleteLocked()) {
                workPending_.store(true, std::memory_order_release);
                if (selected) retired_.push_back(selected);
                if (dedicated) retired_.push_back(dedicated);
                return false;
            }
        }
        if (selected) release(selected, "locked");
        if (dedicated) release(dedicated, "dedicated");
        return true;
    }
    // Low-frequency retirement captures completion evidence. A fence stays alive
    // even if overlay cleanup replaces it. Repeated evidence only extends its target.
    void RecordRetirementWork(Fence* fence, uint64_t value) {
        if (!fence || !value) return;
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        for (auto& work : work_) {
            if (work.fence == fence) { work.value = std::max(work.value, value); return; }
        }
        workPending_.store(true, std::memory_order_release);
        work_.push_back({fence, value});
        fence->AddRef();
    }
    // An admitted callback can outlast the bounded drain. Its existing fence
    // publication extends pending retirement evidence, with no normal-path COM
    // call or allocation. It must not allow an earlier completed value to retire
    // queues still used by that callback's final GPU submission.
    void ObserveRetiringSubmission(Fence* fence, uint64_t value) {
        if (!workPending_.load(std::memory_order_acquire) && !RetirementPending()) return;
        RecordRetirementWork(fence, value);
    }
    void DeferSelectionRetirement() { pending_.store(true, std::memory_order_release); }
    bool RetirementPending() const { return pending_.load(std::memory_order_acquire); }

    template<class Release>
    bool CleanupDeferred(bool runtimeActive, uint32_t callbacks, Release&& release) {
        // This is the established next-pass release point for an already-retired
        // locked queue. Its GPU completion was proven before it entered this slot.
        if (Queue* previous = deferred_.exchange(nullptr, std::memory_order_acq_rel))
            release(previous, "deferred locked");
        if (!workPending_.load(std::memory_order_acquire) &&
            (runtimeActive || !RetirementPending())) return false;
        std::vector<Queue*> retired;
        Queue* dedicated = nullptr;
        bool detached = false;
        {
            std::lock_guard<std::recursive_mutex> lock(mutex_);
            if (callbacks || !GpuCompleteLocked()) return false;
            retired.swap(retired_);
            for (const auto& work : work_) work.fence->Release();
            work_.clear();
            workPending_.store(false, std::memory_order_release);
            if (!runtimeActive && pending_.exchange(false, std::memory_order_acq_rel)) {
                Queue* selected = selected_.exchange(nullptr, std::memory_order_acq_rel);
                dedicated = dedicated_.exchange(nullptr, std::memory_order_acq_rel);
                if (selected) {
                    if (Queue* previous = deferred_.exchange(selected, std::memory_order_acq_rel))
                        retired.push_back(previous);
                }
                detached = true;
            }
        }
        for (Queue* queue : retired) release(queue, "retired");
        if (dedicated) release(dedicated, "dedicated");
        return detached;
    }
    template<class Release>
    void Shutdown(Release&& release) {
        std::vector<Queue*> retired;
        {
            std::lock_guard<std::recursive_mutex> lock(mutex_);
            for (auto* slot : {&selected_, &dedicated_, &pinned_, &healthy_, &deferred_}) {
                if (Queue* queue = slot->exchange(nullptr, std::memory_order_acq_rel)) retired_.push_back(queue);
            }
            retired.swap(retired_);
            for (const auto& work : work_) work.fence->Release();
            work_.clear();
            workPending_.store(false, std::memory_order_release);
            pending_.store(false, std::memory_order_release);
        }
        for (Queue* queue : retired) release(queue, "shutdown");
    }
private:
    struct Work { Fence* fence; uint64_t value; };
    bool GpuCompleteLocked() const {
        for (const auto& work : work_) if (work.fence->GetCompletedValue() < work.value) return false;
        return true;
    }
    std::recursive_mutex& mutex_;
    std::atomic<Queue*> selected_{nullptr}, dedicated_{nullptr}, healthy_{nullptr}, pinned_{nullptr};
    std::atomic<Queue*> deferred_{nullptr};
    std::atomic<bool> pending_{false};
    std::atomic<bool> workPending_{false};
    std::vector<Work> work_;
    std::vector<Queue*> retired_;
};
}  // namespace ce::dx12
