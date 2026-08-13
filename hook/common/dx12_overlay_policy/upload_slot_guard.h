#pragma once

#include <d3d12.h>
#include <wrl/client.h>

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
