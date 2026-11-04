#pragma once

namespace ce::dx12_overlay_policy {

// Whether an ExecuteCommandLists whose immediate caller is a third-party overlay is the overlay's own submission.
//
// With CE at this queue's live method entry, application dispatch reaches CE before any overlay below it, so an
// overlay caller is submitting its own work. If another entry sits above CE, that overlay can be forwarding the
// application's call: count it. Inspect this invocation's queue, never earlier calls or module generations. An
// already-loaded overlay can install a hook later, and two queues need not have the same chain.
inline bool IsOverlayOwnedEclSubmission(bool immediateCallerIsOverlay, const void* currentQueueEntry,
                                        const void* ceEntry) {
    return immediateCallerIsOverlay && ceEntry && currentQueueEntry == ceEntry;
}

}  // namespace ce::dx12_overlay_policy
