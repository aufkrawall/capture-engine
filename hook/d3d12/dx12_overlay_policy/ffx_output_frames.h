#pragma once

// Which game frame an output of AMD's frame generation swapchain shows, and who drew the overlay into it.
//
// AMD composes a frame's outputs (generated, then real) on its presenter thread while the game already runs the next
// frame. The proxy Present(N) first waits until every output of the frames before N is composed (FidelityFX SDK
// 1.1.4 FrameInterpolationSwapChainDX12::Present: waitForFenceValue(compositionFenceCPU,
// previousFramesSentForPresentation)), then copies the double-buffered UI resource and dispatches N's interpolation
// on the game thread, and only then schedules N. CE's proxy prework for N runs before that wait, so the presenter can
// still be composing frame N-1 when prework N retires the UI baseline: deciding by "after prework N" blended frame
// N-1's remaining outputs twice and judged one of them uncovered (dx12_fg_switch_test, session 20261003_120641,
// 12:07:04.937). The composing frame therefore advances inside AMD's Present - at its first submission on the game
// thread, which follows the wait and precedes the scheduling - or, failing that, when it returns.
//
// The overlay travels with its frame on every route the prework draws: the proxy backbuffer, AMD's copy of the
// UI resource, a UI texture the game keeps per frame (alternating textures) and CE's alternating substitute. A
// single texture AMD reads while the game rewrites it is the exception: there the UI baseline never hands over
// (dx12_hook_ffx_proxy_present.cpp), so its frames are all baseline-owned.

#include <array>
#include <atomic>
#include <cstdint>

namespace ce::dx12_overlay_policy {

// Who put the overlay into one frame's outputs (decided by that frame's proxy prework).
enum class FFXFrameOverlayOwner : uint8_t {
    kUnknown = 0,  // no prework record: judged the frame-agnostic way
    kNone,         // the prework drew nothing (failed composite): only a topmost draw can cover it
    kBaseline,     // drawn into the frame's own copy of the UI resource or into its proxy backbuffer
    kTopmost,      // the UI baseline is retired for this frame; the final-batch route draws its outputs
};

struct FFXFrameOwnerRecord {
    FFXFrameOverlayOwner owner = FFXFrameOverlayOwner::kUnknown;
};

inline const char* FFXFrameOverlayOwnerName(FFXFrameOverlayOwner owner) {
    switch (owner) {
        case FFXFrameOverlayOwner::kNone:
            return "none";
        case FFXFrameOverlayOwner::kBaseline:
            return "ui-baseline";
        case FFXFrameOverlayOwner::kTopmost:
            return "topmost";
        default:
            return "unknown";
    }
}

// The owners of the most recent frames; the prework writes on the game thread, the presenter reads. AMD keeps at most
// two frames in flight, so a small ring never loses a frame that still has outputs to compose.
class FFXFrameOwnerRing {
public:
    void Record(uint64_t frame, FFXFrameOwnerRecord record) {
        if (frame == 0) {
            return;
        }
        slots_[frame % slots_.size()].store(Pack(frame, record), std::memory_order_release);
    }

    FFXFrameOwnerRecord Lookup(uint64_t frame) const {
        if (frame == 0) {
            return {};
        }
        const uint64_t packed = slots_[frame % slots_.size()].load(std::memory_order_acquire);
        if ((packed >> 8) != frame) {
            return {};
        }
        FFXFrameOwnerRecord record;
        record.owner = static_cast<FFXFrameOverlayOwner>(packed & 0xFFu);
        return record;
    }

    void Clear() {
        for (auto& slot : slots_) {
            slot.store(0, std::memory_order_release);
        }
    }

private:
    static uint64_t Pack(uint64_t frame, FFXFrameOwnerRecord record) {
        return (frame << 8) | static_cast<uint64_t>(record.owner);
    }

    std::array<std::atomic<uint64_t>, 16> slots_ = {};
};

// The newest frame AMD may be composing. Advances monotonically; a frame's outputs read it at their final batch.
class FFXComposingFrame {
public:
    // Returns true when this call advanced it.
    bool AdvanceTo(uint64_t frame) {
        uint64_t current = frame_.load(std::memory_order_acquire);
        while (current < frame) {
            if (frame_.compare_exchange_weak(current, frame, std::memory_order_acq_rel, std::memory_order_acquire)) {
                return true;
            }
        }
        return false;
    }

    uint64_t Current() const { return frame_.load(std::memory_order_acquire); }

private:
    std::atomic<uint64_t> frame_{0};
};

// Whether the final-batch route draws on an output of a frame with `record`: exactly when its prework retired the
// baseline for it, so the frames still composing with the baseline get only the marker. A frame without a prework
// record keeps the frame-agnostic ownership grant.
inline bool ShouldDrawTopmostOnFFXOutput(FFXFrameOwnerRecord record, bool ownershipGranted) {
    if (record.owner != FFXFrameOverlayOwner::kUnknown) {
        return record.owner == FFXFrameOverlayOwner::kTopmost;
    }
    return ownershipGranted;
}

struct FFXOutputVerdict {
    bool covered = false;
    bool doubleDrawn = false;
};

// An output of a recorded frame: covered by its own topmost draw or its frame's baseline - never both.
inline FFXOutputVerdict JudgeFrameExactFFXOutput(FFXFrameOverlayOwner owner, bool topmostDrawn) {
    const bool baseline = owner == FFXFrameOverlayOwner::kBaseline;
    FFXOutputVerdict verdict;
    verdict.covered = baseline || topmostDrawn;
    verdict.doubleDrawn = baseline && topmostDrawn;
    return verdict;
}

}  // namespace ce::dx12_overlay_policy
