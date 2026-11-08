#pragma once

// Third-party overlay coexistence: detecting Steam/Rockstar/EOS overlays and
// adapting our hook install order and draw routing around them.
//
// This file is the umbrella; the topic headers below hold the code.

#include "hook/overlay/overlay_compat_detail/module_table.h"
#include "hook/overlay/overlay_compat_detail/caller_verdict_cache.h"
#include "hook/overlay/overlay_compat_detail/runtime_module_inspect.h"
#include "hook/overlay/overlay_compat_detail/routing_policy.h"
