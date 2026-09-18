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

}  // namespace ce::streamline_ota
