#pragma once

namespace ce::vtable_hook_policy {

inline bool ShouldReclaimRestoredSlot(const void* current, const void* detour, const void* predecessor) {
    return predecessor && current != detour && current == predecessor;
}

inline bool ShouldPreserveForeignFollower(const void* current, const void* detour, const void* predecessor) {
    return predecessor && current != detour && current != predecessor;
}

// Whether the pointer CE is about to keep as "the original" is code from
// outside the module that owns the vtable - that is, another injector's detour
// rather than the implementation the vtable shipped with.
//
// This is the precondition for the cycle that froze Gothic II in session
// 20260916_011148: CE and Steam's gameoverlayrenderer each hooked
// IDirectDrawSurface7's Flip slot, and installed in that order each one's
// saved original was the other's detour. Nothing here prevents the second
// installer from doing that - CE does not control another process-wide
// injector - but CE knows at its own install time that it is chaining into
// foreign code, which is the one fact the recursion itself cannot report. The
// caller logs it, with the module named, so a future occurrence is attributable
// without a dump of the hung process.
//
// A vtable that does not live inside a module image (a wrapper object's
// heap-allocated vtable) has no owning module to compare against, so nothing is
// claimed for it.
inline bool SavedOriginalIsForeignChain(const void* entryModule, const void* vtableModule, const void* selfModule) {
    return entryModule != nullptr && vtableModule != nullptr && entryModule != vtableModule &&
           entryModule != selfModule;
}

}  // namespace ce::vtable_hook_policy
