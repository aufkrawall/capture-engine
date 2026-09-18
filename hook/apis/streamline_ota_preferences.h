#pragma once

/*
 * See streamline_ota_preferences.cpp for why clearing Streamline's own OTA
 * preference bits is a separate mechanism from refusing the NGX updater launch,
 * and for the three guards that keep reaching into the game's `slInit`
 * arguments safe.
 */

#include "../common/streamline_api_generation.h"

namespace ce::streamline_ota {

// Registers CE's `slInit` route, once, when the process runs Streamline 2.x and
// the resolved profile asks for `ngx_ota=off`. A no-op in every other case,
// including an unknown or 1.x generation.
void RegisterDynamicHookOnce(ce::streamline_api::Generation generation);

}  // namespace ce::streamline_ota
