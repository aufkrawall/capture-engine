/**
 * DirectDraw surface write/access tracking shared by the detours and the CPU
 * overlay composite.
 */

#pragma once

#include <windows.h>

#include <unknwn.h>

#include "../common/ddraw_present_policy.h"

// What the application may have written into a surface CE is about to
// composite into. `Whole` is the conservative answer whenever the exact
// rectangle is unknown or the tracker could not retain an exact mark.
enum class DirectDrawWriteRegion { None, Partial, Whole };

// The access paired with a successful Unlock. DirectDraw normally permits one
// lock at a time, but Deferred keeps nested/overlapping locks conservative and
// presents only after the final one is released.
enum class DirectDrawLockAccess { Unknown, Deferred, ReadOnly, Writable };

void BeginDirectDrawSurfaceLock(IUnknown* surface, bool writable);
DirectDrawLockAccess CompleteDirectDrawSurfaceLock(IUnknown* surface);

void MarkDirectDrawSurfaceWrite(IUnknown* surface, const RECT* rect, bool rectIsExact);
bool DirectDrawSurfaceHasPendingWrite(IUnknown* surface);
DirectDrawWriteRegion ConsumeDirectDrawSurfaceWrites(IUnknown* surface,
                                                      ce::ddraw_present_policy::Rect& out);
void ClearDirectDrawSurfaceWrites(IUnknown* surface);

// Requeues a conservative whole-surface mark after an internal composite
// failure. Unlike the application-facing marker, this remains active while the
// presentation recursion guard is set.
void RequeueDirectDrawSurfaceWrite(IUnknown* surface);

// Drops tracked surfaces, active locks and marks at a target/lifecycle boundary.
void ResetDirectDrawSurfaceWrites();

// Only composite targets retain write history; texture churn must not evict a
// scanout surface's mark.
void RegisterDirectDrawCompositeSurface(IUnknown* surface);
