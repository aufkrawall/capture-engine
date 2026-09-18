#pragma once

/*
 * See streamline_ota_preferences.cpp for why clearing Streamline's own OTA
 * preference bits is a separate mechanism from refusing the NGX updater launch,
 * and for the three guards that keep reaching into the game's `slInit`
 * arguments safe.
 */

namespace ce::streamline_ota {

// Installs CE's `slInit` route, when the process runs Streamline 2.x and the
// resolved profile asks for `ngx_ota=off`. A no-op in every other case,
// including an unknown or 1.x generation.
//
// Call this as early as a config exists. It resolves the generation itself from
// the loaded interposer's file version rather than waiting to be told, because
// the only useful moment is before the game's first `slInit` and CE's own
// hook-time classification lands after it.
//
// Safe to call repeatedly: it installs once, and retries while the interposer
// has not been mapped yet.
void InstallSlInitRouteIfConfigured();

// Whether the game's `slInit` has actually come through CE's route, and whether
// the route was ever installed.
//
// These exist because "installed" and "effective" turned out to be different
// things and nothing in the logs could tell them apart. Session 20260918_223542
// installed the route at 22:35:49.085 with the IAT patched, and the runtime
// still resolved its core from the driver's OTA store at 22:35:49.249 - with no
// log line either way, because the hook only reported when it actually cleared
// bits. Four different causes produced that same silence.
//
// The caller that matters is the foreign-core observation: pairing it with
// these answers separates "the game called slInit before CE was there" from
// "CE's route missed the call" from "CE saw it and the flags were already
// clear", which need completely different responses.
bool WasSlInitObserved();
bool WasSlInitRouteInstalled();

}  // namespace ce::streamline_ota
