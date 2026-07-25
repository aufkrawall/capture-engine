#pragma once

// DX12 overlay policy: the decision logic behind Post-SL/FFX overlay routing, barrier
// modes, queue selection, and FG transition handling.
//
// Header-only and inline by design so the present path pays no call overhead. This
// file is the umbrella; the topic headers below hold the code.

#include "dx12_overlay_policy/overlay_submission.h"
#include "dx12_overlay_policy/ffx_routing.h"
#include "dx12_overlay_policy/fg_metrics_and_transitions.h"
#include "dx12_overlay_policy/streamline_ownership.h"
#include "dx12_overlay_policy/postsl_queue_selection.h"
#include "dx12_overlay_policy/postsl_keepalive.h"
#include "dx12_overlay_policy/protected_ffx_startup.h"
