#include "ddraw_hook_internal.h"

// Which surface pixels the application may have changed since CE last
// composited there.
//
// The composite restores a saved backdrop when it knows the application has not
// touched the region, and reads the surface when it has. Without this proof the
// only safe answer is "the application wrote everything", which is what an
// unregistered surface, an unknown lock rectangle or an overflowed tracker
// reports.
//
// Only surfaces that can actually be a composite target are tracked: a loading
// screen creates and locks hundreds of texture surfaces, and letting those fill
// the tracker would evict the primary's mark and turn every later composite
// into a full-surface read.

namespace {

namespace policy = ce::ddraw_present_policy;

constexpr size_t kMaxTrackedSurfaces = 8;
constexpr size_t kMaxWriteMarks = 16;
constexpr size_t kMaxActiveLocks = 16;

struct SurfaceIdentity {
    uintptr_t identity = 0;
    void* raw = nullptr;
};

SurfaceIdentity g_trackedSurfaces[kMaxTrackedSurfaces];
size_t g_trackedSurfaceCount = 0;
bool g_trackAnySurface = false;

struct WriteMark {
    uintptr_t identity = 0;
    policy::Rect rect = {};
    bool whole = false;
};

WriteMark g_writeMarks[kMaxWriteMarks];

struct ActiveLock {
    uintptr_t identity = 0;
    policy::SurfaceLockTrack track;
};

ActiveLock g_activeLocks[kMaxActiveLocks];

// A tracker that has ever overflowed cannot answer "nothing was written" for
// any surface again; the conservative answer costs one read per composite,
// while a wrong "clean" answer leaves a stale composite on screen.
bool g_writeTrackingOverflowed = false;

bool IsSurfaceTrackedLocked(void* surface, uintptr_t identity) {
    if (g_trackAnySurface)
        return true;
    for (size_t i = 0; i < g_trackedSurfaceCount; ++i) {
        if (g_trackedSurfaces[i].raw == surface || (identity != 0 && g_trackedSurfaces[i].identity == identity))
            return true;
    }
    return false;
}

WriteMark* FindMarkLocked(uintptr_t identity) {
    for (WriteMark& mark : g_writeMarks) {
        if (mark.whole || !mark.rect.IsEmpty()) {
            if (mark.identity == identity)
                return &mark;
        }
    }
    return nullptr;
}

WriteMark* FindOrCreateMarkLocked(uintptr_t identity) {
    if (WriteMark* existing = FindMarkLocked(identity))
        return existing;
    for (WriteMark& mark : g_writeMarks) {
        if (!mark.whole && mark.rect.IsEmpty()) {
            mark.identity = identity;
            return &mark;
        }
    }
    g_writeTrackingOverflowed = true;
    return nullptr;
}

void MarkWholeSurfaceLocked(uintptr_t identity) {
    WriteMark* mark = FindOrCreateMarkLocked(identity);
    if (mark)
        mark->whole = true;
}

void ClearMarkLocked(WriteMark& mark) {
    mark.identity = 0;
    mark.rect = {};
    mark.whole = false;
}

ActiveLock* FindActiveLockLocked(uintptr_t identity) {
    for (ActiveLock& active : g_activeLocks) {
        if (active.track.depth != 0 && active.identity == identity)
            return &active;
    }
    return nullptr;
}

}  // namespace

void BeginDirectDrawSurfaceLock(IUnknown* surface, bool writable) {
    if (!surface)
        return;
    const uintptr_t identity = DirectDrawObjectIdentity(surface);
    std::lock_guard<std::mutex> lock(ddraw_hook_g_DDrawIdentityMutex);
    ActiveLock* active = FindActiveLockLocked(identity);
    if (!active) {
        for (ActiveLock& candidate : g_activeLocks) {
            if (candidate.track.depth == 0) {
                active = &candidate;
                active->identity = identity;
                break;
            }
        }
    }
    if (!active) {
        return;
    }
    policy::BeginSurfaceLockTrack(active->track, writable);
}

DirectDrawLockAccess CompleteDirectDrawSurfaceLock(IUnknown* surface, bool unlockSucceeded) {
    if (!surface)
        return DirectDrawLockAccess::Unknown;
    const uintptr_t identity = DirectDrawObjectIdentity(surface);
    std::lock_guard<std::mutex> lock(ddraw_hook_g_DDrawIdentityMutex);
    ActiveLock* active = FindActiveLockLocked(identity);
    if (!active)
        return DirectDrawLockAccess::Unknown;
    return policy::CompleteSurfaceLockTrack(active->track, unlockSucceeded);
}

