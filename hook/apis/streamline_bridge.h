#pragma once

// The Streamline generation bridge: a CE-owned Streamline 2.x runtime driving a game that
// shipped 1.x, so DLSS-G / multi-frame generation becomes reachable in titles that would
// otherwise be stuck on Streamline 1.x.
//
// Policy (what may activate, what each call translates to) lives in
// streamline_bridge_policy.h and is unit-tested. This header is the runtime half: loading
// the 2.x runtime, initialising it, and taking the game's import slots over.
namespace ce::streamline_bridge {

// Decides whether this process should be bridged and, if so, brings the 2.x runtime up and
// repoints the game's sl.interposer imports. Idempotent, and a no-op unless
// `streamline_upgrade` is on. Must run before the game drives its own Streamline: the
// decision refuses rather than half-switching once it is too late.
void TryActivate();

// True once the bridge owns this process's Streamline surface. While it does, the ordinary
// `streamline_dll_path` substitution must stand down for the whole sl.* family - both want
// the same configured folder, for opposite purposes.
bool IsActive();

}  // namespace ce::streamline_bridge
