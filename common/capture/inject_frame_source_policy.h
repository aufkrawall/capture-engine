#pragma once

#include <stdint.h>

// Which inject frame-ring slots media may admit for the current capture session.
//
// Every inject slot names the process that wrote it (FrameSlot::sourcePid), and
// media duplicates the slot's texture and fence handles out of exactly that
// process. The session owner is the process CE injected into and publishes its
// own PID once (SharedMemoryLayout::SetSourcePid); a slot written by anyone else
// is normally stale or foreign and must not make media open an unrelated process.
//
// Split renderers are the one legitimate exception. A 32-bit D3D9 client can hand
// its frames to a 64-bit child that owns the Vulkan device and WSI swapchain
// (Portal RTX: hl2.exe -> NvRemixBridge.exe). The CE Vulkan layer in that child
// captures the final output and writes the slots with its own PID, because the
// handles live in its handle table, while sourcePid stays with the profiled
// parent. The layer proves that topology before it participates (it is only
// admitted as the direct child of the published source/profile target) and
// publishes it as CaptureState::vulkanLayerClaim = {renderer, client}. Frames
// from that renderer belong to the session exactly when the claim names the
// session source as its client.
//
// Keep this header free of Windows and shared-memory dependencies: callers
// decode the claim (ce::vulkan_layer_claim) and pass both halves in.
namespace ce::inject_frame_source {

enum class Admission : uint8_t {
    // The slot was written by the session owner, or one side carries no PID yet.
    kSessionSource,
    // The slot was written by the renderer the Vulkan ownership claim assigns
    // to the session source. Callers still confirm the renderer is a live direct
    // child of the session source before admitting it.
    kSplitRendererCandidate,
    // Any other writer: drop the frame.
    kForeign,
};

inline Admission Classify(uint32_t slotSourcePid, uint32_t sessionSourcePid, uint32_t claimRendererPid,
                          uint32_t claimClientPid) {
    if (sessionSourcePid == 0 || slotSourcePid == 0 || slotSourcePid == sessionSourcePid)
        return Admission::kSessionSource;
    // A direct renderer publishes itself in both halves; that is not a split and
    // cannot vouch for a different writer.
    if (claimRendererPid != 0 && claimRendererPid != claimClientPid && claimRendererPid == slotSourcePid &&
        claimClientPid == sessionSourcePid) {
        return Admission::kSplitRendererCandidate;
    }
    return Admission::kForeign;
}

inline const char* AdmissionName(Admission admission) {
    switch (admission) {
        case Admission::kSessionSource:
            return "session-source";
        case Admission::kSplitRendererCandidate:
            return "split-renderer";
        case Admission::kForeign:
            return "foreign";
    }
    return "unknown";
}

// Result of asking the OS whether a renderer is a live direct child of a client.
enum class ParentCheck : uint8_t {
    kDirectChild,
    kNotDirectChild,  // includes "the renderer is gone"
    kUnavailable,     // the process snapshot itself failed; say nothing about the pair
};

// Remembers the outcome of the (comparatively expensive) process-tree check for
// one renderer/client pair, so the per-frame path only compares PIDs. A new
// renderer or session source is a new pair and is verified again; a PID pair
// never changes meaning while both processes stay alive. An unavailable answer
// is not cached, so a transient snapshot failure cannot latch a rejection for
// the rest of the recording.
class SplitRendererVerificationCache {
public:
    struct Verdict {
        bool admitted = false;
        bool ranCheck = false;  // this call ran `check`; log the outcome once
        ParentCheck check = ParentCheck::kUnavailable;
    };

    // `check(renderer, client)` returns a ParentCheck.
    template <typename Check>
    Verdict Verify(uint32_t rendererPid, uint32_t clientPid, Check&& check) {
        Verdict verdict;
        if (rendererPid == 0 || clientPid == 0 || rendererPid == clientPid)
            return verdict;
        if (!hasVerdict_ || rendererPid_ != rendererPid || clientPid_ != clientPid) {
            verdict.ranCheck = true;
            verdict.check = check(rendererPid, clientPid);
            if (verdict.check == ParentCheck::kUnavailable) {
                hasVerdict_ = false;
                return verdict;
            }
            rendererPid_ = rendererPid;
            clientPid_ = clientPid;
            verified_ = verdict.check == ParentCheck::kDirectChild;
            hasVerdict_ = true;
        } else {
            verdict.check = verified_ ? ParentCheck::kDirectChild : ParentCheck::kNotDirectChild;
        }
        verdict.admitted = verified_;
        return verdict;
    }

private:
    uint32_t rendererPid_ = 0;
    uint32_t clientPid_ = 0;
    bool verified_ = false;
    bool hasVerdict_ = false;
};

}  // namespace ce::inject_frame_source
