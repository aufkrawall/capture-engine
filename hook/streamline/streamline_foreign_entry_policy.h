#pragma once

// Which Streamline export hooks may leave an entry another component already patched, and how a hook
// slot treats an export it left. Header-only so the unit tests drive it directly.

namespace ce::streamline_runtime_policy {

// A core export hook leaves a foreign-owned entry alone only where CE still has another route to the
// export's callers: the GetProcAddress-time hook and the import-slot patches. Rewriting the foreign
// patch is not neutral - a tool that verifies its patch (OptiScaler fingerprints each
// slGetPluginFunction it detours) reads CE's rewrite as a freshly loaded image and detours again on
// top of CE, while CE's trampoline still leads into that tool's handler: the two call each other until
// the stack overflows (Talos + OptiScaler, 20261023_173151). While the interposer takeover owns the
// import slots, the entry hook is CE's only view of the game's statically imported calls, so it keeps
// chaining there.
inline bool ShouldLeaveForeignOwnedStreamlineEntry(bool exportHasCallerSideRoute, bool takeoverOwnsImportSlots) {
    return exportHasCallerSideRoute && !takeoverOwnsImportSlots;
}

enum class StreamlineHookSlotRequest {
    kInstall,              // nothing claims the slot for another live target: install on this one
    kAlreadyHandled,       // CE already hooked this target, or already left it to its owner
    kRefuseSecondInstance, // another live target owns the slot's single forward pointer
};

// `slotTarget` is the export the slot recorded: one CE hooked, or one CE left to the component that
// had already patched its entry. A left export keeps its target without an installed hook, so unload
// invalidation and the reload self-heal still attribute it to its module - and it still owns the
// slot's one process-global original: a second live instance of the export must not take that over,
// exactly as with an installed hook (Cyberpunk 20260816_045933). A recorded target whose module is
// gone is an ordinary unload/reload generation.
inline StreamlineHookSlotRequest ResolveStreamlineHookSlotRequest(const void* slotTarget, const void* requestedTarget,
                                                                  bool slotTargetStillLoaded) {
    if (slotTarget && slotTarget == requestedTarget) {
        return StreamlineHookSlotRequest::kAlreadyHandled;
    }
    if (slotTarget && slotTargetStillLoaded) {
        return StreamlineHookSlotRequest::kRefuseSecondInstance;
    }
    return StreamlineHookSlotRequest::kInstall;
}

}  // namespace ce::streamline_runtime_policy
