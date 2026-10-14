#pragma once

#include <atomic>

namespace ce::streamline_interposer_takeover {

// Decide ownership before entering the loader, without taking an application
// lock. A resident-core notification may decline pending takeover, but must
// never undo an entry thunk that already selected the custom interposer.
class TakeoverDecision {
public:
    bool TryClaimCustom(bool entryThunk) {
        Route expected = Route::Pending;
        if (entryThunk && route_.compare_exchange_strong(expected, Route::Custom, std::memory_order_acq_rel))
            return true;
        return IsCustom();
    }

    bool TrySelectResident() {
        Route expected = Route::Pending;
        return route_.compare_exchange_strong(expected, Route::Resident, std::memory_order_acq_rel);
    }

    // Only a final LoadAndPublish failure may release a custom claim. Its
    // publication arbiter already guarantees that no peer can publish success.
    void SelectResidentAfterLoadFailure() { route_.store(Route::Resident, std::memory_order_release); }

    bool IsPending() const { return route_.load(std::memory_order_acquire) == Route::Pending; }
    bool IsCustom() const { return route_.load(std::memory_order_acquire) == Route::Custom; }
    bool IsResident() const { return route_.load(std::memory_order_acquire) == Route::Resident; }
    bool AllowsCustomLoad(bool armed, bool loadInProgress) const {
        return !IsResident() && (armed || loadInProgress);
    }

private:
    enum class Route { Pending, Custom, Resident };
    static_assert(std::atomic<Route>::is_always_lock_free);
    std::atomic<Route> route_{Route::Pending};
};

// Loader callbacks only publish a request. The hook worker owns import
// restoration and hook installation, including their application locks.
class TakeoverRollbackRequest {
public:
    void Request(const char* reason) { reason_.store(reason, std::memory_order_release); }

    template <typename Restore>
    bool Service(Restore restore) {
        const char* reason = reason_.exchange(nullptr, std::memory_order_acq_rel);
        if (!reason)
            return false;
        restore(reason);
        return true;
    }

private:
    static_assert(std::atomic<const char*>::is_always_lock_free);
    std::atomic<const char*> reason_{nullptr}; // Static reason strings only.
};

}  // namespace ce::streamline_interposer_takeover
