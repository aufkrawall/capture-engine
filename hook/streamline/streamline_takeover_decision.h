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

    bool IsCustom() const { return route_.load(std::memory_order_acquire) == Route::Custom; }
    bool IsResident() const { return route_.load(std::memory_order_acquire) == Route::Resident; }

private:
    enum class Route { Pending, Custom, Resident };
    std::atomic<Route> route_{Route::Pending};
};

}  // namespace ce::streamline_interposer_takeover
