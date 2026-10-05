#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <utility>

namespace ce::dx12 {
// Internal lifecycle owner. SDK settings, runtime presence and visible FG status
// are separate observations; none of them grants callback or render admission.
class PostSLLifecycle {
public:
    class Callback {
    public:
        explicit Callback(PostSLLifecycle& owner) : owner_(owner) {
            owner_.callbacksInFlight_.fetch_add(1, std::memory_order_acq_rel);
            admitted_ = owner_.CallbacksEnabled();
        }
        ~Callback() { owner_.callbacksInFlight_.fetch_sub(1, std::memory_order_acq_rel); }
        Callback(const Callback&) = delete;
        Callback& operator=(const Callback&) = delete;
        explicit operator bool() const { return admitted_; }
    private:
        PostSLLifecycle& owner_;
        bool admitted_;
    };

    template<class Publish>
    void InstallCallback(Publish&& publish) {
        callbacksEnabled_.store(true, std::memory_order_release);
        std::forward<Publish>(publish)();
    }
    template<class Unpublish>
    void RemoveCallback(Unpublish&& unpublish) {
        SuspendCallbacks();
        std::forward<Unpublish>(unpublish)();
    }
    // Warm resume retains the already-published callback; observer-only entry
    // suspends execution without changing the shared callback pointer.
    void ResumeCallbacks() { callbacksEnabled_.store(true, std::memory_order_release); }
    void SuspendCallbacks() { callbacksEnabled_.store(false, std::memory_order_release); }
    bool CallbacksEnabled() const { return callbacksEnabled_.load(std::memory_order_acquire); }
    uint32_t CallbacksInFlight() const { return callbacksInFlight_.load(std::memory_order_acquire); }
    // These observations are independent atomic reads, not a transactional snapshot.
    uint32_t Epoch() const { return epoch_.load(std::memory_order_acquire); }

    template<class Unpublish>
    void PublishRetirement(Unpublish&& unpublish) {
        RemoveCallback(std::forward<Unpublish>(unpublish));
        InvalidateGeneration();
    }

    // Cancellation must be published before acquiring the render lock. A
    // confirmation carries its epoch so a racing old store cannot resurrect it.
    void InvalidateGeneration() { epoch_.fetch_add(1, std::memory_order_acq_rel); }
    void BeginReactivation() { confirmedGeneration_.store(0, std::memory_order_release); }
    bool ConfirmedInCurrentEpoch() const {
        return confirmedGeneration_.load(std::memory_order_acquire) == uint64_t{Epoch()} + 1;
    }
    template<class PublishProof>
    bool ConfirmRender(uint32_t entryEpoch, PublishProof&& publishProof) {
        if (entryEpoch != Epoch()) return false;
        std::forward<PublishProof>(publishProof)();
        if (entryEpoch != Epoch()) return false;
        confirmedGeneration_.store(uint64_t{entryEpoch} + 1, std::memory_order_release);
        return true;
    }
    template<class Render>
    bool RenderTransaction(Render&& render) {
        std::unique_lock<std::mutex> lock(renderMutex_, std::try_to_lock);
        if (!lock.owns_lock()) return false;
        std::forward<Render>(render)(Epoch());
        return true;
    }
    template<class Retire>
    auto FinishRetirement(Retire&& retire) {
        std::lock_guard<std::mutex> lock(renderMutex_);
        BeginReactivation();
        return std::forward<Retire>(retire)();
    }
private:
    std::atomic<uint32_t> epoch_{0};
    std::atomic<uint64_t> confirmedGeneration_{0};
    std::atomic<bool> callbacksEnabled_{false};
    std::atomic<uint32_t> callbacksInFlight_{0};
    std::mutex renderMutex_;
};
}  // namespace ce::dx12
