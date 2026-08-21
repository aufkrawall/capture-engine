#pragma once

// The Streamline generation bridge: a CE-owned Streamline 2.x runtime driving a game that
// shipped 1.x, so DLSS-G / multi-frame generation becomes reachable in titles that would
// otherwise be stuck on Streamline 1.x.
//
// Policy (what may activate, what each call translates to) lives in
// streamline_bridge_policy.h and is unit-tested. This header is the runtime half: loading
// the 2.x runtime, initialising it, and taking the game's import slots over.
namespace ce::streamline_bridge {

// Decides whether this process should be bridged and, if so, repoints the game's
// sl.interposer imports, then brings the 2.x runtime up behind them. Idempotent, and a
// no-op unless `streamline_upgrade` is on.
//
// Call it as early as a loaded config allows. It does not have to beat the game's own
// `slInit` - that is recoverable, and the runtime CE arrives in has usually run it already -
// but everything it does before device creation is free, and everything after is not.
void TryActivate();

// Hands the game's D3D12 device to the bridged 2.x runtime, from whichever route found it
// first. A no-op unless the bridge is active, and idempotent.
//
// The bridge's own `D3D12CreateDevice` slot is the direct route, but it is not the only one
// a title can take: a game using the Agility SDK creates its device through
// `ID3D12DeviceFactory::CreateDevice`, which no `sl.interposer` export covers, and CE's DX12
// hook then discovers the device from a command queue instead. Streamline 2.x needs the
// device either way - without it, most of its exports jump through an unbound plugin pointer
// - so the bridge takes it from whichever route sees it first rather than assuming one.
void NotifyD3D12Device(void* device);

// True once the bridge owns this process's Streamline surface. While it does, the ordinary
// `streamline_dll_path` substitution must stand down for the whole sl.* family - both want
// the same configured folder, for opposite purposes.
bool IsActive();

}  // namespace ce::streamline_bridge
