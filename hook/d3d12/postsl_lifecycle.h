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
            epoch_ = owner_.Epoch();
            admitted_ = owner_.CallbacksEnabled();
        }
        ~Callback() { owner_.callbacksInFlight_.fetch_sub(1, std::memory_order_acq_rel); }
        Callback(const Callback&) = delete;
        Callback& operator=(const Callback&) = delete;
        explicit operator bool() const { return admitted_; }
        uint32_t Epoch() const { return epoch_; }
    private:
        PostSLLifecycle& owner_;
        bool admitted_;
        uint32_t epoch_;
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
    void InvalidateGeneration() { epoch_.fetch_add(1, std::memory_order_acq_rel); BeginReactivation(); }
    void BeginReactivation() {
        UpdateRoute([](uint64_t route) { return ((route & ~kFlags) + kRevision) | (route & (kFlags & ~kEpochConfirmed)); });
    }
    bool ConfirmedInCurrentEpoch() const {
        return (route_.load(std::memory_order_acquire) & kEpochConfirmed) != 0 &&
               confirmedGeneration_.load(std::memory_order_acquire) == uint64_t{Epoch()} + 1;
    }
    // Route activation, synthetic startup and retained route proof are distinct
    // from callback admission and current-epoch proof. One atomic word keeps
    // cancellation revision and its flags together, without extra reader loads.
    bool RouteActive() const { return (route_.load(std::memory_order_acquire) & kActive) != 0; }
    bool RouteConfirmed() const { return (route_.load(std::memory_order_acquire) & kConfirmed) != 0; }
    bool SyntheticProbeUnconfirmed() const { return (route_.load(std::memory_order_acquire) & kSynthetic) != 0; }
    void ActivateRoute(bool preserveSynthetic = true) {
        UpdateRoute([&](uint64_t route) { return (route | kActive) & (preserveSynthetic ? ~uint64_t{0} : ~kSynthetic); });
    }
    void ActivateSyntheticProbe() { route_.fetch_or(kActive | kSynthetic, std::memory_order_acq_rel); }
    // Temporary route suspension preserves proven ownership for make-before-break.
    void SuspendRoute() { route_.fetch_and(~kActive, std::memory_order_acq_rel); }
    void ResetStartupEvidence() { route_.fetch_and(~kSynthetic, std::memory_order_acq_rel); }
    void InvalidateRouteProof() {
        UpdateRoute([](uint64_t route) { return ((route & ~kFlags) + kRevision) | (route & (kActive | kSynthetic)); });
    }
    void RestartRoute() { UpdateRoute([](uint64_t route) { return (route & ~kFlags) + kRevision; }); }

    struct RenderConfirmation {
        bool accepted = false;
        bool firstRouteProof = false;
        operator bool() const { return accepted; }
    };
    template<class PublishProof>
    RenderConfirmation ConfirmRender(uint32_t entryEpoch, PublishProof&& publishProof) {
        if (entryEpoch != Epoch()) return {};
        uint64_t route = route_.load(std::memory_order_acquire);
        const uint64_t revision = route & ~kFlags;
        std::forward<PublishProof>(publishProof)();
        if (entryEpoch != Epoch()) return {};
        route = route_.load(std::memory_order_acquire);
        for (;;) {
            if ((route & ~kFlags) != revision) return {};
            const uint64_t confirmed = (route | kConfirmed | kEpochConfirmed) & ~kSynthetic;
            if (confirmed == route || route_.compare_exchange_weak(route, confirmed,
                    std::memory_order_acq_rel, std::memory_order_acquire)) break;
        }
        confirmedGeneration_.store(uint64_t{entryEpoch} + 1, std::memory_order_release);
        return {true, (route & kConfirmed) == 0};
    }
    template<class Render>
    bool RenderTransaction(uint32_t admissionEpoch, Render&& render) {
        std::unique_lock<std::mutex> lock(renderMutex_, std::try_to_lock);
        if (!lock.owns_lock() || admissionEpoch != Epoch()) return false;
        std::forward<Render>(render)(admissionEpoch);
        return true;
    }
    template<class Retire>
    auto FinishRetirement(Retire&& retire) {
        std::lock_guard<std::mutex> lock(renderMutex_);
        BeginReactivation();
        return std::forward<Retire>(retire)();
    }
private:
    static constexpr uint64_t kActive = 1, kSynthetic = 2, kConfirmed = 4, kEpochConfirmed = 8, kFlags = 15, kRevision = 16;
    template<class Update>
    void UpdateRoute(Update&& update) {
        uint64_t route = route_.load(std::memory_order_acquire);
        while (!route_.compare_exchange_weak(route, update(route), std::memory_order_acq_rel,
                                             std::memory_order_acquire)) {}
    }
    std::atomic<uint64_t> route_{0};
    std::atomic<uint32_t> epoch_{0};
    std::atomic<uint64_t> confirmedGeneration_{0};
    std::atomic<bool> callbacksEnabled_{false};
    std::atomic<uint32_t> callbacksInFlight_{0};
    std::mutex renderMutex_;
};
}  // namespace ce::dx12
