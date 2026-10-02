#pragma once

// Shared capture-pipeline policy: the decision logic behind CFR scheduling, WGC/DXGI
// source health, smoothness buffering, and A/V timeline contracts.
//
// Header-only and inline by design so the capture loops pay no call overhead, and so
// the logic stays reachable from tests/test_capture_pipeline_policy.cpp without a
// link dependency. This file is the umbrella; the topic headers below hold the code.

#include "common/capture/capture_policy/constants.h"
#include "common/capture/capture_policy/cfr_scheduling.h"
#include "common/capture/capture_policy/cfr_overload_recovery.h"
#include "common/capture/capture_policy/cfr_startup.h"
#include "common/capture/capture_policy/encoder_priority_and_routing.h"
#include "common/capture/capture_policy/recording_health.h"
#include "common/capture/capture_policy/source_state.h"
#include "common/capture/capture_policy/live_debt.h"
#include "common/capture/capture_policy/smoothness_budget.h"
#include "common/capture/capture_policy/ingress_and_active_delay.h"
#include "common/capture/capture_policy/cfr_repeat_metrics.h"
#include "common/capture/capture_policy/cfr_nearest_playout.h"
#include "common/capture/capture_policy/final_output_timing.h"
#include "common/capture/capture_policy/inject_lineage.h"