void RegisterDirectDrawCompositeSurface(IUnknown* surface) {
    if (!surface)
        return;
    const uintptr_t identity = DirectDrawObjectIdentity(surface);
    std::lock_guard<std::mutex> lock(ddraw_hook_g_DDrawIdentityMutex);
    if (g_trackAnySurface)
        return;
    for (size_t i = 0; i < g_trackedSurfaceCount; ++i) {
        if (g_trackedSurfaces[i].raw == surface || (identity != 0 && g_trackedSurfaces[i].identity == identity))
            return;
    }
    if (g_trackedSurfaceCount >= kMaxTrackedSurfaces) {
       // More distinct composite targets than any DirectDraw flip chain should
        // have. Track everything from here; the tracker stays conservative.
        g_trackAnySurface = true;
        return;
    }
    g_trackedSurfaces[g_trackedSurfaceCount].identity = identity;
    g_trackedSurfaces[g_trackedSurfaceCount].raw = surface;
    ++g_trackedSurfaceCount;
}

void MarkDirectDrawSurfaceWrite(IUnknown* surface, const RECT* rect, bool rectIsExact) {
    if (!surface || ddraw_hook_g_CaptureRecurse != 0)
        return;

    const uintptr_t identity = DirectDrawObjectIdentity(surface);
    std::lock_guard<std::mutex> lock(ddraw_hook_g_DDrawIdentityMutex);
    if (!IsSurfaceTrackedLocked(surface, identity))
        return;

    WriteMark* mark = FindOrCreateMarkLocked(identity);
    if (!mark)
        return;

    if (!rectIsExact || !rect) {
        mark->whole = true;
        ddraw_hook_g_PresentationDiagnostics.applicationWritesMarked.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    const policy::Rect written{rect->left, rect->top, rect->right, rect->bottom};
    if (written.IsEmpty())
        return;
    mark->rect = policy::UnionRect(mark->rect, written);
    ddraw_hook_g_PresentationDiagnostics.applicationWritesMarked.fetch_add(1, std::memory_order_relaxed);
}

bool DirectDrawSurfaceHasPendingWrite(IUnknown* surface) {
    if (!surface)
        return false;
    const uintptr_t identity = DirectDrawObjectIdentity(surface);
    std::lock_guard<std::mutex> lock(ddraw_hook_g_DDrawIdentityMutex);
    if (g_writeTrackingOverflowed)
        return true;
    return FindMarkLocked(identity) != nullptr;
}

DirectDrawWriteRegion ConsumeDirectDrawSurfaceWrites(IUnknown* surface, policy::Rect& out) {
    out = {};
    if (!surface)
        return DirectDrawWriteRegion::Whole;
    const uintptr_t identity = DirectDrawObjectIdentity(surface);
    std::lock_guard<std::mutex> lock(ddraw_hook_g_DDrawIdentityMutex);
    if (g_writeTrackingOverflowed)
        return DirectDrawWriteRegion::Whole;
    WriteMark* mark = FindMarkLocked(identity);
    if (!mark)
        return DirectDrawWriteRegion::None;
    const bool whole = mark->whole;
    out = mark->rect;
    ClearMarkLocked(*mark);
    if (whole)
        return DirectDrawWriteRegion::Whole;
    return out.IsEmpty() ? DirectDrawWriteRegion::None : DirectDrawWriteRegion::Partial;
}

void ClearDirectDrawSurfaceWrites(IUnknown* surface) {
    if (!surface)
        return;
    const uintptr_t identity = DirectDrawObjectIdentity(surface);
    std::lock_guard<std::mutex> lock(ddraw_hook_g_DDrawIdentityMutex);
    if (WriteMark* mark = FindMarkLocked(identity))
        ClearMarkLocked(*mark);
}

void RequeueDirectDrawSurfaceWrite(IUnknown* surface) {
    if (!surface)
        return;
    const uintptr_t identity = DirectDrawObjectIdentity(surface);
    std::lock_guard<std::mutex> lock(ddraw_hook_g_DDrawIdentityMutex);
    MarkWholeSurfaceLocked(identity);
}

void ResetDirectDrawSurfaceWrites() {
    std::lock_guard<std::mutex> lock(ddraw_hook_g_DDrawIdentityMutex);
    for (SurfaceIdentity& surface : g_trackedSurfaces)
        surface = {};
    for (WriteMark& mark : g_writeMarks)
        ClearMarkLocked(mark);
    for (ActiveLock& active : g_activeLocks)
        active = {};
    g_trackedSurfaceCount = 0;
    g_trackAnySurface = false;
    g_writeTrackingOverflowed = false;
}
