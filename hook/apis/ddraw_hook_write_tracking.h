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

// The access paired with an Unlock attempt. See
// ce::ddraw_present_policy::CompleteSurfaceLockTrack for why every Unlock
// attempt resolves the surface's whole track.
using DirectDrawLockAccess = ce::ddraw_present_policy::SurfaceLockAccess;

void BeginDirectDrawSurfaceLock(IUnknown* surface, bool writable);
DirectDrawLockAccess CompleteDirectDrawSurfaceLock(IUnknown* surface, bool unlockSucceeded);

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
