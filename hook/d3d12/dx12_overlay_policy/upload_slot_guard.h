#pragma once

#include <d3d12.h>
#include <wrl/client.h>

#include <array>
#include <cstddef>
#include <cstdint>

// Pinning the fence whose lifetime the per-slot upload-ring guards are keyed
// to.
//
// The descriptor-free and textured DX12 overlay backends round-robin through
// small pools of persistently-mapped UPLOAD vertex/index buffers. Each slot
// records the absolute overlay-fence value that this frame's overlay work
// will signal; before reusing a slot the CPU waits for that value. Absolute
// values are only meaningful while the fence object they were recorded
// against is still the overlay fence.
//
// Overlay reinit (InitOverlaySync) releases the old fence and creates a new
// one, and the new object can be allocated at the SAME address as the
// released old one (ABA reuse). A raw-pointer comparison then fails to
// detect the replacement, stale guards survive, and every subsequent wait
// blocks for the full liveness timeout against an unreachable value. The
// observed fallout (session 20260813_173453, DLSS-FG switch after FG-mode
// switching spam): every present stalls ~1 s, the game drops to ~1 FPS, and
// the overlay draw is skipped on every frame.
//
// Pinning the bound fence with an owning reference makes the comparison
// sound: while we hold a reference the old fence cannot be destroyed, so a
// replacement fence can never reuse its address, and any pointer change
// provably belongs to a new fence lifetime. The caller clears its per-slot
// guard values whenever RebindIfNeeded() reports a change.
namespace ce::dx12_overlay_policy {

// The overlay command allocator and persistently mapped VB/IB pools share one
// lifetime domain. Keeping the count in one place lets a renderer bind upload
// slot N to allocator N, whose fence completion was already proved before the
// allocator was reset. An independent, smaller upload ring can wrap while all
// allocator slots are still legitimately in flight and overwrite GPU reads.
inline constexpr int kAllocatorCoupledUploadSlotCount = 16;

inline constexpr int ResolveAllocatorCoupledUploadSlot(int allocatorSlot) {
    return allocatorSlot >= 0 && allocatorSlot < kAllocatorCoupledUploadSlotCount ? allocatorSlot : -1;
}

inline constexpr bool CanUseFenceGuardedUploadSlotFallback(bool hasFence, uint64_t publishedGuardValue) {
    return hasFence && publishedGuardValue != 0;
}

// Which allocator and upload slot the overlay's next draw takes. Rotating through all 16 made every slot write the
// whole text once after each text change, and the text changes every 16-50 frames: on 32-bit D3D12, whose text is
// a megabyte of solid spans, that was about 340 KB per frame into write-combined memory (Mirror's Edge,
// 20261104_133147). The GPU is rarely more than a frame or two behind, so taking the lowest finished slot keeps two
// or three slots in use, and their allocators warm.
//
// Finished means proved. A slot's fence value only proves its last submission if the value was assigned after that
// submission: a route that submits without a Signal leaves an old value behind, and rotation only got away with that
// because the slot came around again 16 frames later. Such a slot stays out until a Signal covers it, so those routes
// fall back to plain rotation, exactly what they had before.
class OverlaySlotPicker {
public:
    // Before the list reaches the queue. `currentFenceValue`: the value Signals have reached so far; the next one
    // this slot gets is larger.
    void NoteSubmitted(int slot, uint64_t currentFenceValue) {
        if (slot >= 0 && slot < kAllocatorCoupledUploadSlotCount) {
            submitted_[static_cast<size_t>(slot)] = true;
            submittedAt_[static_cast<size_t>(slot)] = currentFenceValue;
        }
    }

    // The first finished slot from `firstSlot` on, or -1.
    int Pick(const uint64_t* fenceValues, int slotCount, int firstSlot, uint64_t completedFenceValue) const {
        if (!fenceValues || slotCount <= 0 || slotCount > kAllocatorCoupledUploadSlotCount || firstSlot < 0) {
            return -1;
        }
        for (int offset = 0; offset < slotCount; ++offset) {
            const int slot = (firstSlot + offset) % slotCount;
            if (IsFinished(slot, fenceValues[slot], completedFenceValue)) {
                return slot;
            }
        }
        return -1;
    }

    // The fence and allocators were replaced: every value recorded so far belongs to them.
    void Reset() {
        submitted_ = {};
        submittedAt_ = {};
    }

private:
    bool IsFinished(int slot, uint64_t fenceValue, uint64_t completedFenceValue) const {
        const auto index = static_cast<size_t>(slot);
        if (fenceValue > completedFenceValue) {
            return false;
        }
        return !submitted_[index] || fenceValue > submittedAt_[index];
    }

    std::array<bool, kAllocatorCoupledUploadSlotCount> submitted_{};
    std::array<uint64_t, kAllocatorCoupledUploadSlotCount> submittedAt_{};
};

// The lowest finished slot is only for the plain route: under frame generation or a runtime-owned swapchain the
// overlay's slots may have no Signal, and a failed Signal leaves nothing to reason from.
inline constexpr bool ShouldReuseFinishedOverlaySlot(bool hasFence, bool frameGenerationActive, bool runtimeOwnedPresentation,
                                                     bool gpuCompletionUnprovable) {
    return hasFence && !frameGenerationActive && !runtimeOwnedPresentation && !gpuCompletionUnprovable;
}

class UploadSlotGuardFenceBinding {
public:
    // Pins `publishedFence` (AddRef) and releases the previously pinned
    // fence. Returns true when the published fence differs from the
    // previously pinned one, meaning the fence lifetime changed and the
    // caller must clear its per-slot guard values. Republishing the same
    // fence returns false and keeps existing guards valid.
    bool RebindIfNeeded(ID3D12Fence* publishedFence) {
        if (publishedFence == boundFence_.Get()) {
            return false;
        }
        // WRL assignment pins the new fence and releases the old one.
        boundFence_ = publishedFence;
        return true;
    }

    // The pinned fence, valid for GetCompletedValue/SetEventOnCompletion for
    // as long as this binding lives. Null until the first RebindIfNeeded.
    ID3D12Fence* GetFence() const { return boundFence_.Get(); }

    void Reset() { boundFence_.Reset(); }

    // Process-termination fast path: hand ownership to the OS instead of
    // touching a potentially torn-down driver. Returns the released fence.
    ID3D12Fence* Detach() { return boundFence_.Detach(); }

private:
    Microsoft::WRL::ComPtr<ID3D12Fence> boundFence_;
};

}  // namespace ce::dx12_overlay_policy
