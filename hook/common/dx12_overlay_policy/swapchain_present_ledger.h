#pragma once

// Per-swapchain overlay visibility ledger for FG / swapchain handoff diagnostics.
//
// The coverage tracker answers "was this present covered"; it cannot say WHICH
// image reached the screen without the overlay when a frame-generation switch
// replaces the swapchain. A switch shows the departing chain's final image for as
// long as the application takes to present on the replacement (300-350 ms per
// switch in the DX12 switch test app), so an overlay missing from exactly that
// final image is a visible dropout even though every later present is covered.
//
// The ledger keeps one record per swapchain lifetime (a creation re-arms the
// record even when DXGI reuses the address) and reports a handoff on the first
// present of a new lifetime: the departing chain's last present state, how many
// presents in a row it ended without the overlay, the arriving chain's first
// present state and the time nothing was presented in between. Diagnostic only;
// no rendering decision reads it.

#include <cstddef>
#include <cstdint>

namespace ce::dx12_overlay_policy {

enum class PresentOverlayState : uint8_t {
    kMissing = 0,    // no CE overlay draw belongs to this present
    kInherited = 1,  // no draw, but the FG runtime composes the last drawn overlay
    kDrawn = 2,      // a CE overlay draw was submitted for this present
};

inline const char* PresentOverlayStateName(PresentOverlayState state) {
    switch (state) {
        case PresentOverlayState::kDrawn:
            return "drawn";
        case PresentOverlayState::kInherited:
            return "inherited";
        default:
            return "MISSING";
    }
}

inline PresentOverlayState ClassifyPresentOverlayState(bool drawObserved, bool coveredByTracker) {
    if (drawObserved) {
        return PresentOverlayState::kDrawn;
    }
    return coveredByTracker ? PresentOverlayState::kInherited : PresentOverlayState::kMissing;
}

struct SwapchainPresentRecord {
    uintptr_t swapchain = 0;
    uint64_t lifetime = 0;  // ledger-assigned ordinal of the observed creation (0 = creation not observed)
    uint64_t createdUs = 0;
    uint64_t presents = 0;
    uint64_t firstPresentUs = 0;
    uint64_t lastPresentUs = 0;
    PresentOverlayState firstState = PresentOverlayState::kMissing;
    PresentOverlayState lastState = PresentOverlayState::kMissing;
    uint32_t firstRouteMask = 0;
    uint32_t lastRouteMask = 0;
    uint64_t missingPresents = 0;
    uint64_t tailMissing = 0;             // consecutive MISSING presents ending at the last present
    uint64_t presentsBeforeFirstOverlay = 0;
    uint64_t firstOverlayUs = 0;          // first drawn/inherited present (0 = none yet)
};

struct SwapchainPresentEvent {
    // First present of a new swapchain lifetime after another chain presented.
    bool handoff = false;
    // The arriving chain showed the overlay for the first time after N MISSING presents.
    bool overlayArrivedLate = false;
    SwapchainPresentRecord departing;
    SwapchainPresentRecord current;
    uint64_t noPresentGapUs = 0;
};

class SwapchainPresentLedger {
public:
    static constexpr size_t kCapacity = 8;

    void NoteCreated(uintptr_t swapchain, uint64_t nowUs) {
        if (!swapchain) {
            return;
        }
        Slot& slot = Acquire(swapchain);
        slot.record = SwapchainPresentRecord{};
        slot.record.swapchain = swapchain;
        slot.record.lifetime = ++lifetimeCounter_;
        slot.record.createdUs = nowUs;
        slot.lastTouchUs = nowUs;
    }

    SwapchainPresentEvent NotePresent(uintptr_t swapchain, PresentOverlayState state, uint32_t routeMask,
                                      uint64_t nowUs) {
        SwapchainPresentEvent event;
        if (!swapchain) {
            return event;
        }
        Slot& slot = Acquire(swapchain);
        SwapchainPresentRecord& record = slot.record;
        const bool firstPresent = record.presents == 0;
        if (firstPresent) {
            record.firstPresentUs = nowUs;
            record.firstState = state;
            record.firstRouteMask = routeMask;
            event.handoff = lastValid_ && !(last_.swapchain == swapchain && last_.lifetime == record.lifetime);
            if (event.handoff) {
                event.departing = last_;
                event.noPresentGapUs = nowUs >= last_.lastPresentUs ? nowUs - last_.lastPresentUs : 0;
            }
        }
        ++record.presents;
        record.lastPresentUs = nowUs;
        record.lastState = state;
        record.lastRouteMask = routeMask;
        slot.lastTouchUs = nowUs;
        if (state == PresentOverlayState::kMissing) {
            ++record.missingPresents;
            ++record.tailMissing;
        } else {
            record.tailMissing = 0;
            if (record.firstOverlayUs == 0) {
                record.firstOverlayUs = nowUs;
                record.presentsBeforeFirstOverlay = record.presents - 1;
                event.overlayArrivedLate = record.presentsBeforeFirstOverlay > 0;
            }
        }
        event.current = record;
        last_ = record;
        lastValid_ = true;
        return event;
    }

    uint64_t Lifetimes() const {
        return lifetimeCounter_;
    }

private:
    struct Slot {
        SwapchainPresentRecord record;
        uint64_t lastTouchUs = 0;
    };

    // A swapchain never observed being created gets an implicit lifetime (0).
    Slot& Acquire(uintptr_t swapchain) {
        Slot* oldest = &slots_[0];
        for (Slot& slot : slots_) {
            if (slot.record.swapchain == swapchain) {
                return slot;
            }
            if (slot.lastTouchUs < oldest->lastTouchUs) {
                oldest = &slot;
            }
        }
        for (Slot& slot : slots_) {
            if (slot.record.swapchain == 0) {
                slot.record.swapchain = swapchain;
                return slot;
            }
        }
        // Evict the least recently touched record; the departing chain survives
        // in last_ even when its slot is reused.
        *oldest = Slot{};
        oldest->record.swapchain = swapchain;
        return *oldest;
    }

    Slot slots_[kCapacity] = {};
    SwapchainPresentRecord last_;
    bool lastValid_ = false;
    uint64_t lifetimeCounter_ = 0;
};

}  // namespace ce::dx12_overlay_policy
