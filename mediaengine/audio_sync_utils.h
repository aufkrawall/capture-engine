#pragma once

// Audio sync helpers: resampling ratios, drift accounting, and track timeline math.
//
// This file is the umbrella; the topic headers below hold the code.

#include "audio_sync/timeline_constants.h"
#include "audio_sync/packet_clamp_and_drift.h"
#include "audio_sync/source_silence_policy.h"
